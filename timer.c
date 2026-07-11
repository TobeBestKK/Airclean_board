#include <cms.h>
#include "timer.h"

#define TIMER_TMR0_OVERFLOWS_PER_SECOND  ((unsigned char)61)

static unsigned char timer_tmr0_overflows;

/*
 * 复位 Timer0 计时基准。
 */
void Timer0_ResetTick(void)
{
    TMR0 = (unsigned char)0x00;
    T0IF = 0;
    timer_tmr0_overflows = (unsigned char)0x00;
}

/*
 * 初始化 Timer0，并使用 1:256 预分频进行秒级计时。
 */
void Timer0_Init(void)
{
    T0CS = 0;
    T0SE = 0;
    PSA = 0;
    PS2 = 1;
    PS1 = 1;
    PS0 = 1;
    T0IE = 0;

    Timer0_ResetTick();
}

/*
 * 累计 Timer0 溢出；每约 61 次返回一次秒节拍。
 */
unsigned char Timer0_PollSecond(void)
{
    if (T0IF != 0)
    {
        T0IF = 0;
        timer_tmr0_overflows++;

        if (timer_tmr0_overflows >= TIMER_TMR0_OVERFLOWS_PER_SECOND)
        {
            timer_tmr0_overflows = (unsigned char)0x00;
            return (unsigned char)0x01;
        }
    }

    return (unsigned char)0x00;
}
