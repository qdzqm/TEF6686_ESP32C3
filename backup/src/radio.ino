#include "TEF6686.h"
#include "Wire.h"
#include <Arduino.h>
#include "ILI9341_LTSM.hpp"
#include "fonts_LTSM/FontRetro_LTSM.hpp"
#include "fonts_LTSM/FontDefault_LTSM.hpp"
#include "fonts_LTSM/FontSevenSeg_LTSM.hpp"
#include "fonts_LTSM/FontPico_LTSM.hpp"
#include "fonts_LTSM/FontSixteenSeg_LTSM.hpp"
#include "fonts_LTSM/FontArialBold_LTSM.hpp"
#include "fonts_LTSM/FontGroTesk_LTSM.hpp"
#include "fonts_LTSM/FontMega_LTSM.hpp"
#include "fonts_LTSM/FontArialRound_LTSM.hpp"

// 硬件引脚定义
#define ENCODER_PIN_A 1
#define ENCODER_PIN_B 3
#define BUTTON_PIN 2

#define TFT_RST 20
#define TFT_DC 21
#define TFT_CS 7

// 频率范围常量
#define FM_MIN_FREQ 8750   // 87.5 MHz
#define FM_MAX_FREQ 10800  // 108.0 MHz
#define AM_MIN_FREQ 531   // 531 KHz
#define AM_MAX_FREQ 1602  // 1602 KHz
#define SW_MIN_FREQ 1720   // 1711 KHz
#define SW_MAX_FREQ 30000  // 30 MHz

// 步进常量
#define FM_Step_100k 10    // 0.1 MHz步进
#define AM_Step_9k 9       // 9 kHz步进
#define AM_Step_1k 1       // 1 kHz步进
#define SW_Step_5k 5       // 5 kHz步进
#define SW_Step_500k 500   // 500 kHz步进

// 显示尺寸常量
const uint16_t DISPLAY_WIDTH = 320;   // 旋转270度后的宽度
const uint16_t DISPLAY_HEIGHT = 240;  // 旋转270度后的高度

// 标签数组
const char* topLabels[] = {"FM", "AM", "SW"};
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
    bool bottomSelected[3][3] = {{true, false, false}, {true, false, false}, {true, false, false}};  // 改为3列
    uint16_t freq = 10370;
    bool displayNeedsUpdate = false;
    bool seekMode = true;
    uint16_t lastDisplayedFreq = 0;
    int lastSignalLevel = -1;
    bool lastStereoStatus = false;
    uint16_t fmFreq = 10370;
    uint16_t amFreq = 540;
    uint16_t swFreq = 6000;
    int currentBand = -1;
    int nextBand = 0;
    int swStep = SW_Step_5k;
    int amStep = AM_Step_9k;
    bool fmSeekStep = true;  // true: SEEK_100K, false: SEEK_50K
};

RadioState radioState;

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
    // 频率不变则不更新
    if (freq == radioState.lastDisplayedFreq) return;
    
    myTFT.setFont(font);
    myTFT.setTextColor(myTFT.C_GREEN, myTFT.C_BLACK);
    
    // 定义固定位置（基于32像素字符宽度）
    int pos1 = start_x;                // 第1个字符位置
    int pos2 = start_x + 32;           // 第2个字符位置  
    int pos3 = start_x + 32*2;         // 第3个字符位置
    int pos4 = start_x + 32*3;
    int pos5 = start_x + 32*4;
    int dotPos = start_x + 32*3+16;         // 小数点位置（在第3个字符后偏右）
    int decimalPos = start_x + 32*3+32;     // 小数位位置（小数点后）
    
    if (radioState.nextBand == 0) {
        // ================= FM显示 =================
        // 格式：带1位小数的MHz频率 (如87.5, 98.5, 108.0)
        // freq格式：x100 (8750 = 87.5 MHz)
        
        int integerPart = freq / 100;      // 整数部分 (87, 108)
        int decimalDigit = (freq / 10) % 10; // 十分位 (5, 0)
        
        // 分解整数部分
        if (integerPart >= 100) {
            // 三位数FM频率 (如108.0)
            int hundreds = integerPart / 100;      // 1
            int tens = (integerPart % 100) / 10;   // 0
            int units = integerPart % 10;          // 8
            
            // 显示在固定位置
            myTFT.setCursor(pos1, start_y);
            myTFT.print(hundreds);
            myTFT.setCursor(pos2, start_y);
            myTFT.print(tens);
            myTFT.setCursor(pos3, start_y);
            myTFT.print(units);
        } else {
            // 两位数FM频率 (如87.5)
            int tens = integerPart / 10;     // 8
            int units = integerPart % 10;    // 7
            
            // 显示在固定位置，第一位留空
            myTFT.fillRect(pos1, start_y, 32, 50, myTFT.C_BLACK);
            myTFT.setCursor(pos2, start_y);
            myTFT.print(tens);
            myTFT.setCursor(pos3, start_y);
            myTFT.print(units);
        }
        
        // 显示小数点（固定位置）
        int dotY = start_y + 50 - 10;  // 数字底部
        myTFT.fillRect(pos4, start_y, 32, 50, myTFT.C_BLACK);
        myTFT.fillRect(dotPos, dotY, 6, 6, myTFT.C_GREEN);
        
        // 显示小数位（固定位置）
        myTFT.setCursor(decimalPos, start_y);
        myTFT.print(decimalDigit);
        
    } else if (radioState.nextBand == 1 || radioState.nextBand == 2) {
        // ================= AM/短波显示 =================
        // 根据位数决定显示位置
        
        if (freq >= 10000) {
            // ===== 5位数短波频率 (10000-30000) =====
            // 规则：个位数在FM小数位置，十位数在FM小数点位置
            
            int digit1 = freq / 10000;               // 万位
            int digit2 = (freq % 10000) / 1000;      // 千位
            int digit3 = (freq % 1000) / 100;        // 百位
            int digit4 = (freq % 100) / 10;          // 十位
            int digit5 = freq % 10;                  // 个位
            
            // 显示在固定位置
            myTFT.setCursor(pos1, start_y);
            myTFT.print(digit1);
            myTFT.setCursor(pos2, start_y);
            myTFT.print(digit2);
            myTFT.setCursor(pos3, start_y);
            myTFT.print(digit3);
            
            // 十位数在小数点位置
            myTFT.setCursor(pos4, start_y);
            myTFT.print(digit4);
            
            // 个位数在小数位位置
            myTFT.setCursor(pos5, start_y);
            myTFT.print(digit5);
            
        } else if (freq >= 1000) {
            // ===== 4位数AM/短波频率 (1000-9999) =====
            // 规则：个位数显示在原先小数点所在位置
            
            int digit1 = freq / 1000;               // 千位
            int digit2 = (freq % 1000) / 100;       // 百位
            int digit3 = (freq % 100) / 10;         // 十位
            int digit4 = freq % 10;                 // 个位                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            
            
            // 显示在固定位置
            myTFT.setCursor(pos1, start_y);
            myTFT.print(digit1);
            myTFT.setCursor(pos2, start_y);
            myTFT.print(digit2);
            myTFT.setCursor(pos3, start_y);
            myTFT.print(digit3);
            
            // 个位数在小数点位置
            myTFT.setCursor(pos4, start_y);
            myTFT.print(digit4);
            myTFT.fillRect(pos5, start_y, 33+10, 50, myTFT.C_BLACK);
            
            // 小数位位置留空
            
        } else if (freq >= 100) {
            // ===== 3位数AM频率 (100-999) =====
            // 规则：都显示在FM整数位置
            
            int digit1 = freq / 100;               // 百位
            int digit2 = (freq % 100) / 10;        // 十位
            int digit3 = freq % 10;                // 个位
            
            // 显示在固定位置
            myTFT.setCursor(pos1, start_y);
            myTFT.print(digit1);
            myTFT.setCursor(pos2, start_y);
            myTFT.print(digit2);
            myTFT.setCursor(pos3, start_y);
            myTFT.print(digit3);
            myTFT.fillRect(pos4, start_y, 32*2+10, 50, myTFT.C_BLACK);
            // 小数点和小数位位置留空
            
        } else {
            // 2位数或1位数（极少见）
            // 显示在FM整数位置
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
        // 频率超出定义范围，显示错误
        myTFT.setCursor(pos1, start_y);
        myTFT.print("---");
    }
    
    radioState.lastDisplayedFreq = freq;
}

// 信号强度显示函数
void updateSignal(int start_x, int start_y, int level) {
    if (level == radioState.lastSignalLevel) return;
    
    // 信号柱状图参数
    const int BARS_COUNT = 5;
    const int BAR_WIDTH = 6;
    const int BAR_SPACING = 2;
    const int BARS_HEIGHT = 20;
    const int MIN_BAR_HEIGHT = 3;
    const int MAX_BAR_HEIGHT = 18;
    const int BAR_INCREMENT = 3;
    
    // 映射信号等级
    int signalBars = 0;
    if (level < 10) {
        signalBars = 0;
    } else if (level >= 50) {
        signalBars = 5;
    } else {
        signalBars = ((level - 10) * 5 / 40) + 1;
    }
    
    // 绘制所有信号条
    for (int i = 0; i < BARS_COUNT; i++) {
        int barX = start_x + i * (BAR_WIDTH + BAR_SPACING);
        
        // 计算当前条的高度
        int barHeight = MIN_BAR_HEIGHT + (i * BAR_INCREMENT);
        if (barHeight > MAX_BAR_HEIGHT) barHeight = MAX_BAR_HEIGHT;
        
        int barY = start_y + BARS_HEIGHT - barHeight;
        
        if (i < signalBars) {
            // 显示的条：根据等级设置颜色
            uint16_t barColor;
            if (signalBars <= 1) barColor = myTFT.C_WHITE;
            else if (signalBars <= 3) barColor = myTFT.C_YELLOW;
            else barColor = myTFT.C_GREEN;
            
            // 绘制实心条
            myTFT.fillRect(barX, barY, BAR_WIDTH, barHeight, barColor);
            
            // 在条的上方添加一个小间隙
            myTFT.fillRect(barX, barY, BAR_WIDTH, 1, myTFT.C_BLACK);
        } else {
            // 不显示的条：用黑色填充
            myTFT.fillRect(barX, barY, BAR_WIDTH, barHeight, myTFT.C_BLACK);
            
            // 绘制灰色边框（可选）
            myTFT.drawRectWH(barX, barY, BAR_WIDTH, barHeight, myTFT.C_DGREY);
        }
    }
    
    radioState.lastSignalLevel = level;
}

// ==================== 界面绘制函数 ====================

void drawScreenLayout() {
    myTFT.fillScreen(myTFT.C_BLACK);
    
    // 顶部标签
    myTFT.setFont(FontArialBold);
    int startX = 0;
    for (int i = 0; i < 3; i++) {
        uint16_t color = radioState.topSelected[i] ? myTFT.C_GREEN : myTFT.C_DGREY;
        myTFT.setTextColor(color, myTFT.C_BLACK);
        myTFT.setCursor(startX, 10);
        myTFT.print(topLabels[i]);
        startX += (strlen(topLabels[i]) * 16) + 32;
    }
    
    // 底部标签（使用updateBottomLabels函数）
    updateBottomLabels();
    
    // 立体声状态
    myTFT.fillRect(5, DISPLAY_HEIGHT/2 + 5, 90, 20, myTFT.C_BLACK);
    myTFT.setTextColor(myTFT.C_LGREY, myTFT.C_BLACK);
    myTFT.setCursor(5, DISPLAY_HEIGHT/2 + 5);
    myTFT.print("MONO");
}

void updateTopLabels() {
    myTFT.setFont(FontArialBold);
    int startX = 0;
    for (int i = 0; i < 3; i++) {
        uint16_t color = radioState.topSelected[i] ? myTFT.C_GREEN : myTFT.C_DGREY;
        myTFT.setTextColor(color, myTFT.C_BLACK);
        myTFT.setCursor(startX, 10);
        myTFT.print(topLabels[i]);
        startX += (strlen(topLabels[i]) * 16) + 32;
    }
}

void updateBottomLabels() {
    myTFT.setFont(FontDefault);
    int startX = 80;  // 调整起始位置以容纳3个标签
    
    // 清空底部标签区域
    myTFT.fillRect(60, DISPLAY_HEIGHT - 18, 200, 18, myTFT.C_BLACK);
    
    // 显示3个底部标签
    for (int j = 0; j < 3; j++) {
        uint16_t color = radioState.bottomSelected[radioState.nextBand][j] ? myTFT.C_WHITE : myTFT.C_DGREY;
        myTFT.setTextColor(color, myTFT.C_BLACK);
        myTFT.setCursor(startX, DISPLAY_HEIGHT - 13);
        myTFT.print(bottomLabels[radioState.nextBand][j]);
        startX += (strlen(bottomLabels[radioState.nextBand][j]) * 8) + 16;
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
                // 长按切换频段
                radioState.currentBand = radioState.nextBand;
                radioState.nextBand = (radioState.currentBand + 1) % 3;
                
                // 切换频段
                for (int i = 0; i < 3; i++) {
                    radioState.topSelected[i] = (i == radioState.nextBand);
                }
                
                switch(radioState.nextBand) {
                    case 0:
                        radio.setFrequency(radioState.fmFreq);
                        radioState.freq = radioState.fmFreq;
                        myTFT.fillRect(100, DISPLAY_HEIGHT - 18, 200, 18, myTFT.C_BLACK);
                        if(radioState.bottomSelected[0][0] == true) radioState.seekMode = true;
                        else radioState.seekMode = false;
                        radioState.displayNeedsUpdate = true;
                        break;
                    case 1:
                        radio.SetFreqAM(radioState.amFreq);
                        radioState.freq = radioState.amFreq;
                        myTFT.fillRect(100, DISPLAY_HEIGHT - 18, 200, 18, myTFT.C_BLACK);
                        radioState.seekMode = false;
                        radioState.displayNeedsUpdate = true;
                        break;
                    case 2:
                        radio.SetFreqAM(radioState.swFreq);
                        radioState.freq = radioState.swFreq;
                        myTFT.fillRect(100, DISPLAY_HEIGHT - 18, 200, 18, myTFT.C_BLACK);
                        radioState.seekMode = false;
                        radioState.displayNeedsUpdate = true;
                        break;
                }
                
                updateTopLabels();
                updateBottomLabels();
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
        
        // 在3个底部标签选项之间循环切换
        for (int j = 0; j < 3; j++) {
            if (radioState.bottomSelected[radioState.nextBand][j]) {
                // 当前选中的选项
                radioState.bottomSelected[radioState.nextBand][j] = false;
                radioState.bottomSelected[radioState.nextBand][(j + 1) % 3] = true;
                
                // 根据频段和选项设置对应的模式
                if (radioState.nextBand == 0) {
                    // FM频段
                    if (j == 0) {  // 从SEEK_100K切换到SEEK_50K
                        radioState.fmSeekStep = false;
                    } else if (j == 1) {  // 从SEEK_50K切换到TUNE
                        radioState.seekMode = false;
                    } else {  // 从TUNE切换到SEEK_100K
                        radioState.seekMode = true;
                        radioState.fmSeekStep = true;
                    }
                } else if (radioState.nextBand == 1) {
                    // AM频段 - 修复这里的逻辑
                    if (j == 0) {  // 从TUNE_9K切换到TUNE_1K
                        radioState.seekMode = false;
                        radioState.amStep = AM_Step_1k;  // 1K步进
                    } else if (j == 1) {  // 从TUNE_1K切换到SEEK
                        radioState.seekMode = true;  // 启用搜索模式
                        // 保持当前步进设置
                    } else {  // 从SEEK切换到TUNE_9K
                        radioState.seekMode = false;
                        radioState.amStep = AM_Step_9k;  // 9K步进
                        radioState.amFreq = radioState.amFreq - (radioState.amFreq % 9); // 对齐9K边界
                    }
                } else if (radioState.nextBand == 2) {
                    // SW频段 - 修复这里的逻辑
                    if (j == 0) {  // 从TUNE_5K切换到TUNE_500K
                        radioState.seekMode = false;
                        radioState.swStep = SW_Step_500k;  // 500K步进
                    } else if (j == 1) {  // 从TUNE_500K切换到SEEK
                        radioState.seekMode = true;  // 启用搜索模式
                        // 保持当前步进设置
                    } else {  // 从SEEK切换到TUNE_5K
                        radioState.seekMode = false;
                        radioState.swStep = SW_Step_5k;  // 5K步进
                    }
                }
                
                break;
            }
        }
        updateBottomLabels();
    }
    
    if (doubleClickActionPending) {
        // ... 保持原有的doubleClick逻辑不变 ...
        // 注意：这里需要根据底部标签重新设置seekMode
        switch(radioState.nextBand) {
            case 0:
                // FM频段逻辑保持不变
                radio.setFrequency(radioState.fmFreq);
                radioState.freq = radioState.fmFreq;
                myTFT.fillRect(60, DISPLAY_HEIGHT - 18, 200, 18, myTFT.C_BLACK);
                if (radioState.bottomSelected[0][0] || radioState.bottomSelected[0][1]) {
                    radioState.seekMode = true;
                    radioState.fmSeekStep = radioState.bottomSelected[0][0];
                } else {
                    radioState.seekMode = false;
                }
                radioState.displayNeedsUpdate = true;
                break;
            case 1:
                // AM频段 - 根据底部标签设置seekMode
                radio.SetFreqAM(radioState.amFreq);
                radioState.freq = radioState.amFreq;
                myTFT.fillRect(60, DISPLAY_HEIGHT - 18, 200, 18, myTFT.C_BLACK);
                radioState.seekMode = (radioState.bottomSelected[1][2]);  // 只有SEEK选项才启用seekMode
                radioState.displayNeedsUpdate = true;
                break;
            case 2:
                // SW频段 - 根据底部标签设置seekMode
                radio.SetFreqAM(radioState.swFreq);
                radioState.freq = radioState.swFreq;
                myTFT.fillRect(60, DISPLAY_HEIGHT - 18, 200, 18, myTFT.C_BLACK);
                radioState.seekMode = (radioState.bottomSelected[2][2]);  // 只有SEEK选项才启用seekMode
                radioState.displayNeedsUpdate = true;
                break;
        }
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
        Serial.println("ERROR: Radio initialization failed!");
        return false;
    }
    
    delay(500);
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
// ==================== 测试一下seek的时候显示FM的频率 ===================
uint16_t search(uint8_t up) {
    uint16_t mode = 20;
    uint16_t startFrequency = Radio_GetCurrentFreq();
    uint16_t seekStep = radioState.fmSeekStep ? FM_Step_100k : (FM_Step_100k / 2);  // 100K或50K步进

    while (true) {
        switch(mode){
            case 20:
                Radio_ChangeFreqOneStep(up,seekStep);
                Radio_SetFreq(Radio_SEARCHMODE, Radio_GetCurrentBand(), Radio_GetCurrentFreq());
                updateFrequency(120, (DISPLAY_HEIGHT - 50) / 2, Radio_GetCurrentFreq(), FontSixteenSeg);
            
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
// ==================== 主程序 ====================

void setup() {
    delay(50);
    Serial.begin(115200);
    Serial.println("\n=== FM Radio with Display ===");
    
    pinMode(0, OUTPUT);
    digitalWrite(0, HIGH);
    
    if (!initRadio()) while(1);
    if (!initDisplay()) while(1);
    
    initEncoder();
    
    radio.setFrequency(radioState.freq);
    radio.setVolume(-300);
    
    drawScreenLayout();
    radioState.displayNeedsUpdate = true;
    
    digitalWrite(0, LOW);
}

void loop() {
    static unsigned long lastUpdateTime = 0;
    static unsigned long lastSignalUpdateTime = 0;
    unsigned long currentTime = millis();
    
    // 处理按键
    updateButtonState();
    
    // 处理按键动作
    if (clickActionPending || doubleClickActionPending) {
        processButtonActions();
    }
    
    // 处理编码器
    static int32_t lastCount = 0;
    int32_t currentCount = encoder.getCount();
    
    if (currentCount != lastCount) {
        if (radioState.seekMode) {
            // 处理频率调整
            if (currentCount > 3) {
                switch(radioState.nextBand) {
                    case 0: // FM
                        search(true);
                        radioState.freq = radio.getFrequency();
                        radioState.fmFreq = radioState.freq;
                        encoder.reset();
                        break;
                    case 1: // AM
                        radioState.amFreq = radioState.amFreq + radioState.amStep;
                        if (radioState.amFreq > AM_MAX_FREQ) radioState.amFreq = AM_MIN_FREQ;
                        radio.SetFreqAM(radioState.amFreq);
                        radioState.freq = radioState.amFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                    case 2: // SW
                        radioState.swFreq = radioState.swFreq + radioState.swStep;
                        if (radioState.swFreq > SW_MAX_FREQ) radioState.swFreq = SW_MIN_FREQ;
                        radio.SetFreqAM(radioState.swFreq);
                        radioState.freq = radioState.swFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                }
            } else if (currentCount < -3) {
                switch(radioState.nextBand) {
                    case 0: // FM
                        search(false);
                        radioState.freq = radio.getFrequency();
                        radioState.fmFreq = radioState.freq;
                        encoder.reset();
                        break;
                    case 1: // AM
                        radioState.amFreq = radioState.amFreq - radioState.amStep;
                        if (radioState.amFreq < AM_MIN_FREQ) radioState.amFreq = AM_MAX_FREQ;
                        radio.SetFreqAM(radioState.amFreq);
                        radioState.freq = radioState.amFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                    case 2: // SW
                        radioState.swFreq = radioState.swFreq - radioState.swStep;
                        if (radioState.swFreq < SW_MIN_FREQ) radioState.swFreq = SW_MAX_FREQ;
                        radio.SetFreqAM(radioState.swFreq);
                        radioState.freq = radioState.swFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                }
            }
        } else {
            // 处理频率调整
            if (currentCount > 3) {
                switch(radioState.nextBand) {
                    case 0: // FM
                        radioState.fmFreq = radioState.fmFreq + FM_Step_100k;
                        if (radioState.fmFreq > FM_MAX_FREQ) radioState.fmFreq = FM_MIN_FREQ;
                        radio.setFrequency(radioState.fmFreq);
                        radioState.freq = radioState.fmFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                    case 1: // AM
                        radioState.amFreq = radioState.amFreq + radioState.amStep;
                        if (radioState.amFreq > AM_MAX_FREQ) radioState.amFreq = AM_MIN_FREQ;
                        radio.SetFreqAM(radioState.amFreq);
                        radioState.freq = radioState.amFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                    case 2: // SW
                        radioState.swFreq = radioState.swFreq + radioState.swStep;
                        if (radioState.swFreq > SW_MAX_FREQ) radioState.swFreq = SW_MIN_FREQ;
                        radio.SetFreqAM(radioState.swFreq);
                        radioState.freq = radioState.swFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                }
            } else if (currentCount < -3) {
                switch(radioState.nextBand) {
                    case 0: // FM
                        radioState.fmFreq = radioState.fmFreq - FM_Step_100k;
                        if (radioState.fmFreq < FM_MIN_FREQ) radioState.fmFreq = FM_MAX_FREQ;
                        radio.setFrequency(radioState.fmFreq);
                        radioState.freq = radioState.fmFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                    case 1: // AM
                        radioState.amFreq = radioState.amFreq - radioState.amStep;
                        if (radioState.amFreq < AM_MIN_FREQ) radioState.amFreq = AM_MAX_FREQ;
                        radio.SetFreqAM(radioState.amFreq);
                        radioState.freq = radioState.amFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                    case 2: // SW
                        radioState.swFreq = radioState.swFreq - radioState.swStep;
                        if (radioState.swFreq < SW_MIN_FREQ) radioState.swFreq = SW_MAX_FREQ;
                        radio.SetFreqAM(radioState.swFreq);
                        radioState.freq = radioState.swFreq;
                        radioState.displayNeedsUpdate = true;
                        encoder.reset();
                        break;
                }
            }
        }
        
        lastCount = currentCount;
    }
    
    // 更新频率显示
    if (radioState.displayNeedsUpdate) {
        updateFrequency(120, (DISPLAY_HEIGHT - 50) / 2, radioState.freq, FontSixteenSeg);
        radioState.displayNeedsUpdate = false;

        uint16_t signalLevel = radio.getLevel(1);
        bool stereoStatus = radio.getStereoStatus();
        updateSignal(DISPLAY_WIDTH - 52, 5, signalLevel);
    }
    
    // 更新信号显示
    if(radioState.nextBand == 0) {
        if (currentTime - lastSignalUpdateTime >= 500) {
            uint16_t signalLevel = radio.getLevel(1);
            bool stereoStatus = radio.getStereoStatus();
            updateSignal(DISPLAY_WIDTH - 52, 5, signalLevel);
            
            if (stereoStatus != radioState.lastStereoStatus) {
                myTFT.setFont(FontDefault);
                
                if (stereoStatus) {
                    myTFT.setTextColor(myTFT.C_GREEN, myTFT.C_BLACK);
                    myTFT.setCursor(5, DISPLAY_HEIGHT/2 + 5);
                    myTFT.print("STEREO");
                } else {
                    myTFT.setTextColor(myTFT.C_LGREY, myTFT.C_BLACK);
                    myTFT.setCursor(5, DISPLAY_HEIGHT/2 + 5);
                    myTFT.print("MONO   ");
                }
                
                radioState.lastStereoStatus = stereoStatus;
            }
            lastSignalUpdateTime = currentTime;
        }
    } else {
        if (currentTime - lastSignalUpdateTime >= 500) {
            uint16_t signalLevel = radio.getLevel(0);
            updateSignal(DISPLAY_WIDTH - 52, 5, signalLevel);

            myTFT.setFont(FontDefault);
            myTFT.setTextColor(myTFT.C_LGREY, myTFT.C_BLACK);
            myTFT.setCursor(5, DISPLAY_HEIGHT/2 + 5);
            myTFT.print("MONO   ");
            radioState.lastStereoStatus = 0;
            lastSignalUpdateTime = currentTime;
        }        

    }

    delay(10);
}