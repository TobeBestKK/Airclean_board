#include <cms.h>
#include "board.h"
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

#define TIMER_TMR0_OVERFLOWS_PER_SECOND  ((unsigned char)61)
#define TIMER_SECONDS_PER_HOUR           ((unsigned int)3600)
#define TIMER_MAX_HOURS                  ((unsigned char)24)
#define TOUCH_TMR2_TICKS_PER_SCAN        ((unsigned char)32)

static unsigned char key_stable_value;
static volatile unsigned char touch_tmr2_ticks;
static unsigned char timer_tmr0_overflows;

/*
 * Timer0_ResetTick
 * 复位 Timer0 计时基准：清零 TMR0 寄存器、清除溢出标志 T0IF、
 * 并将软件溢出计数器 timer_tmr0_overflows 清零，为下一轮秒计时做准备。
 */
static void Timer0_ResetTick(void)
{
    TMR0 = (unsigned char)0x00;
    T0IF = 0;
    timer_tmr0_overflows = (unsigned char)0x00;
}

/*
 * Timer0_Init
 * 初始化 Timer0 模块：使用内部指令时钟（T0CS=0）、上升沿计数（T0SE=0）、
 * 将预分频器分配给 Timer0（PSA=0）并设置为 1:256（PS2:PS0=111），
 * 关闭 Timer0 中断（T0IE=0），随后调用 Timer0_ResetTick 完成初始复位。
 */
static void Timer0_Init(void)
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
 * Timer0_PollSecond
 * 轮询 Timer0 溢出标志以累计秒数。每次检测到 T0IF 置位则清零并递增
 * 软件溢出计数器；当溢出次数累计到 TIMER_TMR0_OVERFLOWS_PER_SECOND
 *（约 61 次）时认为已过去 1 秒，复位计数器并返回 1，否则返回 0。
 */
static unsigned char Timer0_PollSecond(void)
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

/*
 * 中断服务程序
 *
 * 处理 Timer2（触摸扫描时序）和 EUSART 接收（WiFi + 语音）中断。
 * 注意: 使用独立的 if 而非 else-if, 确保多个中断源同时触发时均得到处理。
 */
void interrupt Touch_Timer2_ISR(void)
{
    /* Timer2: 触摸按键扫描 */
    if (TMR2IF != 0)
    {
        TMR2IF = 0;
        touch_tmr2_ticks++;
        __CMS_GetTouchKeyValue();
    }

    /* EUSART 接收: WiFi 协议帧 + 语音命令 */
    if (RCIF != 0)
    {
        WIFI_ISR_Rx();
    }
}

/*
 * main
 * 程序入口与主循环。初始化板级硬件、TM1628 显示驱动、Timer0、
 * WiFi 模块 (UART)，随后进入死循环。
 *
 * 主循环每轮执行:
 *   1. Touch_Poll()      — 触摸按键扫描
 *   2. WIFI_Process()    — 涂鸦 WiFi 协议解析与应答
 *   3. WiFi DP 处理     — 将云端下发的 DP 状态应用到本地
 *   4. 按键消抖与响应   — 触摸按键事件 (K1~K4)
 *   5. 语音命令处理     — 来自 UART 缓冲区的语音指令 (0xA0~0xA6)
 *   6. 定时器递减       — 每小时减一, 归零时关机
 *
 * 状态变更时 (按键/语音/WiFi) 会调用 WIFI_Report*() 上报云端。
 */
void main(void)
{
    unsigned char led_state;
    unsigned char key_now;
    unsigned char key_last;
    unsigned char key_down;
    unsigned char voice_command;
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
    timer_hours = (unsigned char)0x00;
    timer_seconds = (unsigned int)0;

    TM1628_AllOff();
    TM1628_SetLeds(led_state);

    while (1)
    {
        asm("clrwdt");

        /* ---- 触摸按键扫描 ---- */
        Touch_Poll();

        /* ---- 涂鸦 WiFi 协议处理 ---- */
        WIFI_Process();

        /* ---- WiFi 云端 DP 下发 → 本地状态同步 (仅 DP 101 开关) ---- */
        if (wifi_dp_changed != (unsigned char)0x00)
        {
            wifi_dp_changed = (unsigned char)0x00;

            /* 调试: 收到云端命令时翻转 LED1 (定时灯), 确认通信链路正常 */
            led_state = (unsigned char)(led_state ^ LED_MASK_1);

            if (dp_power != (unsigned char)0x00)
            {
                /* 云端开机: 若本地已关则开机 */
                if ((led_state & LED_MASK_3) == (unsigned char)0x00)
                {
                    led_state = LED_MASK_3;
                    timer_hours = (unsigned char)0x00;
                    timer_seconds = (unsigned int)0;
                    Timer0_ResetTick();
                    TM1628_SetDefaultDisplay();
                }
            }
            else
            {
                /* 云端关机: 若本地已开则关机 */
                if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                {
                    led_state = (unsigned char)0x00;
                    timer_hours = (unsigned char)0x00;
                    timer_seconds = (unsigned int)0;
                    Timer0_ResetTick();
                    TM1628_AllOff();
                }
            }

            TM1628_SetLeds(led_state);
        }

        /* ---- 触摸按键消抖与下降沿检测 ---- */
        key_now = Key_ReadStable();
        key_down = (unsigned char)(key_now & (unsigned char)(~key_last));

        if (key_down != (unsigned char)0x00)
        {
            /* K3: 电源开关 */
            if ((key_down & KEY_MASK_K3) != (unsigned char)0x00)
            {
                if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                {
                    /* 关机 */
                    led_state = (unsigned char)0x00;
                    timer_hours = (unsigned char)0x00;
                    timer_seconds = (unsigned int)0;
                    Timer0_ResetTick();
                    TM1628_AllOff();
                    WIFI_ReportPower((unsigned char)0x00);
                }
                else
                {
                    /* 开机 */
                    led_state = LED_MASK_3;
                    timer_hours = (unsigned char)0x00;
                    timer_seconds = (unsigned int)0;
                    Timer0_ResetTick();
                    TM1628_SetDefaultDisplay();
                    WIFI_ReportPower((unsigned char)0x01);
                }
            }
            else if ((led_state & LED_MASK_3) != (unsigned char)0x00)
            {
                /* K1: 定时 */
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

                /* K2: 滤网 / 负离子 */
                if ((key_down & KEY_MASK_K2) != (unsigned char)0x00)
                {
                    led_state = (unsigned char)(led_state ^ LED_MASK_2);
                }

                /* K4: 风速 */
                if ((key_down & KEY_MASK_K4) != (unsigned char)0x00)
                {
                    led_state = (unsigned char)(led_state ^ LED_MASK_4);
                }
            }

            TM1628_SetLeds(led_state);
        }

        /* ---- 语音命令处理 (从 WiFi 协议层语音队列读取) ---- */
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
                    timer_hours = (unsigned char)0x00;
                    timer_seconds = (unsigned int)0;
                    Timer0_ResetTick();
                    TM1628_SetDefaultDisplay();
                    WIFI_ReportPower((unsigned char)0x01);
                }
                break;

            case VOICE_CMD_TURN_OFF:
                led_state = (unsigned char)0x00;
                timer_hours = (unsigned char)0x00;
                timer_seconds = (unsigned int)0;
                Timer0_ResetTick();
                TM1628_AllOff();
                WIFI_ReportPower((unsigned char)0x00);
                break;

            case VOICE_CMD_SPEED_UP:
                if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                {
                    led_state = (unsigned char)(led_state | LED_MASK_4);
                }
                break;

            case VOICE_CMD_SPEED_DOWN:
                if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                {
                    led_state = (unsigned char)(led_state & (unsigned char)(~LED_MASK_4));
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

        /* ---- 定时器递减 (每秒一次) ---- */
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
    }
}
