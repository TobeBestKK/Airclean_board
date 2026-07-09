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

/* Change to 0 if the real key signal is active-low. */
#define KEY_PRESSED_LEVEL  1

void Board_Init(void);

#endif