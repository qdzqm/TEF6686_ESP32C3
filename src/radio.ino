#include "TEF6686.h"
#include "Wire.h"
#include <Arduino.h>
#include "StateStore.h"
#include "ILI9341_LTSM.hpp"

// 状态存储负载长度 = EEPROM 记录数据区字节数(见 StateStore.h)
#define STATE_PAYLOAD_LEN REC_DATA_LEN

#include "fonts_LTSM/FontRetro_LTSM.hpp"
#include "fonts_LTSM/FontDefault_LTSM.hpp"
#include "fonts_LTSM/FontSevenSeg_LTSM.hpp"
#include "fonts_LTSM/FontPico_LTSM.hpp"
#include "fonts_LTSM/FontSixteenSeg_LTSM.hpp"
#include "fonts_LTSM/FontArialBold_LTSM.hpp"
#include "fonts_LTSM/FontGroTesk_LTSM.hpp"
#include "fonts_LTSM/FontMega_LTSM.hpp"
#include "fonts_LTSM/FontArialRound_LTSM.hpp"
#include "fonts_LTSM/FontHallfetica_LTSM.hpp"

// 硬件引脚定义
#define ENCODER_PIN_A 3
#define ENCODER_PIN_B 1
#define BUTTON_PIN 2

#define TFT_RST 20
#define TFT_DC 21
#define TFT_CS 7

// 频率范围常量
#define FM_MIN_FREQ 8750   // 87.5 MHz
#define FM_MAX_FREQ 10800  // 108.0 MHz
#define AM_MIN_FREQ 531   // 531 KHz
#define AM_MAX_FREQ 1710  // 1710 KHz
#define SW_MIN_FREQ 2300   // 2300 KHz
#define SW_MAX_FREQ 27000  // 27 MHz

/* ==================== EEPROM 延时保存配置 ====================
 * 手动调台 / 搜台落台后, 频率需连续稳定满下面设定的时长才会写入 EEPROM
 * (避免频繁旋钮导致反复擦写磨损)。可根据实际体验自行调整, 单位: 毫秒。
 * 波段切换/步进切换/双击等明确操作不受此延时影响, 仍立即保存。 */
#define STATE_SAVE_DELAY_MANUAL_MS  20000UL   // 手动调台: 稳定后保存的延时(ms)
#define STATE_SAVE_DELAY_SEEK_MS    20000UL   // 搜台落台: 稳定后保存的延时(ms)
#define STATE_SAVE_DELAY_THEME_MS   10000UL    // 屏幕配色: 选定该配色并停留满此时长后保存(ms)

// 显示尺寸常量
const uint16_t DISPLAY_WIDTH = 320;   // 旋转270度后的宽度
const uint16_t DISPLAY_HEIGHT = 240;  // 旋转270度后的高度

// ==================== 屏幕布局区域 (320x240, 四个圆角色块区域) ====================
// 字体尺寸: 频率数字 FontSixteenSeg 32x48 / 顶部标签 FontHallfetica 16x16 / 底栏与状态文字 FontDefault 8x8
// 纵向: 顶部波段栏36 + 间隙4 + 频率面板128 + 间隙4 + 状态栏28 + 间隙4 + 底部模式栏36 = 240
const uint16_t TOPBAR_X = 2, TOPBAR_Y = 2, TOPBAR_W = 316, TOPBAR_H = 34;                // 顶部波段栏(FM/MW/SW)
const uint16_t FREQPANEL_X = 4, FREQPANEL_Y = 40, FREQPANEL_W = 312, FREQPANEL_H = 128;  // 频率显示面板
const uint16_t STATUSBAR_X = 2, STATUSBAR_Y = 172, STATUSBAR_W = 316, STATUSBAR_H = 28; // 状态栏(立体声 + 信号强度)
const uint16_t BOTBAR_X = 2, BOTBAR_Y = 204, BOTBAR_W = 316, BOTBAR_H = 34;              // 底部模式栏(SEEK/TUNE/步进)
const uint16_t PANEL_RADIUS_BAR = 10;                                                    // 三个横栏色块圆角半径
const uint16_t PANEL_RADIUS_FREQ = 12;                                                   // 频率面板色块圆角半径
const uint16_t FREQ_START_X = 88;                                                        // 频率数字原点x(按5字符宽144居中于面板)
const uint16_t FREQ_START_Y = FREQPANEL_Y + (FREQPANEL_H - 48) / 2;                      // =80, 数字高48在面板内垂直居中
const uint16_t FREQ_UNIT_X = FREQPANEL_X + FREQPANEL_W - 8 - 3 * 8;                      // =284, 单位文字3字符宽24, 右缘留8
const uint16_t FREQ_UNIT_Y = FREQ_START_Y + 48 - 8;                                      // =120, 单位文字与数字底部对齐
const uint16_t STATUS_TEXT_X = 8;                                                        // MONO/STEREO 原点x
const uint16_t STATUS_TEXT_Y = STATUSBAR_Y + (STATUSBAR_H - 8) / 2;                      // =182, 8px文字在状态栏内垂直居中
const uint16_t SIGNAL_X = DISPLAY_WIDTH - 8 - 38;                                        // =274, 信号条5格宽38, 右缘留8
const uint16_t SIGNAL_Y = STATUSBAR_Y + (STATUSBAR_H - 20) / 2;                          // =176, 信号条高20垂直居中

// 标签数组
const char* topLabels[] = {"FM", "MW", "SW"};
const char* bottomLabels[3][3] = 
{
    {"SEEK_100K", "SEEK_50K", "TUNE"},
    {"TUNE_9K", "TUNE_1K", "SEEK"},
    {"TUNE_5K", "TUNE_500K", "SEEK"}
};

// 全局对象
ILI9341_LTSM myTFT;
TEF6686 radio;

// ==================== 全局变量 ====================

// 界面和频率状态结构体
struct RadioState {
    bool topSelected[3] = {true, false, false};
    bool bottomSelected[3][3] = {{true, false, false}, {true, false, false}, {true, false, false}};
    
    bool displayNeedsUpdate = false;
    bool seekMode = true;
    uint16_t lastDisplayedFreq = 0;
    int lastSignalLevel = -1;
    bool lastStereoStatus = false;
    uint16_t fmFreq = 10370;
    uint16_t amFreq = 540;
    uint16_t swFreq = 6000;
    uint16_t freq = fmFreq;
    int currentBand = -1;
    int nextBand = 0;
    int swStep = SW_Step_5k;
    int mwStep = AM_Step_9k;
    bool fmSeekStep = true;
    uint8_t themeIdx = 0;      // 当前屏幕配色方案序号(见 SCREEN_THEMES)
};

RadioState radioState;

// ==================== 屏幕配色方案 ====================
// 每个配色包含 7 个颜色角色:
//   bg     背景色(整屏底色, 色块之间的间隙)
//   panel  频率显示面板色(主色块, 较醒目)
//   panel2 顶部/状态/底部横栏色(辅色块, 与 panel 区分层次)
//   edge   色块区域描边色
//   fg     前景主色(频率数字/选中标签/信号条/小数点/STEREO)
//   dim    次要色(未选中标签/信号条边框/单位文字)
//   hint   提示文字色(MONO 等)
typedef struct {
    uint16_t bg;
    uint16_t panel;
    uint16_t panel2;
    uint16_t edge;
    uint16_t fg;
    uint16_t dim;
    uint16_t hint;
} ScreenTheme;

static const ScreenTheme SCREEN_THEMES[] = {
    /* 0 经典黑白 */ { ILI9341_LTSM::C_BLACK, ILI9341_LTSM::C_DGREY,  0x4208,                 ILI9341_LTSM::C_LGREY, ILI9341_LTSM::C_WHITE,  ILI9341_LTSM::C_LGREY, ILI9341_LTSM::C_LGREY },
    /* 1 琥珀复古 */ { ILI9341_LTSM::C_BLACK, ILI9341_LTSM::C_MAROON, 0x5000,                 ILI9341_LTSM::C_BROWN, ILI9341_LTSM::C_YELLOW, ILI9341_LTSM::C_BROWN, ILI9341_LTSM::C_OLIVE },
    /* 2 绿色荧光 */ { ILI9341_LTSM::C_BLACK, ILI9341_LTSM::C_DGREEN, 0x0100,                 ILI9341_LTSM::C_OLIVE, ILI9341_LTSM::C_GREEN,  ILI9341_LTSM::C_OLIVE, ILI9341_LTSM::C_OLIVE },
    /* 3 深蓝冰蓝 */ { ILI9341_LTSM::C_NAVY,  0x10C8,                 0x08A8,                 ILI9341_LTSM::C_LBLUE, ILI9341_LTSM::C_WHITE,  ILI9341_LTSM::C_LBLUE, ILI9341_LTSM::C_LBLUE },
    /* 4 白纸反色 */ { ILI9341_LTSM::C_WHITE, ILI9341_LTSM::C_LGREY,  0xE71C,                 ILI9341_LTSM::C_GREY,  ILI9341_LTSM::C_BLACK,  ILI9341_LTSM::C_DGREY, ILI9341_LTSM::C_GREY  },
};
#define THEME_COUNT (sizeof(SCREEN_THEMES) / sizeof(SCREEN_THEMES[0]))

/* 当前生效的配色(绘制函数统一使用这 7 个变量) */
uint16_t themeBg     = ILI9341_LTSM::C_BLACK;
uint16_t themePanel  = ILI9341_LTSM::C_DGREY;
uint16_t themePanel2 = 0x4208;
uint16_t themeEdge   = ILI9341_LTSM::C_LGREY;
uint16_t themeFg     = ILI9341_LTSM::C_WHITE;
uint16_t themeDim    = ILI9341_LTSM::C_LGREY;
uint16_t themeHint   = ILI9341_LTSM::C_LGREY;

/* 应用指定序号的配色方案(越界则回到方案 0) */
void applyTheme(uint8_t idx) {
    if (idx >= THEME_COUNT) idx = 0;
    radioState.themeIdx = idx;
    themeBg     = SCREEN_THEMES[idx].bg;
    themePanel  = SCREEN_THEMES[idx].panel;
    themePanel2 = SCREEN_THEMES[idx].panel2;
    themeEdge   = SCREEN_THEMES[idx].edge;
    themeFg     = SCREEN_THEMES[idx].fg;
    themeDim    = SCREEN_THEMES[idx].dim;
    themeHint   = SCREEN_THEMES[idx].hint;
}

// 编码器
class RotaryEncoder {
private:
    volatile int32_t count;
    uint8_t lastState;
    unsigned long lastInterruptTime;
    
public:
    RotaryEncoder() : count(0), lastState(0), lastInterruptTime(0) {}
    
    void update(uint8_t pinA, uint8_t pinB) {
        uint8_t stateA = digitalRead(pinA);
        uint8_t stateB = digitalRead(pinB);
        uint8_t currentState = (stateA << 1) | stateB;
        uint8_t stateChange = (lastState << 2) | currentState;
        
        switch (stateChange) {
            case 0b0001: case 0b0111: case 0b1110: case 0b1000:
                count--;
                break;
            case 0b0010: case 0b1011: case 0b1101: case 0b0100:
                count++;
                break;
        }
        
        lastState = currentState;
        lastInterruptTime = millis();
    }
    
    int32_t getCount() { return count; }
    void reset() { count = 0; }
};

RotaryEncoder encoder;

// 按钮状态
enum ButtonState { BUTTON_IDLE, BUTTON_PRESSED, BUTTON_WAIT_RELEASE, 
                  BUTTON_DOUBLE_WAIT, BUTTON_DOUBLE_PRESSED };
ButtonState buttonState = BUTTON_IDLE;
unsigned long buttonPressTime = 0;
unsigned long buttonReleaseTime = 0;
bool clickActionPending = false;
bool doubleClickActionPending = false;

// 按钮常量
const unsigned long DEBOUNCE_TIME = 20;
const unsigned long CLICK_MAX_TIME = 300;
const unsigned long DOUBLE_CLICK_GAP = 400;
const unsigned long LONG_PRESS_TIME = 800;

// ==================== 显示函数 ====================

// 频率显示函数
void updateFrequency(int start_x, int start_y, uint16_t freq, const uint8_t* font) {
    if (freq == radioState.lastDisplayedFreq) return;
    
    myTFT.setFont(font);
    myTFT.setTextColor(themeFg, themePanel);
    
    int pos1 = start_x;
    int pos2 = start_x + 32;  
    int pos3 = start_x + 32*2;
    int pos4 = start_x + 32*3;
    int pos5 = start_x + 32*4;
    int dotPos = start_x + 32*3+16;
    int decimalPos = start_x + 32*3+32;
    
    if (radioState.nextBand == 0) {
        int integerPart = freq / 100;
        int decimalDigit = (freq / 10) % 10;
        
        if (integerPart >= 100) {
            int hundreds = integerPart / 100;
            int tens = (integerPart % 100) / 10;
            int units = integerPart % 10;
            
            myTFT.setCursor(pos1, start_y);
            myTFT.print(hundreds);
            myTFT.setCursor(pos2, start_y);
            myTFT.print(tens);
            myTFT.setCursor(pos3, start_y);
            myTFT.print(units);
        } else {
            int tens = integerPart / 10;
            int units = integerPart % 10;
            
            myTFT.fillRect(pos1, start_y, 32, 50, themePanel);
            myTFT.setCursor(pos2, start_y);
            myTFT.print(tens);
            myTFT.setCursor(pos3, start_y);
            myTFT.print(units);
        }
        
        int dotY = start_y + 50 - 10;
        myTFT.fillRect(pos4, start_y, 32, 50, themePanel);
        myTFT.fillRect(dotPos, dotY, 6, 6, themeFg);
        
        myTFT.setCursor(decimalPos, start_y);
        myTFT.print(decimalDigit);
        
    } else if (radioState.nextBand == 1 || radioState.nextBand == 2) {
        if (freq >= 10000) {
            int digit1 = freq / 10000;
            int digit2 = (freq % 10000) / 1000;
            int digit3 = (freq % 1000) / 100;
            int digit4 = (freq % 100) / 10;
            int digit5 = freq % 10;
            
            myTFT.setCursor(pos1, start_y);
            myTFT.print(digit1);
            myTFT.setCursor(pos2, start_y);
            myTFT.print(digit2);
            myTFT.setCursor(pos3, start_y);
            myTFT.print(digit3);
            
            myTFT.setCursor(pos4, start_y);
            myTFT.print(digit4);
            
            myTFT.setCursor(pos5, start_y);
            myTFT.print(digit5);
            
        } else if (freq >= 1000) {
            int digit1 = freq / 1000;
            int digit2 = (freq % 1000) / 100;
            int digit3 = (freq % 100) / 10;
            int digit4 = freq % 10;
            
            myTFT.setCursor(pos1, start_y);
            myTFT.print(digit1);
            myTFT.setCursor(pos2, start_y);
            myTFT.print(digit2);
            myTFT.setCursor(pos3, start_y);
            myTFT.print(digit3);
            
            myTFT.setCursor(pos4, start_y);
            myTFT.print(digit4);
            myTFT.fillRect(pos5, start_y, 33+10, 50, themePanel);
            
        } else if (freq >= 100) {
            int digit1 = freq / 100;
            int digit2 = (freq % 100) / 10;
            int digit3 = freq % 10;
            
            myTFT.setCursor(pos1, start_y);
            myTFT.print(digit1);
            myTFT.setCursor(pos2, start_y);
            myTFT.print(digit2);
            myTFT.setCursor(pos3, start_y);
            myTFT.print(digit3);
            myTFT.fillRect(pos4, start_y, 32*2+10, 50, themePanel);
            
        } else {
            myTFT.setCursor(pos2, start_y);
            if (freq >= 10) {
                int tens = freq / 10;
                int units = freq % 10;
                myTFT.print(tens);
                myTFT.setCursor(pos3, start_y);
                myTFT.print(units);
            } else {
                myTFT.print(freq);
            }
        }
    } else {
        myTFT.setCursor(pos1, start_y);
        myTFT.print("---");
    }
    
    radioState.lastDisplayedFreq = freq;
}

// 信号强度显示函数
void updateSignal(int start_x, int start_y, int level) {
    if (level == radioState.lastSignalLevel) return;
    
    const int BARS_COUNT = 5;
    const int BAR_WIDTH = 6;
    const int BAR_SPACING = 2;
    const int BARS_HEIGHT = 20;
    const int MIN_BAR_HEIGHT = 3;
    const int MAX_BAR_HEIGHT = 18;
    const int BAR_INCREMENT = 3;
    
    int signalBars = 0;
    if (level < 10) {
        signalBars = 0;
    } else if (level >= 50) {
        signalBars = 5;
    } else {
        signalBars = ((level - 10) * 5 / 40) + 1;
    }
    
    for (int i = 0; i < BARS_COUNT; i++) {
        int barX = start_x + i * (BAR_WIDTH + BAR_SPACING);
        
        int barHeight = MIN_BAR_HEIGHT + (i * BAR_INCREMENT);
        if (barHeight > MAX_BAR_HEIGHT) barHeight = MAX_BAR_HEIGHT;
        
        int barY = start_y + BARS_HEIGHT - barHeight;
        
        if (i < signalBars) {
            uint16_t barColor;
            if (signalBars <= 1) barColor = themeFg;
            else if (signalBars <= 3) barColor = themeFg;
            else barColor = themeFg;
            
            myTFT.fillRect(barX, barY, BAR_WIDTH, barHeight, barColor);
            myTFT.fillRect(barX, barY, BAR_WIDTH, 1, themePanel2);
        } else {
            myTFT.fillRect(barX, barY, BAR_WIDTH, barHeight, themePanel2);
            myTFT.drawRectWH(barX, barY, BAR_WIDTH, barHeight, themeDim);
        }
    }
    
    radioState.lastSignalLevel = level;
}

// ==================== 界面绘制函数 ====================

/* 绘制圆角色块区域: 圆角填充(panelColor) + 圆角描边(themeEdge) */
void drawPanel(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t panelColor, uint16_t radius) {
    myTFT.fillRoundRect(x, y, w, h, radius, panelColor);
    myTFT.drawRoundRect(x, y, w, h, radius, themeEdge);
}

/* 频率单位文字: FM 显示 MHz, MW/SW 显示 kHz (FontDefault 8x8, 面板内右下角与数字底对齐) */
void updateFreqUnit() {
    myTFT.setFont(FontDefault);
    myTFT.setTextColor(themeDim, themePanel);
    myTFT.setCursor(FREQ_UNIT_X, FREQ_UNIT_Y);
    myTFT.print(radioState.nextBand == 0 ? "MHz" : "kHz");
}

void drawScreenLayout() {
    myTFT.fillScreen(themeBg);
    
    /* 四个圆角色块区域 */
    drawPanel(TOPBAR_X, TOPBAR_Y, TOPBAR_W, TOPBAR_H, themePanel2, PANEL_RADIUS_BAR);          // 顶部波段栏
    drawPanel(FREQPANEL_X, FREQPANEL_Y, FREQPANEL_W, FREQPANEL_H, themePanel, PANEL_RADIUS_FREQ); // 频率显示面板
    drawPanel(STATUSBAR_X, STATUSBAR_Y, STATUSBAR_W, STATUSBAR_H, themePanel2, PANEL_RADIUS_BAR); // 状态栏
    drawPanel(BOTBAR_X, BOTBAR_Y, BOTBAR_W, BOTBAR_H, themePanel2, PANEL_RADIUS_BAR);           // 底部模式栏
    
    /* 顶部波段标签 (FontHallfetica 16x16, 整行水平居中, 栏内垂直居中) */
    myTFT.setFont(FontHallfetica);
    uint16_t rowWidth = 0;
    for (int i = 0; i < 3; i++) rowWidth += (strlen(topLabels[i]) * 16) + 32;
    rowWidth -= 32;  /* 最后一个标签无右侧间距 */
    uint16_t x = (DISPLAY_WIDTH - rowWidth) / 2;
    for (int i = 0; i < 3; i++) {
        uint16_t color = radioState.topSelected[i] ? themeFg : themeDim;
        myTFT.setTextColor(color, themePanel2);
        myTFT.setCursor(x, TOPBAR_Y + (TOPBAR_H - 16) / 2);
        myTFT.print(topLabels[i]);
        x += (strlen(topLabels[i]) * 16) + 32;
    }
    
    updateBottomLabels();
    updateFreqUnit();
    
    /* 状态栏 MONO 提示 (FontDefault 8x8, 栏内垂直居中) */
    myTFT.setFont(FontDefault);
    myTFT.setTextColor(themeHint, themePanel2);
    myTFT.setCursor(STATUS_TEXT_X, STATUS_TEXT_Y);
    myTFT.print("MONO   ");
}

void updateTopLabels() {
    myTFT.setFont(FontHallfetica);
    uint16_t rowWidth = 0;
    for (int i = 0; i < 3; i++) rowWidth += (strlen(topLabels[i]) * 16) + 32;
    rowWidth -= 32;
    uint16_t x = (DISPLAY_WIDTH - rowWidth) / 2;
    for (int i = 0; i < 3; i++) {
        uint16_t color = radioState.topSelected[i] ? themeFg : themeDim;
        myTFT.setTextColor(color, themePanel2);
        myTFT.setCursor(x, TOPBAR_Y + (TOPBAR_H - 16) / 2);
        myTFT.print(topLabels[i]);
        x += (strlen(topLabels[i]) * 16) + 32;
    }
}

void updateBottomLabels() {
    myTFT.setFont(FontDefault);
    int startX = 80;
    
    myTFT.fillRect(60, BOTBAR_Y + 6, 260, BOTBAR_H - 12, themePanel2);
    
    for (int j = 0; j < 3; j++) {
        uint16_t color = radioState.bottomSelected[radioState.nextBand][j] ? themeFg : themeDim;
        myTFT.setTextColor(color, themePanel2);
        myTFT.setCursor(startX, BOTBAR_Y + (BOTBAR_H - 8) / 2);
        myTFT.print(bottomLabels[radioState.nextBand][j]);
        startX += (strlen(bottomLabels[radioState.nextBand][j]) * 8) + 16;
    }
}

// ==================== 状态保存 / 恢复 (EEPROM 磨损均衡) ====================

static uint8_t st_lastPayload[STATE_PAYLOAD_LEN];
static bool   st_lastValid = false;

/* 把当前 radioState 打包成负载字节 */
static void statePackPayload(uint8_t p[STATE_PAYLOAD_LEN]) {
    memset(p, 0, STATE_PAYLOAD_LEN);
    p[0]  = (uint8_t)(radioState.nextBand & 0xFF);
    p[1]  = radioState.seekMode    ? 1 : 0;
    p[2]  = radioState.fmSeekStep  ? 1 : 0;
    p[3]  = (uint8_t)(radioState.mwStep & 0xFF);
    p[4]  = (uint8_t)(radioState.swStep & 0xFF);
    p[5]  = (uint8_t)((radioState.swStep >> 8) & 0xFF);
    p[6]  = (uint8_t)(radioState.fmFreq & 0xFF);
    p[7]  = (uint8_t)((radioState.fmFreq >> 8) & 0xFF);
    p[8]  = (uint8_t)(radioState.amFreq & 0xFF);
    p[9]  = (uint8_t)((radioState.amFreq >> 8) & 0xFF);
    p[10] = (uint8_t)(radioState.swFreq & 0xFF);
    p[11] = (uint8_t)((radioState.swFreq >> 8) & 0xFF);
    int k = 12;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            p[k++] = radioState.bottomSelected[i][j] ? 1 : 0;
    p[21] = radioState.themeIdx;   // 屏幕配色方案序号
}

/* 解包负载到 radioState(带边界防护, 非法值忽略) */
static void statePayloadUnpack(const uint8_t p[STATE_PAYLOAD_LEN]) {
    if (p[0] <= 2) radioState.nextBand = p[0];
    radioState.seekMode   = (p[1] == 1);
    radioState.fmSeekStep = (p[2] == 1);
    if (p[3] == AM_Step_1k || p[3] == AM_Step_9k) radioState.mwStep = p[3];
    uint16_t sstep = (uint16_t)(((uint16_t)p[5] << 8) | p[4]);
    if (sstep == SW_Step_5k || sstep == SW_Step_500k) radioState.swStep = sstep;
    radioState.fmFreq = (uint16_t)((uint16_t)p[6] | ((uint16_t)p[7] << 8));
    radioState.amFreq = (uint16_t)((uint16_t)p[8] | ((uint16_t)p[9] << 8));
    radioState.swFreq = (uint16_t)((uint16_t)p[10] | ((uint16_t)p[11] << 8));
    int k = 12;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            radioState.bottomSelected[i][j] = (p[k] == 1);
            k++;
        }
    if (p[21] < THEME_COUNT) radioState.themeIdx = p[21];   // 配色序号(旧记录为 0 = 默认黑白)
    /* 把频率钳制在合法范围内, 防止 EEPROM 里是脏数据 */
    if (radioState.fmFreq < FM_MIN_FREQ) radioState.fmFreq = FM_MIN_FREQ;
    if (radioState.fmFreq > FM_MAX_FREQ) radioState.fmFreq = FM_MAX_FREQ;
    if (radioState.amFreq < AM_MIN_FREQ) radioState.amFreq = AM_MIN_FREQ;
    if (radioState.amFreq > AM_MAX_FREQ) radioState.amFreq = AM_MAX_FREQ;
    if (radioState.swFreq < SW_MIN_FREQ) radioState.swFreq = SW_MIN_FREQ;
    if (radioState.swFreq > SW_MAX_FREQ) radioState.swFreq = SW_MAX_FREQ;
}

/* 保存当前状态(若状态未变化则跳过写操作, 避免无谓磨损) */
void stateSave() {
    uint8_t p[STATE_PAYLOAD_LEN];
    statePackPayload(p);
    if (st_lastValid && memcmp(p, st_lastPayload, STATE_PAYLOAD_LEN) == 0) return;
    memcpy(st_lastPayload, p, STATE_PAYLOAD_LEN);
    st_lastValid = true;
    st_save(p);
}

/* ==================== 调台落定后延时保存 ====================
 * 手动旋钮调台频率变化频繁, 若每档写 EEPROM 会反复磨损且卡手感;
 * 搜台刚落台时也可能需要观察判断是否继续听。
 * 因此统一用"时间戳 + 稳定窗口"防抖: 变化后只记时间,
 * 待频率连续稳定满【窗口时长】才真正保存一次。
 *    - 手动调台 / 搜台落台: 连续稳定满 STATE_SAVE_DELAY_MANUAL_MS /
 *      STATE_SAVE_DELAY_SEEK_MS 后保存
 * 波段切换/步进切换/双击等低频明确操作仍立即调用 stateSave。 */

static uint32_t st_pendingChangeMs = 0;   /* 最近一次相关频率变化时刻 */
static uint32_t st_pendingWindowMs = 0;   /* 需稳定的窗口时长(ms)      */
static bool     st_needSave        = false;

/* 频率发生变化后调用: 记录变化时刻与窗口, 不立即写 EEPROM */
static void markPendingSave(uint32_t windowMs) {
    st_pendingChangeMs = millis();
    st_pendingWindowMs = windowMs;
    st_needSave = true;
}

/* loop() 中周期性调用: 稳定满窗口则保存一次 */
static void maybeSaveDebounced() {
    if (!st_needSave) return;
    if ((uint32_t)(millis() - st_pendingChangeMs) >= st_pendingWindowMs) {
        st_needSave = false;   /* 先清标志, 省得重复保存 */
        stateSave();           /* 稳定后落定保存(内部会做内容比对去重) */
    }
}

/* 启动时恢复上次状态 */
void stateRestore() {
    st_open();   /* 定位磨损均衡游标(只调用一次) */
    uint8_t p[STATE_PAYLOAD_LEN];
    if (st_load(p)) {
        statePayloadUnpack(p);
        memcpy(st_lastPayload, p, STATE_PAYLOAD_LEN);
        st_lastValid = true;
    }
    /* 恢复后的界面定位 */
    radioState.currentBand = radioState.nextBand;
    for (int i = 0; i < 3; i++)
        radioState.topSelected[i] = (i == radioState.nextBand);
    if (radioState.nextBand == 0)      radioState.freq = radioState.fmFreq;
    else if (radioState.nextBand == 1) radioState.freq = radioState.amFreq;
    else                               radioState.freq = radioState.swFreq;
}

/* 把已恢复的状态实际应用到收音机硬件与界面高亮 */
void applyStateToRadio() {
    for (int i = 0; i < 3; i++)
        radioState.topSelected[i] = (i == radioState.nextBand);

    switch (radioState.nextBand) {
        case 0:
            radio.setFrequency(radioState.fmFreq);
            radioState.freq = radioState.fmFreq;
            /* FM 底栏: 若选中前两格(SEEK_100K / SEEK_50K)则进入 seek */
            if (radioState.bottomSelected[0][0] || radioState.bottomSelected[0][1])
                radioState.seekMode = true;
            else
                radioState.seekMode = false;
            break;
        case 1:
            radio.SetFreqMW(radioState.amFreq);
            radioState.freq = radioState.amFreq;
            radioState.seekMode = radioState.bottomSelected[1][2];
            break;
        case 2:
            radio.SetFreqSW(radioState.swFreq);
            radioState.freq = radioState.swFreq;
            radioState.seekMode = radioState.bottomSelected[2][2];
            break;
    }
}

// ==================== 按键处理函数 ====================

void updateButtonState() {
    static bool lastButtonState = HIGH;
    bool currentButtonState = digitalRead(BUTTON_PIN);
    unsigned long currentTime = millis();
    
    switch (buttonState) {
        case BUTTON_IDLE:
            if (currentButtonState == LOW && lastButtonState == HIGH) {
                buttonPressTime = currentTime;
                buttonState = BUTTON_PRESSED;
            }
            break;
            
        case BUTTON_PRESSED:
            if (currentTime - buttonPressTime > DEBOUNCE_TIME) {
                buttonState = BUTTON_WAIT_RELEASE;
            }
            break;
            
        case BUTTON_WAIT_RELEASE:
            if (currentTime - buttonPressTime > LONG_PRESS_TIME) {
                radioState.currentBand = radioState.nextBand;
                radioState.nextBand = (radioState.currentBand + 1) % 3;
                
                for (int i = 0; i < 3; i++) {
                    radioState.topSelected[i] = (i == radioState.nextBand);
                }
                
                switch(radioState.nextBand) {
                    case 0:
                        radio.setFrequency(radioState.fmFreq);
                        radioState.freq = radioState.fmFreq;
                        myTFT.fillRect(60, BOTBAR_Y + 6, 200, BOTBAR_H - 12, themePanel2);
                        if(radioState.bottomSelected[0][0] == true) radioState.seekMode = true;
                        else if(radioState.bottomSelected[0][1] == true) radioState.seekMode = true;
                        else radioState.seekMode = false;
                        radioState.displayNeedsUpdate = true;
                        break;
                    case 1:
                        radio.SetFreqMW(radioState.amFreq);delay(10);radio.SetFreqMW(radioState.amFreq);
                        radioState.freq = radioState.amFreq;
                        myTFT.fillRect(60, BOTBAR_Y + 6, 200, BOTBAR_H - 12, themePanel2);
                        if(radioState.bottomSelected[1][2] == true) radioState.seekMode = true;
                        else radioState.seekMode = false;
                        radioState.displayNeedsUpdate = true;
                        break;
                    case 2:
                        radio.SetFreqSW(radioState.swFreq);delay(10);radio.SetFreqSW(radioState.swFreq);
                        radioState.freq = radioState.swFreq;
                        myTFT.fillRect(60, BOTBAR_Y + 6, 200, BOTBAR_H - 12, themePanel2);
                        if(radioState.bottomSelected[2][2] == true) radioState.seekMode = true;
                        else radioState.seekMode = false;
                        radioState.displayNeedsUpdate = true;
                        break;
                }
                
                updateTopLabels();
                updateBottomLabels();
                updateFreqUnit();     // 波段切换 -> 更新频率单位文字
                stateSave();          // 波段切换 -> 保存
                buttonState = BUTTON_IDLE;
                return;
            }
            
            if (currentButtonState == HIGH && lastButtonState == LOW) {
                buttonReleaseTime = currentTime;
                
                if (currentTime - buttonPressTime <= CLICK_MAX_TIME) {
                    buttonState = BUTTON_DOUBLE_WAIT;
                } else {
                    buttonState = BUTTON_IDLE;
                }
            }
            break;
            
        case BUTTON_DOUBLE_WAIT:
            if (currentButtonState == LOW && lastButtonState == HIGH) {
                unsigned long secondPressTime = currentTime;
                if (secondPressTime - buttonReleaseTime <= DOUBLE_CLICK_GAP) {
                    doubleClickActionPending = true;
                    buttonState = BUTTON_DOUBLE_PRESSED;
                } else {
                    clickActionPending = true;
                    buttonState = BUTTON_IDLE;
                }
            }
            else if (currentTime - buttonReleaseTime > DOUBLE_CLICK_GAP) {
                clickActionPending = true;
                buttonState = BUTTON_IDLE;
            }
            break;
            
        case BUTTON_DOUBLE_PRESSED:
            if (currentButtonState == HIGH && lastButtonState == LOW) {
                buttonState = BUTTON_IDLE;
            }
            break;
    }
    
    lastButtonState = currentButtonState;
}

void processButtonActions() {
    if (clickActionPending) {
        clickActionPending = false;
        
        int currentBand = radioState.nextBand;
        for (int j = 0; j < 3; j++) {
            if (radioState.bottomSelected[currentBand][j]) {
                radioState.bottomSelected[currentBand][j] = false;
                radioState.bottomSelected[currentBand][(j + 1) % 3] = true;
                
                if (currentBand == 0) {
                    if (j == 0) {
                        radioState.fmSeekStep = false;
                        radioState.seekMode = true;
                    } else if (j == 1) {
                        radioState.seekMode = false;
                    } else {
                        radioState.seekMode = true;
                        radioState.fmSeekStep = true;
                    }
                } else if (currentBand == 1) {
                    if (j == 0) {
                        radioState.seekMode = false;
                        radioState.mwStep = AM_Step_1k;
                    } else if (j == 1) {
                        radioState.seekMode = true;
                    } else {
                        radioState.seekMode = false;
                        radioState.amFreq = radioState.amFreq - (radioState.amFreq % 9);
                        radioState.mwStep = AM_Step_9k;
                    }
                } else if (currentBand == 2) {
                    if (j == 0) {
                        radioState.seekMode = false;
                        radioState.swStep = SW_Step_500k;
                    } else if (j == 1) {
                        radioState.seekMode = true;
                    } else {
                        radioState.seekMode = false;
                        radioState.swStep = SW_Step_5k;
                    }
                }
                
                break;
            }
        }
        updateBottomLabels();
        stateSave();   // 单击切换步进/模式 -> 立即保存
    }
    
    if (doubleClickActionPending) {
        doubleClickActionPending = false;
        
        /* 双击: 循环切换屏幕配色 */
        applyTheme((uint8_t)((radioState.themeIdx + 1) % THEME_COUNT));
        
        /* 用新配色整屏重绘 */
        drawScreenLayout();                     // 清屏 + 顶部波段 + 底部模式标签 + MONO
        radioState.lastDisplayedFreq = 0xFFFF;  // 强制完整重绘频率数字
        radioState.displayNeedsUpdate = true;
        radioState.lastSignalLevel = -1;        // 强制重绘信号条
        radioState.lastStereoStatus = false;    // 强制重绘 STEREO/MONO 文字
        
        /* 不立即写 EEPROM: 在选定的配色上稳定满 STATE_SAVE_DELAY_THEME_MS 才保存,
         * 避免快速双击试色时反复写入造成磨损 */
        markPendingSave(STATE_SAVE_DELAY_THEME_MS);
    }
}

// ==================== 硬件初始化 ====================

void IRAM_ATTR encoderISR() {
    static unsigned long lastInterruptTime = 0;
    unsigned long interruptTime = millis();
    
    if (interruptTime - lastInterruptTime < 5) return;
    
    encoder.update(ENCODER_PIN_A, ENCODER_PIN_B);
    
    lastInterruptTime = interruptTime;
}

bool initRadio() {
    if (!radio.init()) {
        return false;
    }
    
    delay(100);      // 收音机初始化完成后的短暂稳定(原 500ms 过长, 已缩短)
    radio.powerOn();
    delay(100);
    return true;
}

bool initDisplay() {
    bool bhardwareSPI = true;
    
    if (bhardwareSPI) {
        uint32_t TFT_SCLK_FREQ = 40000000;
        myTFT.SetupGPIO_SPI(TFT_SCLK_FREQ, TFT_RST, TFT_DC, TFT_CS);
    }
    
    myTFT.SetupScreenSize(240, 320);
    myTFT.ILI9341Initialize();
    myTFT.setRotation(myTFT.Degrees_270);
    return true;
}

void initEncoder() {
    pinMode(ENCODER_PIN_A, INPUT_PULLUP);
    pinMode(ENCODER_PIN_B, INPUT_PULLUP);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_A), encoderISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_B), encoderISR, CHANGE);
}

// ==================== 搜索函数 ====================

uint16_t FMSeek(uint8_t up) {
    uint16_t mode = 20;
    uint16_t startFrequency = Radio_GetCurrentFreq();
    uint16_t seekStep = radioState.fmSeekStep ? FM_Step_100k : (FM_Step_100k / 2);
    
    while (true) {
        switch(mode){
            case 20:
                Radio_ChangeFreqOneStep(up, seekStep);
                Radio_SetFreq(Radio_SEARCHMODE, Radio_GetCurrentBand(), Radio_GetCurrentFreq());
                updateFrequency(FREQ_START_X, FREQ_START_Y, Radio_GetCurrentFreq(), FontSixteenSeg);
            
                mode = 30;
                Radio_CheckStationInit();
                Radio_ClearCurrentStation();
                break;
            
            case 30:
                delay(20);
                Radio_CheckStation();
                if (Radio_CheckStationStatus() >= NO_STATION) {
                    mode = 40;
                }   
                break;

            case 40:
                if (Radio_CheckStationStatus() == NO_STATION) {        
                    mode = (startFrequency == Radio_GetCurrentFreq()) ? 50 : 20;
                }
                else if (Radio_CheckStationStatus() == PRESENT_STATION) {
                    mode = 50;
                }
                break;
            
            case 50:
                Radio_SetFreq(Radio_PRESETMODE, Radio_GetCurrentBand(), Radio_GetCurrentFreq());
                return Radio_GetCurrentFreq();
        }
    }
    return 0;
}

uint16_t MWSeek(uint8_t up) {
    uint16_t mode = 20;
    uint16_t startFrequency = Radio_GetCurrentFreq();
    Serial.print("MWSeek - mwStep: ");
    Serial.println(radioState.mwStep);
    Serial.print("Current freq: ");
    Serial.println(Radio_GetCurrentFreq());
    while (true) {
        switch(mode){
            case 20:
                Radio_ChangeFreqOneStep(up, 1);
                Radio_SetFreq(Radio_SEARCHMODE, Radio_GetCurrentBand(), Radio_GetCurrentFreq());
                updateFrequency(FREQ_START_X, FREQ_START_Y, Radio_GetCurrentFreq(), FontSixteenSeg);
            
                mode = 30;
                Radio_CheckStationInit();
                Radio_ClearCurrentStation();
                break;
            
            case 30:
                delay(40);
                Radio_CheckStation();
                if (Radio_CheckStationStatus() >= NO_STATION) {
                    mode = 40;
                }   
                break;

            case 40:
                if (Radio_CheckStationStatus() == NO_STATION) {        
                    mode = (startFrequency == Radio_GetCurrentFreq()) ? 50 : 20;
                }
                else if (Radio_CheckStationStatus() == PRESENT_STATION) {
                    mode = 50;
                }
                break;
            
            case 50:
                Radio_SetFreq(Radio_PRESETMODE, Radio_GetCurrentBand(), Radio_GetCurrentFreq());
                return Radio_GetCurrentFreq();
        }
    }
    return 0;
}

uint16_t SWSeek(uint8_t up) {
    uint16_t mode = 20;
    uint16_t startFrequency = Radio_GetCurrentFreq();

    while (true) {
        switch(mode){
            case 20:
                Radio_ChangeFreqOneStep(up, 5);
                Radio_SetFreq(Radio_SEARCHMODE, Radio_GetCurrentBand(), Radio_GetCurrentFreq());
                updateFrequency(FREQ_START_X, FREQ_START_Y, Radio_GetCurrentFreq(), FontSixteenSeg);
            
                mode = 30;
                Radio_CheckStationInit();
                Radio_ClearCurrentStation();
                break;
            
            case 30:
                delay(40);
                Radio_CheckStation();
                if (Radio_CheckStationStatus() >= NO_STATION) {
                    mode = 40;
                }   
                break;

            case 40:
                if (Radio_CheckStationStatus() == NO_STATION) {        
                    mode = (startFrequency == Radio_GetCurrentFreq()) ? 50 : 20;
                }
                else if (Radio_CheckStationStatus() == PRESENT_STATION) {
                    mode = 50;
                }
                break;
            
            case 50:
                Radio_SetFreq(Radio_PRESETMODE, Radio_GetCurrentBand(), Radio_GetCurrentFreq());
                return Radio_GetCurrentFreq();
        }
    }
    return 0;
}

// ==================== 主程序 ====================

void setup() {
    // 原 delay(2000) 开机空等, 无任何依赖, 已移除以加快开机。

    Serial.begin(115200);
    
    pinMode(0, OUTPUT);
    digitalWrite(0, HIGH);
    
    if (!initRadio()) while(1);
    if (!initDisplay()) while(1);
    
    initEncoder();

    // 从 EEPROM 恢复上次保存的状态（波段/频率/步进/模式/配色）
    stateRestore();
    applyTheme(radioState.themeIdx);   // 应用恢复的配色(必须在 drawScreenLayout 之前)
    
    applyStateToRadio();   // 把恢复的状态实际设置到收音机硬件与界面
    radio.setVolume(-220);
    
    drawScreenLayout();
    radioState.displayNeedsUpdate = true;
    
    digitalWrite(0, LOW);
}

void loop() {
    static unsigned long lastUpdateTime = 0;
    static unsigned long lastSignalUpdateTime = 0;
    unsigned long currentTime = millis();
    
    updateButtonState();
    
    if (clickActionPending || doubleClickActionPending) {
        processButtonActions();
    }

    maybeSaveDebounced();   // 调台频率稳定满窗口则落定保存(窗口时长见 STATE_SAVE_DELAY_*_MS)
    
    static int32_t lastCount = 0;
    int32_t currentCount = encoder.getCount();
    
    if (currentCount != lastCount) {
        if (radioState.seekMode) {
            if (currentCount > 3) {
                switch(radioState.nextBand) {
                    case 0:
                        radioState.fmFreq = FMSeek(true);
                        radioState.freq = radioState.fmFreq;
                        encoder.reset();
                        radioState.displayNeedsUpdate = true;
                        break;
                    case 1:
                        radioState.amFreq = MWSeek(true);
                        radioState.freq = radioState.amFreq;
                        encoder.reset();
                        radioState.displayNeedsUpdate = true;
                        break;
                    case 2:
                        radioState.swFreq = SWSeek(true);
                        radioState.freq = radioState.swFreq;
                        encoder.reset();
                        radioState.displayNeedsUpdate = true;
                        break;
                }
            } else if (currentCount < -3) {
                switch(radioState.nextBand) {
                    case 0:
                        radioState.fmFreq = FMSeek(false);
                        radioState.freq = radioState.fmFreq;
                        encoder.reset();
                        radioState.displayNeedsUpdate = true;
                        break;
                    case 1:
                        radioState.amFreq = MWSeek(false);
                        radioState.freq = radioState.amFreq;
                        encoder.reset();
                        radioState.displayNeedsUpdate = true;
                        break;
                    case 2:
                        radioState.swFreq = SWSeek(false);
                        radioState.freq = radioState.swFreq;
                        encoder.reset();
                        radioState.displayNeedsUpdate = true;
                        break;
                }
            }
        } else {
            if (currentCount > 3) {
                switch(radioState.nextBand) {
                    case 0:
                        radioState.fmFreq = radioState.fmFreq + FM_Step_100k;
                        if (radioState.fmFreq > FM_MAX_FREQ) radioState.fmFreq = FM_MIN_FREQ;
                        radio.setFrequency(radioState.fmFreq);
                        radioState.freq = radioState.fmFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                    case 1:
                        radioState.amFreq = radioState.amFreq + radioState.mwStep;
                        if (radioState.amFreq > AM_MAX_FREQ) radioState.amFreq = AM_MIN_FREQ;
                        radio.SetFreqMW(radioState.amFreq);
                        radioState.freq = radioState.amFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                    case 2:
                        radioState.swFreq = radioState.swFreq + radioState.swStep;
                        if (radioState.swFreq > SW_MAX_FREQ) radioState.swFreq = SW_MIN_FREQ;
                        radio.SetFreqSW(radioState.swFreq);
                        radioState.freq = radioState.swFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                }
            } else if (currentCount < -3) {
                switch(radioState.nextBand) {
                    case 0:
                        radioState.fmFreq = radioState.fmFreq - FM_Step_100k;
                        if (radioState.fmFreq < FM_MIN_FREQ) radioState.fmFreq = FM_MAX_FREQ;
                        radio.setFrequency(radioState.fmFreq);
                        radioState.freq = radioState.fmFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                    case 1:
                        radioState.amFreq = radioState.amFreq - radioState.mwStep;
                        if (radioState.amFreq < AM_MIN_FREQ) radioState.amFreq = AM_MAX_FREQ;
                        radio.SetFreqMW(radioState.amFreq);
                        radioState.freq = radioState.amFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                    case 2:
                        radioState.swFreq = radioState.swFreq - radioState.swStep;
                        if (radioState.swFreq < SW_MIN_FREQ) radioState.swFreq = SW_MAX_FREQ;
                        radio.SetFreqSW(radioState.swFreq);
                        radioState.freq = radioState.swFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                }
            }
        }
        
        if (radioState.seekMode) {
            markPendingSave(STATE_SAVE_DELAY_SEEK_MS);    // 搜台落台 -> 稳定后保存
        } else {
            markPendingSave(STATE_SAVE_DELAY_MANUAL_MS);  // 手动调台 -> 稳定后保存
        }

        lastCount = currentCount;
    }
    
    if (radioState.displayNeedsUpdate) {
        updateFrequency(FREQ_START_X, FREQ_START_Y, radioState.freq, FontSixteenSeg);
        radioState.displayNeedsUpdate = false;
    }
    
    if(radioState.nextBand == 0) {
        if (currentTime - lastSignalUpdateTime >= 500) {
            uint16_t signalLevel = radio.getLevel(1);
            bool stereoStatus = radio.getStereoStatus();
            updateSignal(SIGNAL_X, SIGNAL_Y, signalLevel);
            
            if (stereoStatus != radioState.lastStereoStatus) {
                myTFT.setFont(FontDefault);
                
                if (stereoStatus) {
                    myTFT.setTextColor(themeFg, themePanel2);
                    myTFT.setCursor(STATUS_TEXT_X, STATUS_TEXT_Y);
                    myTFT.print("STEREO");
                } else {
                    myTFT.setTextColor(themeHint, themePanel2);
                    myTFT.setCursor(STATUS_TEXT_X, STATUS_TEXT_Y);
                    myTFT.print("MONO   ");
                }
                
                radioState.lastStereoStatus = stereoStatus;
            }
            lastSignalUpdateTime = currentTime;
        }
    } else {
        if (currentTime - lastSignalUpdateTime >= 500) {
            uint16_t signalLevel = radio.getLevel(0);
            updateSignal(SIGNAL_X, SIGNAL_Y, signalLevel);

            myTFT.setFont(FontDefault);
            myTFT.setTextColor(themeHint, themePanel2);
            myTFT.setCursor(STATUS_TEXT_X, STATUS_TEXT_Y);
            myTFT.print("MONO   ");
            radioState.lastStereoStatus = false;
            lastSignalUpdateTime = currentTime;
        }        
    }

    delay(10);
}