#ifndef __TM1628_H__
#define __TM1628_H__

void TM1628_Init(void);
void TM1628_Clear(void);
void TM1628_AllOn(void);
void TM1628_AllOff(void);
void TM1628_DigitsAllOn(void);
void TM1628_SetLeds(unsigned char leds);
void TM1628_SetTimerDisplay(unsigned char value, unsigned char enabled);
void TM1628_SetDefaultDisplay(void);
void TM1628_WriteByteToAddr(unsigned char addr, unsigned char dat);
void TM1628_WalkingTest(void);

#endif