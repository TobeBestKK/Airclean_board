#include "board.h"
#include "fan.h"

/* Timer2 中断驱动的软件 PWM 状态 */
static volatile unsigned char fan_pwm_tick;
static volatile unsigned char fan_pwm_duty_ticks;
static volatile unsigned char fan_pwm_enabled;

/* 停止 PWM 并关闭风扇电源 */
void Fan_Stop(void)
{
    fan_pwm_enabled = (unsigned char)0x00;
    fan_pwm_duty_ticks = (unsigned char)0x00;
    FAN_VCC = 0;
    FAN_PWM = 0;
}

/* 按电源状态与风速档位更新风扇电源及 PWM 占空比 */
void Fan_UpdateOutput(unsigned char power_on, unsigned char speed_level)
{
    if ((power_on != (unsigned char)0x00)
        && (speed_level != FAN_SPEED_LEVEL_OFF))
    {
        FAN_VCC = 1;

        if (speed_level == FAN_SPEED_LEVEL_LOW)
        {
            fan_pwm_duty_ticks = FAN_PWM_LOW_DUTY_TICKS;
        }
        else if (speed_level == FAN_SPEED_LEVEL_MEDIUM)
        {
            fan_pwm_duty_ticks = FAN_PWM_MEDIUM_DUTY_TICKS;
        }
        else
        {
            fan_pwm_duty_ticks = FAN_PWM_HIGH_DUTY_TICKS;
        }

        fan_pwm_enabled = (unsigned char)0x01;
    }
    else
    {
        Fan_Stop();
    }
}

/* Timer2 每次中断执行一次 PWM tick: 计数 < duty 则置高, 否则拉低; 计满周期清零 */
void Fan_Timer2Tick(void)
{
    if (fan_pwm_enabled != (unsigned char)0x00)
    {
        if (fan_pwm_tick < fan_pwm_duty_ticks)
        {
            FAN_PWM = 1;
        }
        else
        {
            FAN_PWM = 0;
        }

        fan_pwm_tick++;
        if (fan_pwm_tick >= FAN_PWM_PERIOD_TICKS)
        {
            fan_pwm_tick = (unsigned char)0x00;
        }
    }
    else
    {
        fan_pwm_tick = (unsigned char)0x00;
        FAN_PWM = 0;
    }
}
