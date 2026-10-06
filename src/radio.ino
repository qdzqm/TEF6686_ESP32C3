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


// 显示尺寸常量
const uint16_t DISPLAY_WIDTH = 320;   // 旋转270度后的宽度
const uint16_t DISPLAY_HEIGHT = 240;  // 旋转270度后的高度

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

// 搜台中止请求：中断里置位，loop 里处理（中断中不能操作 I2C）
volatile bool seekStopRequested = false;
volatile bool seekStopByButton = false;
bool buttonIgnoreUntilRelease = false;

// 按钮常量
const unsigned long DEBOUNCE_TIME = 20;
const unsigned long CLICK_MAX_TIME = 300;
const unsigned long DOUBLE_CLICK_GAP = 400;
const unsigned long LONG_PRESS_TIME = 800;
const unsigned long SEEK_ENCODER_GRACE = 100; // 启动搜台后编码器静止多久才接受旋转中止

// ==================== 显示函数 ====================

// 频率显示函数
void updateFrequency(int start_x, int start_y, uint16_t freq, const uint8_t* font) {
    if (freq == radioState.lastDisplayedFreq) return;
    
    myTFT.setFont(font);
    myTFT.setTextColor(myTFT.C_WHITE, myTFT.C_BLACK);
    
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
            
            myTFT.fillRect(pos1, start_y, 32, 50, myTFT.C_BLACK);
            myTFT.setCursor(pos2, start_y);
            myTFT.print(tens);
            myTFT.setCursor(pos3, start_y);
            myTFT.print(units);
        }
        
        int dotY = start_y + 50 - 10;
        myTFT.fillRect(pos4, start_y, 32, 50, myTFT.C_BLACK);
        myTFT.fillRect(dotPos, dotY, 6, 6, myTFT.C_WHITE);
        
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
            myTFT.fillRect(pos5, start_y, 33+10, 50, myTFT.C_BLACK);
            
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
            myTFT.fillRect(pos4, start_y, 32*2+10, 50, myTFT.C_BLACK);
            
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
            if (signalBars <= 1) barColor = myTFT.C_WHITE;
            else if (signalBars <= 3) barColor = myTFT.C_WHITE;
            else barColor = myTFT.C_WHITE;
            
            myTFT.fillRect(barX, barY, BAR_WIDTH, barHeight, barColor);
            myTFT.fillRect(barX, barY, BAR_WIDTH, 1, myTFT.C_BLACK);
        } else {
            myTFT.fillRect(barX, barY, BAR_WIDTH, barHeight, myTFT.C_BLACK);
            myTFT.drawRectWH(barX, barY, BAR_WIDTH, barHeight, myTFT.C_DGREY);
        }
    }
    
    radioState.lastSignalLevel = level;
}

// ==================== 界面绘制函数 ====================

void drawScreenLayout() {
    myTFT.fillScreen(myTFT.C_BLACK);
    
    myTFT.setFont(FontHallfetica);
    int startX = 0;
    for (int i = 0; i < 3; i++) {
        uint16_t color = radioState.topSelected[i] ? myTFT.C_WHITE : myTFT.C_DGREY;
        myTFT.setTextColor(color, myTFT.C_BLACK);
        myTFT.setCursor(startX, 10);
        myTFT.print(topLabels[i]);
        startX += (strlen(topLabels[i]) * 16) + 32;
    }
    
    updateBottomLabels();
    
    myTFT.fillRect(5, DISPLAY_HEIGHT/2 + 5, 90, 20, myTFT.C_BLACK);
    myTFT.setTextColor(myTFT.C_LGREY, myTFT.C_BLACK);
    myTFT.setCursor(5, DISPLAY_HEIGHT/2 + 5);
    myTFT.print("MONO");
}

void updateTopLabels() {
    myTFT.setFont(FontHallfetica);
    int startX = 0;
    for (int i = 0; i < 3; i++) {
        uint16_t color = radioState.topSelected[i] ? myTFT.C_WHITE : myTFT.C_DGREY;
        myTFT.setTextColor(color, myTFT.C_BLACK);
        myTFT.setCursor(startX, 10);
        myTFT.print(topLabels[i]);
        startX += (strlen(topLabels[i]) * 16) + 32;
    }
}

void updateBottomLabels() {
    myTFT.setFont(FontDefault);
    int startX = 80;
    
    myTFT.fillRect(60, DISPLAY_HEIGHT - 18, 260, 18, myTFT.C_BLACK);
    
    for (int j = 0; j < 3; j++) {
        uint16_t color = radioState.bottomSelected[radioState.nextBand][j] ? myTFT.C_WHITE : myTFT.C_DGREY;
        myTFT.setTextColor(color, myTFT.C_BLACK);
        myTFT.setCursor(startX, DISPLAY_HEIGHT - 13);
        myTFT.print(bottomLabels[radioState.nextBand][j]);
        startX += (strlen(bottomLabels[radioState.nextBand][j]) * 8) + 16;
    }
}

// ==================== 按键处理函数 ====================

void seekStop();

// 按当前波段选中的底部选项，确定 seekMode（FM 前两项为搜索，MW/SW 第三项为搜索）
void syncSeekModeForBand(int band) {
    switch (band) {
        case 0:
            radioState.seekMode = radioState.bottomSelected[0][0] ||
                                  radioState.bottomSelected[0][1];
            break;
        case 1:
            radioState.seekMode = radioState.bottomSelected[1][2];
            break;
        default:
            radioState.seekMode = radioState.bottomSelected[2][2];
            break;
    }
}

void updateButtonState() {
    static bool lastButtonState = HIGH;
    bool currentButtonState = digitalRead(BUTTON_PIN);
    unsigned long currentTime = millis();

    // 按键中止搜台后，本次按压完全忽略直到松手（不产生单击/双击/长按）
    if (buttonIgnoreUntilRelease) {
        lastButtonState = currentButtonState;
        if (currentButtonState == HIGH) buttonIgnoreUntilRelease = false;
        return;
    }
    
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
                        break;
                    case 1:
                        radio.SetFreqMW(radioState.amFreq);
                        radioState.freq = radioState.amFreq;
                        break;
                    default:
                        radio.SetFreqSW(radioState.swFreq);
                        radioState.freq = radioState.swFreq;
                        break;
                }

                syncSeekModeForBand(radioState.nextBand);
                // FM 与 SW 的数值可能相同（如都为 9000），强制重绘避免显示缓存命中
                radioState.lastDisplayedFreq = 0xFFFF;
                radioState.displayNeedsUpdate = true;
                
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
    }
    
    if (doubleClickActionPending) {
        doubleClickActionPending = false;
        
        if (radioState.bottomSelected[0][0] || radioState.bottomSelected[0][1]) {
            radioState.seekMode = true;
        } else {
            radioState.seekMode = false;
        }
        
        switch(radioState.nextBand) {
            case 0:
                radio.setFrequency(radioState.fmFreq);
                radioState.freq = radioState.fmFreq;
                radioState.displayNeedsUpdate = true;
                break;
            case 1:
                radio.SetFreqMW(radioState.amFreq);
                radioState.freq = radioState.amFreq;
                radioState.seekMode = radioState.bottomSelected[1][2];
                radioState.displayNeedsUpdate = true;
                break;
            case 2:
                radio.SetFreqSW(radioState.swFreq);
                radioState.freq = radioState.swFreq;
                radioState.seekMode = radioState.bottomSelected[2][2];
                radioState.displayNeedsUpdate = true;
                break;
        }
    }
}

// 搜台任务状态（ISR 要访问，定义在中断函数之前）
struct SeekJob {
    volatile bool active = false;
    volatile bool armed = false;      // 启动旋转结束后才武装，防止残余边沿误中止
    uint8_t phase = 0;          // 20:频率步进 30:等待+检测 40:结果判定 50:锁定频率
    uint8_t band = 0;
    bool up = true;
    uint16_t startFreq = 0;
    uint16_t step = 0;
    unsigned long phaseTime = 0;
    volatile unsigned long lastEncoderActivity = 0;
};

SeekJob seekJob;

// ==================== 硬件初始化 ====================

void IRAM_ATTR encoderISR() {
    static unsigned long lastInterruptTime = 0;
    unsigned long interruptTime = millis();
    
    if (interruptTime - lastInterruptTime < 5) return;
    
    encoder.update(ENCODER_PIN_A, ENCODER_PIN_B);
    
    lastInterruptTime = interruptTime;

    // 仅在搜台中、且启动旋转已静止（armed）后，新的转动才请求中止
    if (seekJob.active) {
        seekJob.lastEncoderActivity = interruptTime;
        if (seekJob.armed) seekStopRequested = true;
    }
}

// 按键按下沿（FALLING）：立即请求中止搜台，无需等待松手和单击判定
void IRAM_ATTR buttonISR() {
    seekStopRequested = true;
    seekStopByButton = true;
}

bool initRadio() {
    if (!radio.init()) {
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
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);
}

// ==================== 搜台（非阻塞状态机） ====================
// 每次 loop 只推进一步，搜台过程中按键（单击/双击/长按）仍可响应，
// 任意按键动作都会中止当前搜台。
static uint16_t seekGetStep(uint8_t band) {
    switch (band) {
        case 0:
            return radioState.fmSeekStep ? FM_Step_100k : (FM_Step_100k / 2);
        case 1:
            return AM_Step_9k;
        default:
            return 5;
    }
}

static unsigned long seekGetSettleTime(uint8_t band) {
    return band == 0 ? 20 : 40;
}

void seekStart(bool up) {
    if (seekJob.active) return;

    seekJob.band = radioState.nextBand;
    seekJob.up = up;
    seekJob.step = seekGetStep(seekJob.band);

    // MW 频道按 9kHz 间隔排列：搜台前先把频率对齐到 9kHz 栅格
    if (seekJob.band == MW_BAND && (radioState.amFreq % AM_Step_9k) != 0) {
        radioState.amFreq -= radioState.amFreq % AM_Step_9k;
        radio.SetFreqMW(radioState.amFreq);
    }

    seekJob.startFreq = Radio_GetCurrentFreq();
    seekJob.phase = 20;
    seekJob.lastEncoderActivity = millis();
    seekJob.armed = false;
    seekJob.active = true;

    encoder.reset();

    noInterrupts();
    seekStopRequested = false;
    seekStopByButton = false;
    interrupts();

    Serial.print("Seek start: band=");
    Serial.print(topLabels[seekJob.band]);
    Serial.print(up ? " up" : " down");
    Serial.print(" freq=");
    Serial.print(seekJob.startFreq);
    Serial.print(" step=");
    Serial.println(seekJob.step);
}

// 中止搜台：调谐器切到 PRESET 模式，保持当前频率
void seekStop() {
    if (!seekJob.active) return;

    uint16_t stopFreq = Radio_GetCurrentFreq();
    Serial.print("Seek aborted, freq=");
    Serial.println(stopFreq);

    Radio_SetFreq(Radio_PRESETMODE, seekJob.band, stopFreq);

    switch (seekJob.band) {
        case 0: radioState.fmFreq = stopFreq; break;
        case 1: radioState.amFreq = stopFreq; break;
        default: radioState.swFreq = stopFreq; break;
    }
    radioState.freq = stopFreq;
    radioState.displayNeedsUpdate = true;

    seekJob.active = false;
    encoder.reset();
}

void seekTick() {
    if (!seekJob.active) return;

    switch (seekJob.phase) {
        case 20:
            Radio_ChangeFreqOneStep(seekJob.up, seekJob.step);
            Radio_SetFreq(Radio_SEARCHMODE, seekJob.band, Radio_GetCurrentFreq());
            updateFrequency(120, (DISPLAY_HEIGHT - 50) / 2, Radio_GetCurrentFreq(), FontSixteenSeg);
            Serial.print("Seek step -> ");
            Serial.println(Radio_GetCurrentFreq());

            Radio_CheckStationInit();
            Radio_ClearCurrentStation();

            seekJob.phaseTime = millis();
            seekJob.phase = 30;
            break;

        case 30:
            if (millis() - seekJob.phaseTime < seekGetSettleTime(seekJob.band)) break;

            Radio_CheckStation();
            seekJob.phaseTime = millis();

            if (Radio_CheckStationStatus() >= NO_STATION) {
                seekJob.phase = 40;
            }
            break;

        case 40:
            if (Radio_CheckStationStatus() == NO_STATION) {
                seekJob.phase = (seekJob.startFreq == Radio_GetCurrentFreq()) ? 50 : 20;
            } else if (Radio_CheckStationStatus() == PRESENT_STATION) {
                seekJob.phase = 50;
            }
            break;

        case 50: {
            Radio_SetFreq(Radio_PRESETMODE, seekJob.band, Radio_GetCurrentFreq());

            uint16_t foundFreq = Radio_GetCurrentFreq();
            switch (seekJob.band) {
                case 0: radioState.fmFreq = foundFreq; break;
                case 1: radioState.amFreq = foundFreq; break;
                default: radioState.swFreq = foundFreq; break;
            }
            radioState.freq = foundFreq;
            radioState.displayNeedsUpdate = true;

            Serial.print("Seek done, freq=");
            Serial.println(foundFreq);

            seekJob.active = false;
            encoder.reset();
            break;
        }
    }
}
// ==================== 主程序 ====================

void setup() {
    delay(50);
    Serial.begin(115200);
    Serial.println("ESP32 TEF6686 radio booting...");

    pinMode(0, OUTPUT);
    digitalWrite(0, HIGH);
    
    if (!initRadio()) {
        Serial.println("Radio init failed, halt");
        while(1);
    }
    if (!initDisplay()) {
        Serial.println("Display init failed, halt");
        while(1);
    }
    
    initEncoder();
    
    radio.setFrequency(radioState.freq);
    radio.setVolume(-220);
    
    drawScreenLayout();
    radioState.displayNeedsUpdate = true;
    
    digitalWrite(0, LOW);
    Serial.println("Setup done");
}

void loop() {
    static unsigned long lastSignalUpdateTime = 0;

    unsigned long currentTime = millis();

    // 编码器静止超过宽限时间后才武装中止检测，跳过启动搜台的同一次旋转
    if (seekJob.active && !seekJob.armed &&
        currentTime - seekJob.lastEncoderActivity >= SEEK_ENCODER_GRACE) {
        seekJob.armed = true;
    }

    // 搜台中：中断（按键按下沿 / 编码器转动）请求中止 -> 立即处理
    if (seekJob.active && seekStopRequested) {
        bool byButton = seekStopByButton;
        seekStop();
        if (byButton) {
            // 吞掉本次按键：不切换搜索/调谐，也不产生双击/长按
            buttonState = BUTTON_IDLE;
            clickActionPending = false;
            doubleClickActionPending = false;
            buttonIgnoreUntilRelease = true;
        }
    }
    if (!seekJob.active) {
        seekStopRequested = false;
        seekStopByButton = false;
    }
    
    updateButtonState();
    
    if (clickActionPending || doubleClickActionPending) {
        processButtonActions();
    }

    seekTick();
    
    static int32_t lastCount = 0;
    int32_t currentCount = encoder.getCount();
    
    if (!seekJob.active && currentCount != lastCount) {
        if (radioState.seekMode) {
            if (currentCount > 3) {
                seekStart(true);
            } else if (currentCount < -3) {
                seekStart(false);
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
        
        lastCount = currentCount;
    }
    
    if (radioState.displayNeedsUpdate) {
        updateFrequency(120, (DISPLAY_HEIGHT - 50) / 2, radioState.freq, FontSixteenSeg);
        radioState.displayNeedsUpdate = false;
    }
    
    if(radioState.nextBand == 0) {
        if (currentTime - lastSignalUpdateTime >= 500) {
            uint16_t signalLevel = radio.getLevel(1);
            bool stereoStatus = radio.getStereoStatus();
            updateSignal(DISPLAY_WIDTH - 52, 5, signalLevel);
            
            if (stereoStatus != radioState.lastStereoStatus) {
                myTFT.setFont(FontDefault);
                
                if (stereoStatus) {
                    myTFT.setTextColor(myTFT.C_WHITE, myTFT.C_BLACK);
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
            radioState.lastStereoStatus = false;
            lastSignalUpdateTime = currentTime;
        }        
    }
    delay(10);
}
