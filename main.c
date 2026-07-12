#include <cms.h>
#include "board.h"
#include "key.h"
#include "timer.h"
#include "tm1628.h"
#include "Touch_Kscan_Library.h"
#include "tuya_protocol.h"

/*
 * 空气净化器主板主程序 (CMS79F723)。
 * 功能：触摸按键控制 + 涂鸦 WiFi + 语音命令 + TM1628A 显示 + 风扇 PWM。
 * 主循环按以下顺序处理：触摸扫描 → WiFi 协议 → DP 下发 → 按键 → 语音 → 定时 → 风扇输出。
 */

/* 按键位掩码：bit0=K1(定时) bit1=K2(指示) bit2=K3(电源) bit3=K4(风速) */
#define KEY_MASK_K1  ((unsigned char)0x01)
#define KEY_MASK_K2  ((unsigned char)0x02)
#define KEY_MASK_K3  ((unsigned char)0x04)
#define KEY_MASK_K4  ((unsigned char)0x08)

/* LED 位掩码：bit0=LED1(定时图标) bit1=LED2(滤网指示) bit2=LED3(电源) bit3=LED4(风速) */
#define LED_MASK_1   ((unsigned char)0x01)
#define LED_MASK_2   ((unsigned char)0x02)
#define LED_MASK_3   ((unsigned char)0x04)
#define LED_MASK_4   ((unsigned char)0x08)

/* 定时与触摸扫描常量 */
#define TIMER_SECONDS_PER_HOUR           ((unsigned int)3600)
#define TIMER_MAX_HOURS                  ((unsigned char)24)
#define TOUCH_TMR2_TICKS_PER_SCAN        ((unsigned char)32)
#define UART_SPBRG_9600_16MHZ            ((unsigned char)103)

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

#define KEY_IS_PRESSED(key)  ((key) == KEY_PRESSED_LEVEL)

/* Timer2 中断驱动的软件 PWM 与触摸扫描计数器 */
static volatile unsigned char touch_tmr2_ticks;   /* Timer2 周期累计，达阈值触发触摸扫描 */
static volatile unsigned char fan_pwm_tick;       /* PWM 当前周期内 tick 计数 */
static volatile unsigned char fan_pwm_duty_ticks; /* PWM 占空比 (高电平 tick 数) */
static volatile unsigned char fan_pwm_enabled;    /* PWM 输出使能标志 */

/* 初始化 Timer2，按厂商触摸库要求的采样时序配置周期与中断。 */
static void Touch_Init(void)
{
    touch_tmr2_ticks = (unsigned char)0x00;
    TMR2IF = 0;
    PIE1 = (unsigned char)(PIE1 | 0B00000010);  /* TMR2IE = 1 */
    PR2 = (unsigned char)125;
    T2CON = (unsigned char)0x05;                /* 后分频 1:1, 预分频 1:4, 开 TMR2 */
    PEIE = 1;
    GIE = 1;
}

/* 轮询触摸扫描：累计 Timer2 周期达阈值时调用一次按键检测。 */
static void Touch_Poll(void)
{
    if (touch_tmr2_ticks >= TOUCH_TMR2_TICKS_PER_SCAN)
    {
        touch_tmr2_ticks = (unsigned char)0x00;
        __CMS_CheckTouchKey();
    }
}

/* 中断服务程序：Timer2 中断驱动风扇 PWM 与触摸扫描，UART 接收转发至 WIFI 模块。 */
void interrupt Touch_Timer2_ISR(void)
{
    if (TMR2IF != 0)
    {
        TMR2IF = 0;

        /* 软件 PWM：tick < duty 输出高，否则低；满周期归零 */
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

        /* 触摸扫描节拍累计 + 触摸库采样 */
        touch_tmr2_ticks++;
        __CMS_GetTouchKeyValue();
    }

    /* UART 接收中断：转发到 WIFI 模块环形缓冲区 */
    if (RCIF != 0)
    {
        WIFI_ISR_Rx();
    }
}

/* 程序入口与主循环：初始化硬件后循环处理按键、语音指令、定时与风扇 PWM 输出。 */
void main(void)
{
    unsigned char led_state;        /* 4 位 LED 状态 (与 LED_MASK_x 对应) */
    unsigned char key_now;          /* 本轮采样键值 */
    unsigned char key_last;         /* 上轮采样键值 (用于边沿检测) */
    unsigned char key_down;         /* 本次按下事件 (上升沿) */
    unsigned char voice_command;    /* 从队列取出的语音命令 */
    unsigned char fan_speed_level;  /* 当前风速档位 0~3 */
    unsigned char timer_hours;      /* 定时剩余小时 0~24 */
    unsigned int timer_seconds;     /* 当前小时内累计秒数 0~3599 */

    asm("nop");
    asm("clrwdt");

    Board_Init();
    TM1628_Init();
    Timer0_Init();
    Touch_Init();
    WIFI_Init();

    /* 开机默认关机状态 */
    led_state = (unsigned char)0x00;
    key_last = (unsigned char)0x00;
    fan_speed_level = FAN_SPEED_LEVEL_OFF;
    timer_hours = (unsigned char)0x00;
    timer_seconds = (unsigned int)0;

    TM1628_AllOff();
    TM1628_SetLeds(led_state);

    while (1)
    {
        asm("clrwdt");

        Touch_Poll();
        WIFI_Process();

        /* ---- 段1: 处理云端 DP 下发 ---- */
        if (wifi_dp_changed != (unsigned char)0x00)
        {
            wifi_dp_changed = (unsigned char)0x00;

            /* 开关 DP：同步本地开关机状态 */
            if (dp_power != (unsigned char)0x00)
            {
                if ((led_state & LED_MASK_3) == (unsigned char)0x00)
                {
                    led_state = LED_MASK_3;
                    fan_speed_level = FAN_SPEED_LEVEL_OFF;
                    timer_hours = (unsigned char)0x00;
                    timer_seconds = (unsigned int)0;
                    Timer0_ResetTick();
                    TM1628_SetDefaultDisplay();
                }
            }
            else
            {
                if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                {
                    led_state = (unsigned char)0x00;
                    fan_speed_level = FAN_SPEED_LEVEL_OFF;
                    timer_hours = (unsigned char)0x00;
                    timer_seconds = (unsigned int)0;
                    Timer0_ResetTick();
                    TM1628_AllOff();
                }
            }

            /* 仅在开机状态下同步其余 DP */
            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
            {
                /* 定时 DP：重置计时基准并更新显示 */
                timer_hours = dp_timer;
                timer_seconds = (unsigned int)0;
                Timer0_ResetTick();

                if (timer_hours != (unsigned char)0x00)
                {
                    led_state = (unsigned char)(led_state | LED_MASK_1);
                    TM1628_SetTimerDisplay(timer_hours, (unsigned char)0x01);
                }
                else
                {
                    led_state = (unsigned char)(led_state & (unsigned char)(~LED_MASK_1));
                    TM1628_SetTimerDisplay(timer_hours, (unsigned char)0x00);
                }

                /* 风速 DP：越界归零，更新 LED 与显示 */
                fan_speed_level = dp_fan_speed;
                if (fan_speed_level > FAN_SPEED_LEVEL_MAX)
                {
                    fan_speed_level = FAN_SPEED_LEVEL_OFF;
                }

                if (fan_speed_level != FAN_SPEED_LEVEL_OFF)
                {
                    led_state = (unsigned char)(led_state | LED_MASK_4);
                }
                else
                {
                    led_state = (unsigned char)(led_state & (unsigned char)(~LED_MASK_4));
                }
                TM1628_SetSpeedDisplay(fan_speed_level);

                /* 滤网指示 DP */
                if (dp_indicator != (unsigned char)0x00)
                {
                    led_state = (unsigned char)(led_state | LED_MASK_2);
                }
                else
                {
                    led_state = (unsigned char)(led_state & (unsigned char)(~LED_MASK_2));
                }
            }

            TM1628_SetLeds(led_state);
        }

        /* ---- 段2: 按键处理 ---- */
        key_now = Key_ReadStable();
        key_down = (unsigned char)(key_now & (unsigned char)(~key_last));  /* 上升沿 */

        if (key_down != (unsigned char)0x00)
        {
            /* K3 电源键：开关机切换并上报云端 */
            if ((key_down & KEY_MASK_K3) != (unsigned char)0x00)
            {
                if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                {
                    led_state = (unsigned char)0x00;
                    fan_speed_level = FAN_SPEED_LEVEL_OFF;
                    timer_hours = (unsigned char)0x00;
                    timer_seconds = (unsigned int)0;
                    Timer0_ResetTick();
                    TM1628_AllOff();
                    WIFI_ReportPower((unsigned char)0x00);
                }
                else
                {
                    led_state = LED_MASK_3;
                    fan_speed_level = FAN_SPEED_LEVEL_OFF;
                    timer_hours = (unsigned char)0x00;
                    timer_seconds = (unsigned int)0;
                    Timer0_ResetTick();
                    TM1628_SetDefaultDisplay();
                    WIFI_ReportPower((unsigned char)0x01);
                }
            }
            else if ((led_state & LED_MASK_3) != (unsigned char)0x00)
            {
                /* 以下按键仅在开机状态生效 */

                /* K1 定时键：小时数 0→24 循环递增，0 时关闭定时 */
                if ((key_down & KEY_MASK_K1) != (unsigned char)0x00)
                {
                    if (timer_hours < TIMER_MAX_HOURS)
                    {
                        timer_hours++;
                    }
                    else
                    {
                        timer_hours = (unsigned char)0x00;
                    }

                    timer_seconds = (unsigned int)0;
                    Timer0_ResetTick();

                    if (timer_hours == (unsigned char)0x00)
                    {
                        led_state = (unsigned char)(led_state & (unsigned char)(~LED_MASK_1));
                        TM1628_SetTimerDisplay(timer_hours, (unsigned char)0x00);
                    }
                    else
                    {
                        led_state = (unsigned char)(led_state | LED_MASK_1);
                        TM1628_SetTimerDisplay(timer_hours, (unsigned char)0x01);
                    }

                    WIFI_ReportTimer(timer_hours);
                }

                /* K2 滤网指示键：翻转指示灯并上报 */
                if ((key_down & KEY_MASK_K2) != (unsigned char)0x00)
                {
                    led_state = (unsigned char)(led_state ^ LED_MASK_2);
                    WIFI_ReportIndicator(
                        (led_state & LED_MASK_2) ? (unsigned char)0x01 : (unsigned char)0x00);
                }

                /* K4 风速键：档位 0→3 循环递增，0 时关风速 LED */
                if ((key_down & KEY_MASK_K4) != (unsigned char)0x00)
                {
                    fan_speed_level++;
                    if (fan_speed_level > FAN_SPEED_LEVEL_MAX)
                    {
                        fan_speed_level = FAN_SPEED_LEVEL_OFF;
                    }

                    if (fan_speed_level == FAN_SPEED_LEVEL_OFF)
                    {
                        led_state = (unsigned char)(led_state & (unsigned char)(~LED_MASK_4));
                    }
                    else
                    {
                        led_state = (unsigned char)(led_state | LED_MASK_4);
                    }

                    TM1628_SetSpeedDisplay(fan_speed_level);
                    WIFI_ReportFanSpeed(fan_speed_level);
                }
            }

            TM1628_SetLeds(led_state);
        }

        /* ---- 段3: 语音命令处理 (从队列逐条消费) ---- */
        while ((voice_command = WIFI_GetVoiceCommand()) != (unsigned char)0x00)
        {
            switch (voice_command)
            {
                        case VOICE_CMD_WAKEUP:
                            break;

                        /* 开机：仅在关机时执行，避免重复上报 */
                        case VOICE_CMD_TURN_ON:
                            if ((led_state & LED_MASK_3) == (unsigned char)0x00)
                            {
                                led_state = LED_MASK_3;
                                fan_speed_level = FAN_SPEED_LEVEL_OFF;
                                timer_hours = (unsigned char)0x00;
                                timer_seconds = (unsigned int)0;
                                Timer0_ResetTick();
                                TM1628_SetDefaultDisplay();
                                WIFI_ReportPower((unsigned char)0x01);
                            }
                            break;

                        /* 关机：无条件关闭所有输出 */
                        case VOICE_CMD_TURN_OFF:
                            led_state = (unsigned char)0x00;
                            fan_speed_level = FAN_SPEED_LEVEL_OFF;
                            timer_hours = (unsigned char)0x00;
                            timer_seconds = (unsigned int)0;
                            Timer0_ResetTick();
                            TM1628_AllOff();
                            WIFI_ReportPower((unsigned char)0x00);
                            break;

                        /* 加档：仅在开机时生效，已达最高档不动作 */
                        case VOICE_CMD_SPEED_UP:
                            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                            {
                                if (fan_speed_level < FAN_SPEED_LEVEL_MAX)
                                {
                                    fan_speed_level++;
                                }

                                if (fan_speed_level != FAN_SPEED_LEVEL_OFF)
                                {
                                    led_state = (unsigned char)(led_state | LED_MASK_4);
                                }

                                TM1628_SetSpeedDisplay(fan_speed_level);
                                WIFI_ReportFanSpeed(fan_speed_level);
                            }
                            break;

                        /* 减档：仅在开机时生效，已为关不动作 */
                        case VOICE_CMD_SPEED_DOWN:
                            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                            {
                                if (fan_speed_level > FAN_SPEED_LEVEL_OFF)
                                {
                                    fan_speed_level--;
                                }

                                if (fan_speed_level == FAN_SPEED_LEVEL_OFF)
                                {
                                    led_state = (unsigned char)(led_state & (unsigned char)(~LED_MASK_4));
                                }

                                TM1628_SetSpeedDisplay(fan_speed_level);
                                WIFI_ReportFanSpeed(fan_speed_level);
                            }
                            break;

                        /* 定时加一小时 (逻辑同 K1) */
                        case VOICE_CMD_TIMER_ON:
                            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                            {
                                if (timer_hours < TIMER_MAX_HOURS)
                                {
                                    timer_hours++;
                                }
                                else
                                {
                                    timer_hours = (unsigned char)0x00;
                                }

                                timer_seconds = (unsigned int)0;
                                Timer0_ResetTick();

                                if (timer_hours == (unsigned char)0x00)
                                {
                                    led_state = (unsigned char)(led_state & (unsigned char)(~LED_MASK_1));
                                    TM1628_SetTimerDisplay(timer_hours, (unsigned char)0x00);
                                }
                                else
                                {
                                    led_state = (unsigned char)(led_state | LED_MASK_1);
                                    TM1628_SetTimerDisplay(timer_hours, (unsigned char)0x01);
                                }

                                WIFI_ReportTimer(timer_hours);
                            }
                            break;

                        /* 滤网指示翻转 (逻辑同 K2) */
                        case VOICE_CMD_FILTER:
                            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                            {
                                led_state = (unsigned char)(led_state ^ LED_MASK_2);
                                WIFI_ReportIndicator(
                                    (led_state & LED_MASK_2) ? (unsigned char)0x01 : (unsigned char)0x00);
                            }
                            break;

                        default:
                            break;
            }

            TM1628_SetLeds(led_state);
        }

        /* ---- 段4: 定时倒计时 ---- */
        if (Timer0_PollSecond() != (unsigned char)0x00)
        {
            if (timer_hours != (unsigned char)0x00)
            {
                timer_seconds++;

                /* 累满一小时：小时数递减 */
                if (timer_seconds >= TIMER_SECONDS_PER_HOUR)
                {
                    timer_seconds = (unsigned int)0;
                    timer_hours--;

                    /* 归零：关机并上报 */
                    if (timer_hours == (unsigned char)0x00)
                    {
                        led_state = (unsigned char)0x00;
                        fan_speed_level = FAN_SPEED_LEVEL_OFF;
                        Timer0_ResetTick();
                        TM1628_AllOff();
                        WIFI_ReportPower((unsigned char)0x00);
                        WIFI_ReportTimer((unsigned char)0x00);
                    }
                    else
                    {
                        TM1628_SetTimerDisplay(timer_hours, (unsigned char)0x01);
                        WIFI_ReportTimer(timer_hours);
                    }
                }
            }
        }

        key_last = key_now;

        /* ---- 段5: 风扇 PWM 输出 ---- */
        if (((led_state & LED_MASK_3) != (unsigned char)0x00)
            && (fan_speed_level != FAN_SPEED_LEVEL_OFF))
        {
            FAN_VCC = 1;  /* 接通风扇电源 */

            /* 按档位选择占空比 */
            if (fan_speed_level == FAN_SPEED_LEVEL_LOW)
            {
                fan_pwm_duty_ticks = FAN_PWM_LOW_DUTY_TICKS;
            }
            else if (fan_speed_level == FAN_SPEED_LEVEL_MEDIUM)
            {
                fan_pwm_duty_ticks = FAN_PWM_MEDIUM_DUTY_TICKS;
            }
            else
            {
                fan_pwm_duty_ticks = FAN_PWM_HIGH_DUTY_TICKS;
            }

            fan_pwm_enabled = (unsigned char)0x01;  /* ISR 中开始输出 PWM */
        }
        else
        {
            /* 关机或风速为 0：断电并停 PWM */
            fan_pwm_enabled = (unsigned char)0x00;
            fan_pwm_duty_ticks = (unsigned char)0x00;
            FAN_VCC = 0;
            FAN_PWM = 0;
        }
    }
}
