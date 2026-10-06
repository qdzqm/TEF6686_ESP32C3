#ifndef TEF6686_h
#define TEF6686_h

#include <Wire.h>

#include "Tuner_Api.h"
#include "Tuner_Drv_Lithio.h"
#include "Tuner_Interface.h"

#define I2C_PORT 1
#define I2C_ADDR 0x64

class TEF6686 {
  public:
    TEF6686();
    uint8_t init();
    void powerOn();					// call in setup
    void powerOff();				
    void setFrequency(uint16_t frequency);    // frequency as int, i.e. 100.00 as 10000
    uint16_t getFrequency(); // returns the current frequency
    int16_t getLevel(uint8_t band);    // 返回信号电平 dBuV（可能为负）
    uint8_t getStereoStatus();
    void setVolume(int16_t volume); 	// -600 -- +240 (0.1 dB step)
    void setMute();
    void setUnMute();
    void SetFreqMW(uint16_t frequency);
    void SetFreqSW(uint16_t frequency);
};

#endif
