#ifndef __FAN_H__
#define __FAN_H__

/* 风扇档位与 PWM 占空比 (周期 10 tick) */
#define FAN_SPEED_LEVEL_OFF              ((unsigned char)0)
#define FAN_SPEED_LEVEL_LOW              ((unsigned char)1)
#define FAN_SPEED_LEVEL_MEDIUM           ((unsigned char)2)
#define FAN_SPEED_LEVEL_HIGH             ((unsigned char)3)
#define FAN_SPEED_LEVEL_MAX              FAN_SPEED_LEVEL_HIGH
#define FAN_PWM_PERIOD_TICKS             ((unsigned char)10)
#define FAN_PWM_LOW_DUTY_TICKS           ((unsigned char)6)
#define FAN_PWM_MEDIUM_DUTY_TICKS        ((unsigned char)8)
#define FAN_PWM_HIGH_DUTY_TICKS          ((unsigned char)10)

void Fan_Timer2Tick(void);
void Fan_UpdateOutput(unsigned char power_on, unsigned char speed_level);
void Fan_Stop(void);
#endif /* __FAN_H__ */
