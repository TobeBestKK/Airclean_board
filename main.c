#include <cms.h>
#include "board.h"
#include "timer.h"
#include "tm1628.h"
#include "Touch_Kscan_Library.h"
#include "tuya_protocol.h"

#define KEY_MASK_K1  ((unsigned char)0x01)
#define KEY_MASK_K2  ((unsigned char)0x02)
#define KEY_MASK_K3  ((unsigned char)0x04)
#define KEY_MASK_K4  ((unsigned char)0x08)

#define LED_MASK_1   ((unsigned char)0x01)
#define LED_MASK_2   ((unsigned char)0x02)
#define LED_MASK_3   ((unsigned char)0x04)
#define LED_MASK_4   ((unsigned char)0x08)

#define TIMER_SECONDS_PER_HOUR           ((unsigned int)3600)
#define TIMER_MAX_HOURS                  ((unsigned char)24)
#define TOUCH_TMR2_TICKS_PER_SCAN        ((unsigned char)32)
#define UART_SPBRG_9600_16MHZ            ((unsigned char)103)
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

static unsigned char key_stable_value;
static volatile unsigned char touch_tmr2_ticks;
static volatile unsigned char fan_pwm_tick;
static volatile unsigned char fan_pwm_duty_ticks;
static volatile unsigned char fan_pwm_enabled;

/* Timer2 sampling follows the timing required by the vendor touch library. */
static void Touch_Init(void)
{
    touch_tmr2_ticks = (unsigned char)0x00;
    TMR2IF = 0;
    PIE1 = (unsigned char)(PIE1 | 0B00000010);
    PR2 = (unsigned char)125;
    T2CON = (unsigned char)0x05;
    PEIE = 1;
    GIE = 1;
}

static void Touch_Poll(void)
{
    if (touch_tmr2_ticks >= TOUCH_TMR2_TICKS_PER_SCAN)
    {
        touch_tmr2_ticks = (unsigned char)0x00;
        __CMS_CheckTouchKey();
    }
}

/*
 * Key_Delay
 * 按键消抖延时：执行约 3000 次空循环，循环中调用 clrwdt 喂看门狗，
 * 为按键二次采样提供足够时间间隔。
 */
static void Key_Delay(void)
{
    unsigned int i;

    for (i = 0; i < 3000; i++)
    {
        asm("clrwdt");
    }
}


/*
 * Key_ReadRaw
 * 读取 4 个按键的原始电平状态并合并为位掩码返回。
 * 依次检测 TIMER/FILTER/POWER/SPEED 按键，按下则分别置
 * KEY_MASK_K1~K4 对应位，返回值中各位表示对应按键当前是否被按下。
 */
static unsigned char Key_ReadRaw(void)
{
    return (unsigned char)(_CMS_KeyFlag[0] & 0B00001111);
}

/*
 * Key_ReadStable
 * 读取经消抖处理后的稳定按键状态。连续三次调用 Key_ReadRaw 并在
 * 两次采样间插入 Key_Delay，仅当三次读数完全一致时才更新
 * key_stable_value；最终返回该稳定值，避免按键抖动导致误触发。
 */
static unsigned char Key_ReadStable(void)
{
    unsigned char first;
    unsigned char second;
    unsigned char third;

    first = Key_ReadRaw();
    Key_Delay();
    second = Key_ReadRaw();
    Key_Delay();
    third = Key_ReadRaw();

    if ((first == second) && (second == third))
    {
        key_stable_value = third;
    }

    return key_stable_value;
}

void interrupt Touch_Timer2_ISR(void)
{
    if (TMR2IF != 0)
    {
        TMR2IF = 0;

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

        touch_tmr2_ticks++;
        __CMS_GetTouchKeyValue();
    }

    if (RCIF != 0)
    {
        WIFI_ISR_Rx();
    }
}

/*
 * main
 * 程序入口与主循环。初始化板级硬件、TM1628 显示驱动与 Timer0，
 * 随后进入死循环：调用 Key_ReadStable 获取稳定按键状态，通过
 * key_now 与 key_last 按位求下降沿（按下事件）key_down。
 *
 * 按键处理状态机：
 *  - K3（电源）：切换整机开关；关机时清 LED、清计时并全灭显示；
 *    开机时点亮 LED3、复位计时并显示 0 小时。
 *  - 仅在开机状态下：
 *    K1（定时）：递增 timer_hours（满 24 回 0），同时更新 LED1 与
 *      小时显示的闪烁位；timer_hours 为 0 时熄灭 LED1。
 *    K2（滤网）：翻转 LED2。
 *    K4（风速）：翻转 LED4。
 *  每次按键处理后刷新 TM1628 的 LED 输出。
 *
 * 计时处理：每秒由 Timer0_PollSecond 触发，timer_hours 非零时
 *  累计 timer_seconds；满 3600 秒则小时数减一，减到 0 时全灭显示
 *  并复位计时基准，否则刷新小时显示。
 *
 * 循环末尾将 key_now 存入 key_last，供下一轮下降沿检测使用。
 */
void main(void)
{
    unsigned char led_state;
    unsigned char key_now;
    unsigned char key_last;
    unsigned char key_down;
    unsigned char voice_command;
    unsigned char fan_speed_level;
    unsigned char timer_hours;
    unsigned int timer_seconds;

    asm("nop");
    asm("clrwdt");

    Board_Init();
    TM1628_Init();
    Timer0_Init();
    Touch_Init();
    WIFI_Init();

    led_state = (unsigned char)0x00;
    key_stable_value = (unsigned char)0x00;
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

        if (wifi_dp_changed != (unsigned char)0x00)
        {
            wifi_dp_changed = (unsigned char)0x00;

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

            TM1628_SetLeds(led_state);
        }

        key_now = Key_ReadStable();
        key_down = (unsigned char)(key_now & (unsigned char)(~key_last));

        if (key_down != (unsigned char)0x00)
        {
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
                }

                if ((key_down & KEY_MASK_K2) != (unsigned char)0x00)
                {
                    led_state = (unsigned char)(led_state ^ LED_MASK_2);
                }

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
                }
            }

            TM1628_SetLeds(led_state);
        }

        while ((voice_command = WIFI_GetVoiceCommand()) != (unsigned char)0x00)
        {
            switch (voice_command)
            {
                        case VOICE_CMD_WAKEUP:
                            break;

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

                        case VOICE_CMD_TURN_OFF:
                            led_state = (unsigned char)0x00;
                            fan_speed_level = FAN_SPEED_LEVEL_OFF;
                            timer_hours = (unsigned char)0x00;
                            timer_seconds = (unsigned int)0;
                            Timer0_ResetTick();
                            TM1628_AllOff();
                            WIFI_ReportPower((unsigned char)0x00);
                            break;

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
                            }
                            break;

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
                            }
                            break;

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
                            }
                            break;

                        case VOICE_CMD_FILTER:
                            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                            {
                                led_state = (unsigned char)(led_state ^ LED_MASK_2);
                            }
                            break;

                        default:
                            break;
            }

            TM1628_SetLeds(led_state);
        }
        if (Timer0_PollSecond() != (unsigned char)0x00)
        {
            if (timer_hours != (unsigned char)0x00)
            {
                timer_seconds++;

                if (timer_seconds >= TIMER_SECONDS_PER_HOUR)
                {
                    timer_seconds = (unsigned int)0;
                    timer_hours--;

                    if (timer_hours == (unsigned char)0x00)
                    {
                        led_state = (unsigned char)0x00;
                        fan_speed_level = FAN_SPEED_LEVEL_OFF;
                        Timer0_ResetTick();
                        TM1628_AllOff();
                        WIFI_ReportPower((unsigned char)0x00);
                    }
                    else
                    {
                        TM1628_SetTimerDisplay(timer_hours, (unsigned char)0x01);
                    }
                }
            }
        }

        key_last = key_now;

        if (((led_state & LED_MASK_3) != (unsigned char)0x00)
            && (fan_speed_level != FAN_SPEED_LEVEL_OFF))
        {
            FAN_VCC = 1;

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

            fan_pwm_enabled = (unsigned char)0x01;
        }
        else
        {
            fan_pwm_enabled = (unsigned char)0x00;
            fan_pwm_duty_ticks = (unsigned char)0x00;
            FAN_VCC = 0;
            FAN_PWM = 0;
        }
    }
}