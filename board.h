#ifndef __BOARD_H__
#define __BOARD_H__

#include <cms.h>

/* ===== Airclean_board 板级引脚定义 (CMS79F723) ===== */

/* TM1628A 三线接口: RA4=DIO, RA5=CLK, RA6=STB; DIO 读键时需释放为输入 */
#define TM1628_DIO     RA4
#define TM1628_CLK     RA5
#define TM1628_STB     RA6

/* 触摸按键输入 (电容触摸通道, 由 Touch_Kscan_Library 扫描) */
#define KEY_SPEED      RA0
#define KEY_FILTER     RA1
#define KEY_POWER      RA2
#define KEY_TIMER      RA3

/* 风扇控制: FAN_VCC=电源使能, FAN_PWM=软件 PWM 输出 */
#define FAN_VCC        RB0
#define FAN_PWM        RB3

/* H3 外设接口:
   GAS_VCC  = RB1     : 原理图网络为 CND, 当前固件按 0 为开启态处理
   GAS_DATA = RB2/AN13: 原理图网络为 SMELL, 作为模拟输入候选
   外接模块型号、供电方式和有效电平需以模块资料与实机确认 */
#define GAS_VCC        RB1
#define GAS_DATA       RB2

/* H3:SMELL 对应的 ADC 通道号 */
#define GAS_ADC_CHAN   ((unsigned char)13)

/* 按键有效电平: 1=高电平有效, 实际为低电平有效则改 0 */
#define KEY_PRESSED_LEVEL  1

/* 板级硬件初始化: 时钟、ANSEL、TRIS 方向、初始电平 */
void Board_Init(void);

#endif
