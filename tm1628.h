#ifndef __TM1628_H__
#define __TM1628_H__

/* TM1628A 显示驱动 (7 Grid x 11 Segment, 三线 SPI-like 接口) */

/* LED 掩码: bit0=LED1(定时图标) bit1=LED2(滤网指示) bit2=LED3(电源) bit3=LED4(风速) */
#define LED_MASK_1   ((unsigned char)0x01)
#define LED_MASK_2   ((unsigned char)0x02)
#define LED_MASK_3   ((unsigned char)0x04)
#define LED_MASK_4   ((unsigned char)0x08)

void TM1628_Init(void);
void TM1628_Clear(void);
void TM1628_AllOn(void);
void TM1628_AllOff(void);
void TM1628_DigitsAllOn(void);
void TM1628_SetLeds(unsigned char leds);
void TM1628_SetSpeedDisplay(unsigned char value);
void TM1628_SetTimerDisplay(unsigned char value, unsigned char enabled);
void TM1628_SetDefaultDisplay(void);
void TM1628_SetApDisplay(void);

/* PM2.5 显示: 0~999 → DIG7(百)/DIG6(十)/DIG5(个), DP 全亮作 PM2.5/μg/m³ 图标 */
void TM1628_SetPm25Display(unsigned int value);

/* 滤网使用度显示: 0~100 → DIG7/DIG6/DIG5 三位 (与 PM2.5 共用位),
   DIG1/DIG2 的 DP 熄灭, DIG3 的 DP 点亮作 "滤网%" 图标;
   超过 100 显示 100; 不使用等值缓存, 确保视图切换时重写三位 */
void TM1628_SetFilterUsageDisplay(unsigned char value);

/* 设置亮度: level 0=低, 1=中, 2=高, 3=省电熄屏。
   命令格式 0x80|pulse, pulse 对应占空比 1/16~14/16 */
void TM1628_SetBrightness(unsigned char level);

void TM1628_WriteByteToAddr(unsigned char addr, unsigned char dat);
void TM1628_WalkingTest(void);

#endif
