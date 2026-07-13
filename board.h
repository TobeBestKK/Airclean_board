#ifndef __BOARD_H__
#define __BOARD_H__

#include <cms.h>

/*
 * Airclean_board 显示驱动接口
 * RA4 = DIO
 * RA5 = CLK
 * RA6 = STB
 */
#define TM1628_DIO     RA4
#define TM1628_CLK     RA5
#define TM1628_STB     RA6

#define KEY_SPEED      RA0
#define KEY_FILTER     RA1
#define KEY_POWER      RA2
#define KEY_TIMER      RA3

#define FAN_VCC        RB0
#define FAN_PWM        RB3

/* TP-401W 异味传感器接口
 * GAS_VCC  = RB1 : 传感器电源控制，输出 0 时给传感器供电（低电平有效）
 * GAS_DATA = RB2/AN13 : 传感器模拟输出，经 ADC 采集异味浓度
 */
#define GAS_VCC        RB1
#define GAS_DATA       RB2

/* AN13 对应的通道号 (CMS79F723: AN13 = channel 13) */
#define GAS_ADC_CHAN   ((unsigned char)13)

/* Change to 0 if the real key signal is active-low. */
#define KEY_PRESSED_LEVEL  1

void Board_Init(void);

#endif