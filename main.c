#include <cms.h>
#include "board.h"
#include "key.h"
#include "timer.h"
#include "tm1628.h"
#include "Touch_Kscan_Library.h"
#include "tuya_protocol.h"
#include "gas_sensor.h"
#include "eeprom.h"
#include "config.h"
#include "fan.h"
#include "self_check.h"

/* 空气净化器主板主程序 (CMS79F723)。
   功能: 触摸按键控制 + 涂鸦 WiFi + 语音命令 + TM1628A 显示 + 风扇 PWM。
   主循环顺序: 触摸扫描 → WiFi 协议 → DP 下发 → 按键 → 语音 → 定时 → 风扇输出。 */

/* Timer2 周期累计, 达阈值触发触摸扫描 */
static volatile unsigned char touch_tmr2_ticks;

/* 初始化 Timer2, 按厂商触摸库要求的采样时序配置周期与中断 */
static void Touch_Init(void)
{
    touch_tmr2_ticks = (unsigned char)0x00;
    Timer2_Init();
}

/* 轮询触摸扫描: 累计 Timer2 周期达阈值时调用一次按键检测 */
static void Touch_Poll(void)
{
    if (touch_tmr2_ticks >= TOUCH_TMR2_TICKS_PER_SCAN)
    {
        touch_tmr2_ticks = (unsigned char)0x00;
        __CMS_CheckTouchKey();
    }
}

/* 读取 PM2.5 并上报云端: 预热未完成跳过, 异常值上报 0 */
static void WIFI_ReportPm25Sample(void)
{
    unsigned int pm25_value;

    if (Gas_IsWarmupDone() == (unsigned char)0)
    {
        return;
    }

    pm25_value = Gas_ReadPm25();
    if (pm25_value <= GAS_PM25_MAX)
    {
        WIFI_ReportPm25(pm25_value);
    }
    else
    {
        WIFI_ReportPm25((unsigned int)0);
    }
}

/* 中断服务程序: Timer2 中断驱动风扇 PWM 与触摸扫描, UART 接收写入 WiFi 环形缓冲区 */
void interrupt Touch_Timer2_ISR(void)
{
    if (TMR2IF != 0)
    {
        TMR2IF = 0;

        Fan_Timer2Tick();

        touch_tmr2_ticks++;       /* 触摸扫描节拍累计 */
        __CMS_GetTouchKeyValue();
    }

    if (RCIF != 0)                /* UART 接收中断: 写入 WiFi 环形缓冲区 */
    {
        WIFI_ISR_Rx();
    }
}

/* 程序入口与主循环: 初始化硬件后循环处理按键、语音指令、定时与风扇 PWM 输出 */
void main(void)
{
    unsigned char led_state;         /* 4 位 LED 状态 (与 LED_MASK_x 对应) */
    unsigned char key_now;           /* 本轮采样键值 */
    unsigned char key_last;          /* 上轮采样键值 (用于边沿检测) */
    unsigned char key_down;          /* 本次按下事件 (上升沿) */
    unsigned char voice_command;     /* 从队列取出的语音命令 */
    unsigned char fan_speed_level;   /* 当前风速档位 0~3 */
    unsigned char timer_hours;       /* 定时剩余小时 0~24 */
    unsigned int  timer_seconds;     /* 当前小时内累计秒数 0~3599 */
    unsigned char sec_tick;          /* 本循环 Timer0 是否到达 1 秒 */
    unsigned char pm25_sec_cnt;      /* PM2.5 秒级累计, 达 2 秒刷新一次 */
    unsigned char filter_usage;      /* 滤网使用度 0~100, 达到上限后停止累计 */
    unsigned int  filter_sub_hr;     /* 微小时累计 (每秒 +2/+4/+8; 达 FILTER_SUB_HR_PER_UNIT 进位) */
    unsigned char filter_view_mode;  /* 0=PM2.5 显示, 1=滤网使用度显示 */
    unsigned char led_force_mask;    /* 强制叠加的 LED 位 (当前仅滤网告警使用 LED_MASK_3) */
    unsigned char timer_k1_idle_sec; /* K1 空闲秒数, >=5 时 LED1 闪烁 */
    unsigned char timer_led1_blink;  /* LED1 闪烁标志: 0=静态, 1=闪烁中 */
    unsigned char self_check_event;

    asm("nop");
    asm("clrwdt");

    Board_Init();
    TM1628_Init();
    Timer0_Init();
    Touch_Init();
    WIFI_Init();
    Gas_Init();                      /* H3:SMELL ADC 模块初始化 */
    SelfCheck_Init();

    /* 开机默认关机状态 */
    led_state = (unsigned char)0x00;
    key_last = (unsigned char)0x00;
    fan_speed_level = FAN_SPEED_LEVEL_OFF;
    timer_hours = (unsigned char)0x00;
    timer_seconds = (unsigned int)0;
    pm25_sec_cnt = (unsigned char)0x00;
    filter_usage = EEPROM_LoadFilterUsage();   /* 从 EEPROM 恢复滤网使用度 */
    WIFI_SetPm25((unsigned int)0);
    WIFI_SetFilterUsage(filter_usage);
    WIFI_ReportAll();
    filter_sub_hr   = (unsigned int)0;
    filter_view_mode = VIEW_MODE_PM25;         /* 默认显示 PM2.5 数值栏 */
    led_force_mask  = (unsigned char)0x00;
    timer_k1_idle_sec = (unsigned char)0;
    timer_led1_blink  = (unsigned char)0;

    /* 默认关机: 传感器断电 (仅真正开机时才上电预热) */
    Gas_PowerOff();
    TM1628_AllOff();
    TM1628_SetLeds((unsigned char)(led_state | led_force_mask));

    while (1)
    {
        asm("clrwdt");

        Touch_Poll();
        if (SelfCheck_IsActive() == (unsigned char)0x00)
        {
            WIFI_Process();
        }

        /* ---- 段1: 处理云端 DP 下发 ---- */
        if (wifi_dp_changed != (unsigned char)0x00)
        {
            wifi_dp_changed = (unsigned char)0x00;

            /* 开关 DP: 同步本地开关机状态 */
            if (dp_power != (unsigned char)0x00)
            {
                if ((led_state & LED_MASK_3) == (unsigned char)0x00)
                {
                    led_state = LED_MASK_3;
                    fan_speed_level = FAN_SPEED_LEVEL_OFF;
                    timer_hours = (unsigned char)0x00;
                    timer_seconds = (unsigned int)0;
                    pm25_sec_cnt = (unsigned char)0x00;
                    filter_sub_hr = (unsigned int)0;      /* 清微小时累计, 避免跨开机跳变 */
                    Timer0_ResetTick();
                    {
                        Gas_PowerOn();                    /* 传感器上电 */
                        Gas_StartWarmup();                /* 启动 30 秒预热倒计时 */
                        TM1628_SetDefaultDisplay();
                        if (filter_view_mode == VIEW_MODE_PM25)
                        {
                            TM1628_SetPm25Display((unsigned int)GAS_WARMUP_SECONDS);
                        }
                        else
                        {
                            TM1628_SetFilterUsageDisplay(filter_usage);
                        }
                    }
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
                    pm25_sec_cnt = (unsigned char)0x00;
                    filter_sub_hr = (unsigned int)0;      /* 关机清零累计, 主值保留 */
                    Timer0_ResetTick();
                    Gas_PowerOff();
                    TM1628_AllOff();
                }
            }

            /* 仅在开机状态下同步其余 DP */
            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
            {
                /* 定时 DP: 重置计时基准并更新显示 */
                timer_hours = dp_timer;
                timer_seconds = (unsigned int)0;
                timer_k1_idle_sec = (unsigned char)0;
                timer_led1_blink  = (unsigned char)0;
                /* 不调 Timer0_ResetTick(), 否则预热倒计时会停滞约 1 秒 */

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

                /* 风速 DP: 越界归零, 更新 LED 与显示 */
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

            /* 亮度 DP: 开机/关机都可调, 共用缓存避免分支间重复写亮度。 */
            {
                static unsigned char last_brightness_shadow = 2;

                if (dp_brightness != last_brightness_shadow)
                {
                    last_brightness_shadow = dp_brightness;
                    TM1628_SetBrightness(dp_brightness);
                }
            }

            /* SetLeds 出口统一 OR led_force_mask (滤网告警当前使用 LED_MASK_3),
               不改动用户态 led_state, 保证关机判定逻辑不受影响 */
            TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
        }

        /* ---- 段2: 按键处理 ---- */
        key_now = Key_ReadStable();
        key_down = (unsigned char)(key_now & (unsigned char)(~key_last));  /* 上升沿 */
        self_check_event = SelfCheck_Process(
            key_now,
            key_down,
            (unsigned char)((led_state & LED_MASK_3) != (unsigned char)0x00));

        if (self_check_event != SELF_CHECK_EVENT_NONE)
        {
            if (self_check_event == SELF_CHECK_EVENT_FINISHED)
            {
                led_state = (unsigned char)0x00;
                fan_speed_level = FAN_SPEED_LEVEL_OFF;
                timer_hours = (unsigned char)0x00;
                timer_seconds = (unsigned int)0;
                pm25_sec_cnt = (unsigned char)0x00;
                filter_sub_hr = (unsigned int)0;
            }

            key_last = key_now;
            continue;
        }
        /* K3 长按检测: 按住约 2 秒触发 AP 配网, 数码管全显示 "1" 提示 */
        {
            static unsigned int  k3_hold_ticks = 0;
            static unsigned char k3_ap_triggered = 0;

            if ((key_now & KEY_MASK_K3) != (unsigned char)0x00)
            {
                k3_hold_ticks++;
                if (k3_hold_ticks >= K3_LONG_PRESS_TICKS
                    && k3_ap_triggered == (unsigned char)0x00)
                {
                    k3_ap_triggered = (unsigned char)0x01;
                    WIFI_ResetAP();
                    TM1628_SetApDisplay();
                }
            }
            else
            {
                k3_hold_ticks = 0;
                k3_ap_triggered = (unsigned char)0x00;
            }
        }

        /* K2 状态机 (长按翻转 LED2 / 短按切换显示模式):
           - 按下累计 k2_hold_ticks: 达 K2_LONG_PRESS_TICKS 且 system_on 时触发长按
             (翻转 LED2 并上报 DP104), 置 k2_long_fired=1 阻止释放时再触发短按
           - 释放瞬间: 若 k2_long_fired==0 且 ticks>=最小短按阈值, 判定为短按
           - 关机时 K2 的长按/短按功能不执行; K3 由独立逻辑处理。 */
        {
            static unsigned int  k2_hold_ticks = 0;
            static unsigned char k2_long_fired = 0;
            unsigned char k2_phy_pressed;
            unsigned char system_on;

            k2_phy_pressed = (unsigned char)(((key_now  & KEY_MASK_K2) != (unsigned char)0x00));
            system_on      = (unsigned char)(((led_state & LED_MASK_3) != (unsigned char)0x00));

            if (k2_phy_pressed != (unsigned char)0x00)
            {
                /* 按下态: 累计 tick, 长按需开机才生效 */
                k2_hold_ticks++;

                if (system_on
                    && (k2_hold_ticks >= K2_LONG_PRESS_TICKS)
                    && (k2_long_fired == (unsigned char)0x00))
                {
                    k2_long_fired = (unsigned char)0x01;
                    if ((led_state & LED_MASK_2) != (unsigned char)0x00)
                    {
                        /* LED2 亮: 清空滤网使用度, 同时翻转熄灭 LED2 (功能1) */
                        filter_usage = (unsigned char)0;
                        filter_sub_hr = (unsigned int)0;
                        EEPROM_SaveFilterUsage((unsigned char)0);
                        WIFI_ReportFilterUsage((unsigned char)0);
                        led_state = (unsigned char)(led_state ^ LED_MASK_2);
                        WIFI_ReportIndicator((unsigned char)0x00);
                    }
                    else
                    {
                        /* LED2 灭: 翻转点亮 (原长按行为) */
                        led_state = (unsigned char)(led_state ^ LED_MASK_2);
                        WIFI_ReportIndicator(
                            ((led_state & LED_MASK_2) != (unsigned char)0x00)
                                ? (unsigned char)0x01
                                : (unsigned char)0x00);
                    }
                    /* SetLeds 出口统一 OR led_force_mask (滤网告警当前使用 LED_MASK_3) */
            TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
                }
            }
            else
            {
                /* 非按下态 */

                /* 上一轮是按下态 → 检测到释放边沿 */
                if ((key_last & KEY_MASK_K2) != (unsigned char)0x00)
                {
                    if (k2_long_fired == (unsigned char)0x01)
                    {
                        /* 长按已触发: 释放时跳过短按分支, 避免一次按键触发两种操作。 */
                    }
                    else if ((k2_hold_ticks >= (unsigned int)2)
                             && (system_on != (unsigned char)0x00))
                    {
                        /* 短按事件: 仅在开机状态切显示模式 */
                        if (filter_view_mode == VIEW_MODE_PM25)
                        {
                            filter_view_mode = VIEW_MODE_FILTER;
                            TM1628_SetFilterUsageDisplay(filter_usage);
                        }
                        else
                        {
                            filter_view_mode = VIEW_MODE_PM25;
                            if (Gas_IsWarmupDone())
                            {
                                unsigned int _pm;

                                _pm = Gas_ReadPm25();
                                if (_pm <= GAS_PM25_MAX)
                                {
                                    TM1628_SetPm25Display(_pm);
                                }
                                else
                                {
                                    TM1628_SetPm25Display((unsigned int)GAS_PM25_INVALID);
                                }
                            }
                            else
                            {
                                TM1628_SetPm25Display((unsigned int)Gas_GetWarmupRemaining());
                            }
                        }
                        /* 短按仅切换显示视图, 不刷新 LED 状态 — 避免
                           因 4a2 定时闪烁修改了 led_state 但尚未写入硬件,
                           导致此处 SetLeds 意外将闪烁中的 led_state 刷到硬件,
                           表现为 "K2 点按翻转 LED1"。 */
                    }
                    /* 关机状态下的短按: 直接忽略 (不切视图也不改 LED) */
                }

                k2_hold_ticks = 0;
                k2_long_fired = (unsigned char)0x00;
            }
        }

        /* ---- K1 定时键状态机 (长按清空定时 / 短按递增) ---- */
        {
            static unsigned int  k1_hold_ticks = 0;
            static unsigned char k1_long_fired = 0;
            unsigned char system_on;

            system_on = (unsigned char)(((led_state & LED_MASK_3) != (unsigned char)0x00));

            if ((key_now & KEY_MASK_K1) != (unsigned char)0x00 && system_on)
            {
                /* 按下态: 累计 tick, 长按清空定时 */
                k1_hold_ticks++;

                if ((k1_hold_ticks >= K2_LONG_PRESS_TICKS)  /* 复用 ~2s 阈值 */
                    && (k1_long_fired == (unsigned char)0x00))
                {
                    k1_long_fired = (unsigned char)0x01;
                    /* 长按: 清空定时时间, 恢复默认状态 */
                    timer_hours     = (unsigned char)0x00;
                    timer_seconds   = (unsigned int)0;
                    timer_k1_idle_sec = (unsigned char)0;
                    timer_led1_blink  = (unsigned char)0;
                    led_state = (unsigned char)(led_state & (unsigned char)(~LED_MASK_1));
                    TM1628_SetTimerDisplay((unsigned char)0x00, (unsigned char)0x00);
                    TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
                    WIFI_ReportTimer((unsigned char)0x00);
                }
            }
            else
            {
                /* 非按下态 */

                /* 上一轮是按下态 → 检测到释放边沿 */
                if ((key_last & KEY_MASK_K1) != (unsigned char)0x00 && system_on)
                {
                    if (k1_long_fired == (unsigned char)0x00
                        && k1_hold_ticks >= (unsigned int)2
                        && k1_hold_ticks < (K2_LONG_PRESS_TICKS - (unsigned int)50))
                    {
                        /* 短按: 小时数 0→24 循环递增。
                           增加 k1_hold_ticks < (阈值-50) 条件, 防止长按过程中
                           触摸键偶发抖动(瞬间跳变为松开)误触发短按递增。
                           达 ~450 tick 后即使抖动也只被判定为 "准备长按中", 不触发短按,
                           最终达到 ~500 tick 触发长按清空。 */
                        if (timer_hours < TIMER_MAX_HOURS)
                            timer_hours++;
                        else
                            timer_hours = (unsigned char)0x00;

                        timer_seconds = (unsigned int)0;
                        timer_k1_idle_sec = (unsigned char)0;
                        timer_led1_blink  = (unsigned char)0;

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

                        TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
                        WIFI_ReportTimer(timer_hours);
                    }
                }

                k1_hold_ticks = 0;
                k1_long_fired = (unsigned char)0x00;
            }
        }

        /* ---- K4 风速键状态机 (长按恢复默认 OFF / 短按循环递增) ---- */
        {
            static unsigned int  k4_hold_ticks = 0;
            static unsigned char k4_long_fired = 0;
            unsigned char system_on;

            system_on = (unsigned char)(((led_state & LED_MASK_3) != (unsigned char)0x00));

            if ((key_now & KEY_MASK_K4) != (unsigned char)0x00 && system_on)
            {
                /* 按下态: 累计 tick, 长按恢复默认 */
                k4_hold_ticks++;

                if ((k4_hold_ticks >= K2_LONG_PRESS_TICKS)
                    && (k4_long_fired == (unsigned char)0x00))
                {
                    k4_long_fired = (unsigned char)0x01;
                    /* 长按: 恢复风扇档位默认状态 (OFF) */
                    fan_speed_level = FAN_SPEED_LEVEL_OFF;
                    led_state = (unsigned char)(led_state & (unsigned char)(~LED_MASK_4));
                    TM1628_SetSpeedDisplay(fan_speed_level);
                    TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
                    WIFI_ReportFanSpeed(fan_speed_level);
                }
            }
            else
            {
                /* 非按下态 */

                /* 上一轮是按下态 → 检测到释放边沿 */
                if ((key_last & KEY_MASK_K4) != (unsigned char)0x00 && system_on)
                {
                    if (k4_long_fired == (unsigned char)0x00
                        && k4_hold_ticks >= (unsigned int)2)
                    {
                        /* 短按: 档位 0→3 循环递增, 0 时关风速 LED */
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
                        TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
                        WIFI_ReportFanSpeed(fan_speed_level);
                    }
                }

                k4_hold_ticks = 0;
                k4_long_fired = (unsigned char)0x00;
            }
        }

        if (key_down != (unsigned char)0x00)
        {
            /* K3 电源键 (短按): 开关机切换并上报云端 */
            if ((key_down & KEY_MASK_K3) != (unsigned char)0x00)
            {
                if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                {
                    led_state = (unsigned char)0x00;
                    fan_speed_level = FAN_SPEED_LEVEL_OFF;
                    timer_hours = (unsigned char)0x00;
                    timer_seconds = (unsigned int)0;
                    pm25_sec_cnt = (unsigned char)0x00;
                    filter_sub_hr = (unsigned int)0;            /* 关机清零累计, 主值保留 */
                    Timer0_ResetTick();
                    Gas_PowerOff();
                    TM1628_AllOff();
                    WIFI_ReportPower((unsigned char)0x00);
                }
                else
                {
                    led_state = LED_MASK_3;
                    fan_speed_level = FAN_SPEED_LEVEL_OFF;
                    timer_hours = (unsigned char)0x00;
                    timer_seconds = (unsigned int)0;
                    pm25_sec_cnt = (unsigned char)0x00;
                    filter_sub_hr = (unsigned int)0;
                    Timer0_ResetTick();
                    Gas_PowerOn();
                    Gas_StartWarmup();
                    TM1628_SetDefaultDisplay();
                    if (filter_view_mode == VIEW_MODE_PM25)
                    {
                        TM1628_SetPm25Display((unsigned int)GAS_WARMUP_SECONDS);
                    }
                    else
                    {
                        TM1628_SetFilterUsageDisplay(filter_usage);
                    }
                    WIFI_ReportPower((unsigned char)0x01);
                }
            }

            TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
        }

        /* ---- 段3: 语音命令处理 (从队列逐条消费) ---- */
        while ((voice_command = WIFI_GetVoiceCommand()) != (unsigned char)0x00)
        {
            switch (voice_command)
            {
                        case VOICE_CMD_WAKEUP:
                            break;

                        /* 开机: 仅在关机时执行, 避免重复上报 */
                        case VOICE_CMD_TURN_ON:
                            if ((led_state & LED_MASK_3) == (unsigned char)0x00)
                            {
                                led_state = LED_MASK_3;
                                fan_speed_level = FAN_SPEED_LEVEL_OFF;
                                timer_hours = (unsigned char)0x00;
                                timer_seconds = (unsigned int)0;
                                pm25_sec_cnt = (unsigned char)0x00;
                                filter_sub_hr = (unsigned int)0;
                                Timer0_ResetTick();
                                Gas_PowerOn();
                                Gas_StartWarmup();
                                TM1628_SetDefaultDisplay();
                                if (filter_view_mode == VIEW_MODE_PM25)
                                {
                                    TM1628_SetPm25Display((unsigned int)GAS_WARMUP_SECONDS);
                                }
                                else
                                {
                                    TM1628_SetFilterUsageDisplay(filter_usage);
                                }
                                WIFI_ReportPower((unsigned char)0x01);
                            }
                            break;

                        /* 关机: 无条件关闭所有输出 */
                        case VOICE_CMD_TURN_OFF:
                            led_state = (unsigned char)0x00;
                            fan_speed_level = FAN_SPEED_LEVEL_OFF;
                            timer_hours = (unsigned char)0x00;
                            timer_seconds = (unsigned int)0;
                            pm25_sec_cnt = (unsigned char)0x00;
                            filter_sub_hr = (unsigned int)0;
                            Timer0_ResetTick();
                            Gas_PowerOff();
                            TM1628_AllOff();
                            WIFI_ReportPower((unsigned char)0x00);
                            break;

                        /* 加档: 仅在开机时生效, 已达最高档不动作 */
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

                        /* 减档: 仅在开机时生效, 已为关不动作 */
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
                                timer_k1_idle_sec = (unsigned char)0;
                                timer_led1_blink  = (unsigned char)0;
                                /* 不调 Timer0_ResetTick(), 否则预热倒计时会停滞约 1 秒 */

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

                        /* 定时减一小时 (仅在开机且当前有定时任务时生效) */
                        case VOICE_CMD_TIMER_DOWN:
                            if (((led_state & LED_MASK_3) != (unsigned char)0x00)
                                && (timer_hours != (unsigned char)0x00))
                            {
                                timer_hours--;
                                timer_seconds = (unsigned int)0;
                                timer_k1_idle_sec = (unsigned char)0;
                                timer_led1_blink  = (unsigned char)0;

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

                        /* 固定定时 1~24 小时 (仅在开机时生效) */
                        case VOICE_CMD_TIME_1:
                        case VOICE_CMD_TIME_2:
                        case VOICE_CMD_TIME_3:
                        case VOICE_CMD_TIME_4:
                        case VOICE_CMD_TIME_5:
                        case VOICE_CMD_TIME_6:
                        case VOICE_CMD_TIME_7:
                        case VOICE_CMD_TIME_8:
                        case VOICE_CMD_TIME_9:
                        case VOICE_CMD_TIME_10:
                        case VOICE_CMD_TIME_11:
                        case VOICE_CMD_TIME_12:
                        case VOICE_CMD_TIME_13:
                        case VOICE_CMD_TIME_14:
                        case VOICE_CMD_TIME_15:
                        case VOICE_CMD_TIME_16:
                        case VOICE_CMD_TIME_17:
                        case VOICE_CMD_TIME_18:
                        case VOICE_CMD_TIME_19:
                        case VOICE_CMD_TIME_20:
                        case VOICE_CMD_TIME_21:
                        case VOICE_CMD_TIME_22:
                        case VOICE_CMD_TIME_23:
                        case VOICE_CMD_TIME_24:
                            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                            {
                                if (voice_command <= VOICE_CMD_TIME_2)
                                {
                                    timer_hours = (unsigned char)(voice_command - (unsigned char)0xA7);
                                }
                                else if (voice_command <= VOICE_CMD_TIME_12)
                                {
                                    timer_hours = (unsigned char)(voice_command - (unsigned char)0xB0 + (unsigned char)3);
                                }
                                else if (voice_command <= VOICE_CMD_TIME_22)
                                {
                                    timer_hours = (unsigned char)(voice_command - (unsigned char)0xC0 + (unsigned char)13);
                                }
                                else
                                {
                                    timer_hours = (unsigned char)(voice_command - (unsigned char)0xD0 + (unsigned char)23);
                                }

                                timer_seconds = (unsigned int)0;
                                timer_k1_idle_sec = (unsigned char)0;
                                timer_led1_blink  = (unsigned char)0;
                                led_state = (unsigned char)(led_state | LED_MASK_1);
                                TM1628_SetTimerDisplay(timer_hours, (unsigned char)0x01);
                                WIFI_ReportTimer(timer_hours);
                            }
                            break;

                        /* 取消定时但保持设备当前开关状态 */
                        case VOICE_CMD_TIMER_OFF:
                            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                            {
                                timer_hours = (unsigned char)0x00;
                                timer_seconds = (unsigned int)0;
                                timer_k1_idle_sec = (unsigned char)0;
                                timer_led1_blink  = (unsigned char)0;
                                led_state = (unsigned char)(led_state & (unsigned char)(~LED_MASK_1));
                                TM1628_SetTimerDisplay(timer_hours, (unsigned char)0x00);
                                WIFI_ReportTimer(timer_hours);
                            }
                            break;

                        /* 风扇固定档位命令 (仅在开机时生效) */
                        case VOICE_CMD_SPEED_OFF:
                        case VOICE_CMD_SPEED_1:
                        case VOICE_CMD_SPEED_2:
                        case VOICE_CMD_SPEED_3:
                            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                            {
                                if (voice_command == VOICE_CMD_SPEED_OFF)
                                {
                                    fan_speed_level = FAN_SPEED_LEVEL_OFF;
                                    led_state = (unsigned char)(led_state & (unsigned char)(~LED_MASK_4));
                                }
                                else
                                {
                                    fan_speed_level = (unsigned char)(voice_command - VOICE_CMD_SPEED_1 + (unsigned char)1);
                                    led_state = (unsigned char)(led_state | LED_MASK_4);
                                }

                                TM1628_SetSpeedDisplay(fan_speed_level);
                                WIFI_ReportFanSpeed(fan_speed_level);
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

            TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
        }

        /* ---- 段4: 定时倒计时 + PM2.5 每 2s 刷新 (共用 Timer0 秒事件) ---- */
        sec_tick = Timer0_PollSecond();

        if (sec_tick != (unsigned char)0x00)
        {
            /* 4a: 定时倒计时 */
            if (timer_hours != (unsigned char)0x00)
            {
                timer_seconds++;

                if (timer_seconds >= TIMER_SECONDS_PER_HOUR)   /* 累满一小时: 小时数递减 */
                {
                    timer_seconds = (unsigned int)0;
                    timer_hours--;

                    if (timer_hours == (unsigned char)0x00)    /* 归零: 关机并上报 */
                    {
                        led_state = (unsigned char)0x00;
                        fan_speed_level = FAN_SPEED_LEVEL_OFF;
                        Fan_Stop();
                        pm25_sec_cnt = (unsigned char)0x00;
                        filter_sub_hr = (unsigned int)0;
                        timer_k1_idle_sec = (unsigned char)0;
                        timer_led1_blink  = (unsigned char)0;
                        Timer0_ResetTick();
                        Gas_PowerOff();
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

            /* 4a2: K1 空闲秒数累计 + LED1 闪烁控制 (仅开机状态) */
            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
            {
                if (timer_hours > (unsigned char)0)
                {
                    if (timer_k1_idle_sec < (unsigned char)5)   /* 定时运行中: 累计空闲秒数 */
                        timer_k1_idle_sec++;

                    if (timer_k1_idle_sec >= (unsigned char)5)
                    {
                        timer_led1_blink = (unsigned char)1;    /* 超 5s 无 K1 操作: LED1 闪烁 */
                        led_state = (unsigned char)(led_state ^ LED_MASK_1);
                        TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
                    }
                    else
                    {
                        timer_led1_blink = (unsigned char)0;
                        led_state = (unsigned char)(led_state | LED_MASK_1);  /* 未闪烁: LED1 常亮 */
                        TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
                    }
                }
                else
                {
                    timer_k1_idle_sec = (unsigned char)0;       /* 定时关闭: 清闪烁状态 */
                    timer_led1_blink  = (unsigned char)0;
                }
            }

            /* 4b: PM2.5 栏每 2 秒刷新一次, 显示模式 filter_view_mode 决定内容:
                 VIEW_MODE_PM25   → 刷新异味传感器实时 PM2.5 (需 ADC 采样)
                 VIEW_MODE_FILTER → 刷新滤网使用度 (本地变量 filter_usage)
               每秒都会进入 4c (滤网使用度累算); 但只每 2 秒刷新一次 PM2.5 栏。 */
            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
            {
                pm25_sec_cnt++;
                if (pm25_sec_cnt >= PM25_REFRESH_INTERVAL_SEC)
                {
                    pm25_sec_cnt = (unsigned char)0x00;

                    if (filter_view_mode == VIEW_MODE_PM25)
                    {
                        if (Gas_IsWarmupDone())  /* 预热未完成时不读取, 显示由 4e 每秒更新 */
                        {
                            unsigned int _pm;

                            _pm = Gas_ReadPm25();
                            if (_pm <= GAS_PM25_MAX)
                            {
                                TM1628_SetPm25Display(_pm);
                                WIFI_ReportPm25(_pm);
                            }
                            else
                            {
                                /* 传感器异常时主动写无效值消隐并上报 0, 避免保留上一帧旧数值。 */
                                TM1628_SetPm25Display((unsigned int)GAS_PM25_INVALID);
                                WIFI_ReportPm25((unsigned int)0);
                            }
                        }
                    }
                    else
                    {
                        /* 滤网模式: 以 2s 节奏重绘, filter_usage 可能通过 4c 进位后改变 */
                        TM1628_SetFilterUsageDisplay(filter_usage);
                        WIFI_ReportPm25Sample();
                    }
                }
            }
            else
            {
                pm25_sec_cnt = (unsigned char)0x00;   /* 关机: 不累计, 避免开机时立刻触发刷新 */
            }

            /* 4c: 滤网使用度累算 (仅开机状态)。
               依据当前 PM2.5 值分档, 每秒向 filter_sub_hr 加对应档 "微单位":
                 PM2.5 < 100        → 轻度, 每小时 +2 → 每秒 sub += 2
                 100 ≤ PM2.5 < 200  → 中度, 每小时 +4 → 每秒 sub += 4
                 PM2.5 ≥ 200        → 重度, 每小时 +8 → 每秒 sub += 8
               filter_usage 饱和上限 100, 超过不继续累计 (归零需用户设置或换滤网后重启)。 */
            if ((led_state & LED_MASK_3) != (unsigned char)0x00
                && filter_usage < FILTER_USAGE_MAX)
            {
                unsigned int  _pm_now;
                unsigned char add_sub;

                if (Gas_IsWarmupDone())  /* 预热未完成时不读取, 不累计 */
                {
                    _pm_now = Gas_ReadPm25();
                }
                else
                {
                    _pm_now = GAS_PM25_INVALID;
                }

                /* 无效 PM 值不参与滤网使用度累计, 避免异常值被归入重污染档。 */
                if (_pm_now > GAS_PM25_MAX)
                {
                    add_sub = (unsigned char)0;
                }
                else if (_pm_now <= PM25_LEVEL_LIGHT_MAX)
                {
                    add_sub = FILTER_ADD_LIGHT;
                }
                else if (_pm_now <= PM25_LEVEL_MEDIUM_MAX)
                {
                    add_sub = FILTER_ADD_MEDIUM;
                }
                else
                {
                    add_sub = FILTER_ADD_HEAVY;
                }

                if (add_sub != (unsigned char)0)
                {
                    /* 每秒加入对应 "微单位", 最大不超过 FILTER_SUB_HR_PER_UNIT*8。
                       极端情况 filter_sub_hr 可达 (3600*2-1 + 8), unsigned int 足够装。 */
                    filter_sub_hr = filter_sub_hr + (unsigned int)add_sub;

                    /* 满 3600 微单位 → 进位 1 使用度。
                       add_sub 是 2/4/8, 一次可能跨过多个 3600, 用 while 一次性处理所有进位。 */
                    while (filter_sub_hr >= FILTER_SUB_HR_PER_UNIT)
                        {
                            filter_sub_hr = filter_sub_hr - FILTER_SUB_HR_PER_UNIT;
                            if (filter_usage < FILTER_USAGE_MAX)
                            {
                                filter_usage++;
                                EEPROM_SaveFilterUsage(filter_usage);
                                WIFI_ReportFilterUsage(filter_usage);
                            }
                        else
                        {
                            filter_sub_hr = (unsigned int)0;  /* 已到上限, 丢弃剩余防溢出 */
                            break;
                        }
                    }
                }
            }
            else if ((led_state & LED_MASK_3) == (unsigned char)0x00)
            {
                /* 关机: 不累计也不清零主值; 仅清微小时进位缓存 (避免下次开机秒级立刻进位) */
                filter_sub_hr = (unsigned int)0;
            }

            /* 4d: 滤网使用度告警 (独立于开关机)。
               当前使用 LED_MASK_3 作为告警提示, 用独立 mask 不污染 led_state。
               所有 SetLeds 出口统一 OR mask 后输出, 保证 <=90 时恢复用户原状态。 */
            if (filter_usage > FILTER_USAGE_WARN_THRESH)
            {
                led_force_mask = (unsigned char)(led_force_mask | LED_MASK_3);
            }
            else
            {
                led_force_mask = (unsigned char)(led_force_mask & (unsigned char)(~LED_MASK_3));
            }

            /* 4e: H3:SMELL 外设预热倒计时 (每秒更新, 仅开机状态)。
               预热期间在 PM2.5 三位数码管显示剩余秒数 (30→1),
               预热完成后自动读取首次有效 PM2.5 数值并显示。
               加 (led_state & LED_MASK_3) 条件, 防止关机时因 warmup_counter
               未复位仍继续写 TM1628 显示。 */
            if ((led_state & LED_MASK_3) != (unsigned char)0x00
                && !Gas_IsWarmupDone())
            {
                Gas_TickWarmupSecond();
                if (filter_view_mode == VIEW_MODE_PM25)
                {
                    if (Gas_IsWarmupDone())
                    {
                        unsigned int _pm25 = Gas_ReadPm25();   /* 预热完成: 读取首次有效 PM2.5 */
                        if (_pm25 <= GAS_PM25_MAX)
                        {
                            TM1628_SetPm25Display(_pm25);
                        }
                        else
                        {
                            TM1628_SetPm25Display((unsigned int)GAS_PM25_INVALID);
                        }
                    }
                    else
                    {
                        TM1628_SetPm25Display((unsigned int)Gas_GetWarmupRemaining());  /* 仍在预热: 显示剩余秒数 */
                    }
                }
            }
        }

        key_last = key_now;

        /* ---- 段5: 风扇 PWM 输出 ---- */
        Fan_UpdateOutput(
            (unsigned char)((led_state & LED_MASK_3) != (unsigned char)0x00),
            fan_speed_level);
    }
}
