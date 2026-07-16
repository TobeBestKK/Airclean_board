#ifndef __TIMER_H__
#define __TIMER_H__

/* Timer0 秒级节拍 + Timer2 周期中断时基。
   Timer0: 1:256 预分频, Fosc=16MHz、指令周期=Fosc/4=4MHz, ~61 次溢出 = 1 秒。
   Timer2: PR2=125, 1:4 预分频, 周期中断供触摸扫描与风扇 PWM 共用。 */

/* 初始化 Timer0 (1:256 预分频, 软件累计溢出产生秒节拍) */
void Timer0_Init(void);

/* 清零秒节拍累计 */
void Timer0_ResetTick(void);

/* 在主循环轮询: 返回 1 表示到达 1 秒, 0 表示未到 */
unsigned char Timer0_PollSecond(void);

/* 初始化 Timer2: 触摸扫描与风扇 PWM 共用的周期中断时基 */
void Timer2_Init(void);

#endif
