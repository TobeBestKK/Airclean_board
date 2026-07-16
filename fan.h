#ifndef __FAN_H__
#define __FAN_H__

/* 风扇档位 (power_on=1 时由 speed_level 选择 PWM 占空比) */
#define FAN_SPEED_LEVEL_OFF              ((unsigned char)0)
#define FAN_SPEED_LEVEL_LOW              ((unsigned char)1)
#define FAN_SPEED_LEVEL_MEDIUM           ((unsigned char)2)
#define FAN_SPEED_LEVEL_HIGH             ((unsigned char)3)
#define FAN_SPEED_LEVEL_MAX              FAN_SPEED_LEVEL_HIGH

/* PWM 周期与各档占空比 (单位: Timer2 tick, 周期 10 tick) */
#define FAN_PWM_PERIOD_TICKS             ((unsigned char)10)
#define FAN_PWM_LOW_DUTY_TICKS           ((unsigned char)6)
#define FAN_PWM_MEDIUM_DUTY_TICKS        ((unsigned char)8)
#define FAN_PWM_HIGH_DUTY_TICKS          ((unsigned char)10)

/* Timer2 周期中断调用: 软件 PWM 翻转 FAN_PWM */
void Fan_Timer2Tick(void);

/* 按电源状态与档位更新风扇电源及 PWM 占空比 */
void Fan_UpdateOutput(unsigned char power_on, unsigned char speed_level);

/* 停止 PWM 并关闭风扇电源 */
void Fan_Stop(void);

#endif /* __FAN_H__ */
