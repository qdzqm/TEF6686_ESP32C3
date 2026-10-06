#include "TEF6686.h"
#include <Arduino.h>

TEF6686::TEF6686() {
}

uint8_t TEF6686::init() {
  uint8_t result;
  uint8_t status;

  Tuner_I2C_Init();

  delay(5);
  for (uint8_t counter = 0; counter <= 50; ++counter) {
    result = devTEF668x_APPL_Get_Operation_Status(&status);
    if (result == 1) {
      uint8_t initRetries = 0;
      while (Tuner_Init() != 1 && ++initRetries < 50) {
        delay(5);
      }
      if (initRetries >= 50) return 2;

      powerOff();
      return 1; //Ok
    }
    delay(5);
  }
  return 2; //Doesn't exist
}

void TEF6686::powerOn() {
  devTEF668x_APPL_Set_OperationMode(0);
}

void TEF6686::powerOff() {
  devTEF668x_APPL_Set_OperationMode(1);
}

void TEF6686::setFrequency(uint16_t frequency) {
  Radio_SetFreq(Radio_PRESETMODE, FM_BAND, frequency);
}

void TEF6686::SetFreqMW(uint16_t frequency) {
  // devTEF668x_Radio_Tune_AM(frequency);
  Radio_SetFreq(Radio_PRESETMODE, MW_BAND, frequency);
}

void TEF6686::SetFreqSW(uint16_t frequency) {
  // devTEF668x_Radio_Tune_AM(frequency);
  Radio_SetFreq(Radio_PRESETMODE, SW_BAND, frequency);
}
uint16_t TEF6686::getFrequency() {
  return Radio_GetCurrentFreq();
}

uint16_t TEF6686::getLevel(uint8_t band) {
  return Radio_Get_Level(band);
}

uint8_t TEF6686::getStereoStatus() {
  return Radio_CheckStereo();
}

void TEF6686::setVolume(int16_t volume) {
  devTEF668x_Audio_Set_Volume(volume);
}

void TEF6686::setMute() {
  devTEF668x_Audio_Set_Mute(1);
}

void TEF6686::setUnMute() {
  devTEF668x_Audio_Set_Mute(0);
}
