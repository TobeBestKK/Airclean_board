#include <cms.h>
#include "board.h"
#include "key.h"
#include "timer.h"
#include "tm1628.h"
#include "Touch_Kscan_Library.h"
#include "tuya_protocol.h"
#include "gas_sensor.h"
#include "eeprom.h"

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

/* K3 长按配网阈值：约 2 秒 (主循环 ~4ms × 500) */
#define K3_LONG_PRESS_TICKS  ((unsigned int)500)

/* K2 长按滤网指示灯阈值：约 2 秒 (与 K3 相同，统一操作手感) */
#define K2_LONG_PRESS_TICKS  ((unsigned int)500)

/* PM2.5 异味显示刷新间隔 (单位: 秒，使用 Timer0 秒节拍累计 */
#define PM25_REFRESH_INTERVAL_SEC  ((unsigned char)2)

/* 滤网使用度 (0~100)：上限与微小时进位阈值
 * 微小时 (filter_sub_hr) = 每秒 +2/+4/+8 点"微单位"，满 3600 则进位 2/4/8 使用度
 *   轻污染 PM2.5 <100:  2 使用度/小时
 *   中污染 100~199:    4 使用度/小时
 *   重污染 >=200:       8 使用度/小时
 */
#define FILTER_USAGE_MAX        ((unsigned char)100)
#define FILTER_SUB_HR_PER_UNIT  ((unsigned int)3600)
#define FILTER_ADD_LIGHT        ((unsigned char)2)
#define FILTER_ADD_MEDIUM       ((unsigned char)4)
#define FILTER_ADD_HEAVY        ((unsigned char)8)
#define PM25_LEVEL_LIGHT_MAX    ((unsigned int)150)   /* 0~150 轻度污染 */
#define PM25_LEVEL_MEDIUM_MAX   ((unsigned int)300)   /* 151~300 中度污染 */

/* 滤网使用度告警阈值: 当 filter_usage > 此值时, 自动强制点亮 LED3(电源位置)
   作为"滤网需要更换"的提醒灯。使用独立掩码 OR 叠加, 不修改用户 led_state。 */
#define FILTER_USAGE_WARN_THRESH  ((unsigned char)90)

/* 显示模式 (DIG7/DIG6/DIG5 三位 PM2.5 栏切换):
 *   0 = 显示 PM2.5 数值 (默认)
 *   1 = 显示 滤网使用度 (0~100)
 */
#define VIEW_MODE_PM25          ((unsigned char)0x00)
#define VIEW_MODE_FILTER        ((unsigned char)0x01)

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
    unsigned int  timer_seconds;    /* 当前小时内累计秒数 0~3599 */
    unsigned char sec_tick;         /* 本循环 Timer0 是否到达 1 秒 (两个子模块共用) */
    unsigned char pm25_sec_cnt;     /* PM2.5 秒级累计，达 2 秒刷新一次 */
    unsigned char filter_usage;     /* 滤网使用度 0~100，超过饱和 */
    unsigned int  filter_sub_hr;    /* 微小时累计 (每秒 +2/+4/+8; 满 3600 则进位 X 使用度) */
    unsigned char filter_view_mode; /* VIEW_MODE_PM25(0)=PM2.5显示; VIEW_MODE_FILTER(1)=滤网使用度显示 */
    unsigned char led_force_mask;   /* 强制叠加的 LED 位 (当前仅滤网告警强制亮 LED3) */
    unsigned char timer_k1_idle_sec;/* K1 空闲秒数, >=5 时 LED1 闪烁 */
    unsigned char timer_led1_blink; /* LED1 闪烁标志: 0=静态, 1=闪烁中 */

    asm("nop");
    asm("clrwdt");

    Board_Init();
    TM1628_Init();
    Timer0_Init();
    Touch_Init();
    WIFI_Init();
    Gas_Init();                     /* TP-401W 异味传感器: ADC 模块初始化 */

    /* 开机默认关机状态 */
    led_state = (unsigned char)0x00;
    key_last = (unsigned char)0x00;
    fan_speed_level = FAN_SPEED_LEVEL_OFF;
    timer_hours = (unsigned char)0x00;
    timer_seconds = (unsigned int)0;
    pm25_sec_cnt = (unsigned char)0x00;
    /* 从 EEPROM 恢复断电前保存的滤网使用度 */
    filter_usage = EEPROM_LoadFilterUsage();
    filter_sub_hr   = (unsigned int)0;      /* 微小时累计: 上电清零 */
    filter_view_mode = VIEW_MODE_PM25;      /* 默认显示 PM2.5 数值栏 */
    led_force_mask  = (unsigned char)0x00;  /* 强制叠加 LED 掩码: 上电无叠加 */
    timer_k1_idle_sec = (unsigned char)0;   /* K1 空闲秒数初始化 */
    timer_led1_blink  = (unsigned char)0;   /* LED1 闪烁标志初始化 */

    /* 默认关机：传感器也断电 (仅在真正开机时才上电预热) */
    Gas_PowerOff();
    TM1628_AllOff();
    /* 初始 LED 输出: 用户态 | 强制叠加掩码 (上电时 mask=0, 等价原输出) */
    TM1628_SetLeds((unsigned char)(led_state | led_force_mask));

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
                    pm25_sec_cnt = (unsigned char)0x00;
                    filter_sub_hr = (unsigned int)0;      /* 清除微小时累计，避免跨开机跳变 */
                    Timer0_ResetTick();
                    {
                        Gas_PowerOn();                                 /* 异味传感器上电 */
                        Gas_StartWarmup();                             /* 启动 30 秒预热倒计时 */
                        TM1628_SetDefaultDisplay();
                        /* 预热期间显示倒计时秒数 */
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
                    filter_sub_hr = (unsigned int)0;          /* 关机不累计（累计值重置，主值保留） */
                    Timer0_ResetTick();
                    Gas_PowerOff();                                /* 异味传感器断电 */
                    TM1628_AllOff();
                }
            }

            /* 仅在开机状态下同步其余 DP */
            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
            {
                /* 定时 DP：重置计时基准并更新显示 */
                timer_hours = dp_timer;
                timer_seconds = (unsigned int)0;
                timer_k1_idle_sec = (unsigned char)0;   /* 云端设置定时, 重置空闲计数 */
                timer_led1_blink  = (unsigned char)0;   /* 退出闪烁 */
                /* 注意: 不调 Timer0_ResetTick(), 否则预热倒计时会停滞约 1 秒 */

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

            /* 亮度 DP: 开机 / 关机都可调, 共用同一份 last_brightness_shadow 静态缓存.
               原代码把亮度逻辑拆在 "开机分支里一个 static" + "关机分支里另一个 static" 中,
               占用 2B RAM; 现合并为单个文件级 static, 省 1B.
               开机/关机分支互斥执行, 且 dp_brightness 是同一个全局, 共用缓存不会出错. */
            {
                static unsigned char last_brightness_shadow = 2;

                if (dp_brightness != last_brightness_shadow)
                {
                    last_brightness_shadow = dp_brightness;
                    TM1628_SetBrightness(dp_brightness);
                }
            }

            /* 所有 SetLeds 出口叠加 led_force_mask(滤网>90告警强制亮LED3),
               不改动用户态 led_state，保证关机判定等逻辑不受影响。 */
            TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
        }

        /* ---- 段2: 按键处理 ---- */
        key_now = Key_ReadStable();
        key_down = (unsigned char)(key_now & (unsigned char)(~key_last));  /* 上升沿 */

        /* K3 长按检测：按住约 2 秒触发 AP 配网，数码管全显示 "1" 提示 */
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
         *   - 按下累计 k2_hold_ticks: 达 K2_LONG_PRESS_TICKS 且 system_on 时触发长按
         *     (翻转 LED2 并上报 DP104), 置 k2_long_fired=1 阻止释放时再触发短按.
         *   - 释放瞬间: 若 k2_long_fired==0 且 ticks>=最小短按阈值, 判定为短按.
         *   - 关机时 (K3 未触发) 所有按键无效。
         */
        {
            static unsigned int  k2_hold_ticks = 0;
            static unsigned char k2_long_fired = 0;
            unsigned char k2_phy_pressed;   /* 物理键是否按下 (不看开关机) */
            unsigned char system_on;        /* 是否处于开机状态 (LED3 亮) */

            k2_phy_pressed = (unsigned char)(((key_now  & KEY_MASK_K2) != (unsigned char)0x00));
            system_on      = (unsigned char)(((led_state & LED_MASK_3) != (unsigned char)0x00));

            if (k2_phy_pressed != (unsigned char)0x00)
            {
                /* ---- 按下态: 累计 tick, 长按需开机才生效 ---- */
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
                        led_state = (unsigned char)(led_state ^ LED_MASK_2);
                        /* 上报云端 DP104: 滤网指示开关变更 */
                        WIFI_ReportIndicator((unsigned char)0x00);
                    }
                    else
                    {
                        /* LED2 灭: 翻转点亮 (原长按行为) */
                        led_state = (unsigned char)(led_state ^ LED_MASK_2);
                        /* 上报云端 DP104: 滤网指示开关变更 */
                        WIFI_ReportIndicator(
                            ((led_state & LED_MASK_2) != (unsigned char)0x00)
                                ? (unsigned char)0x01
                                : (unsigned char)0x00);
                    }
                    /* 所有 SetLeds 出口叠加 led_force_mask(滤网>90告警强制亮LED3),
               不改动用户态 led_state，保证关机判定等逻辑不受影响。 */
            TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
                }
            }
            else
            {
                /* ---- 非按下态 ---- */

                /* 上一轮是按下态 → 检测到释放边沿 */
                if ((key_last & KEY_MASK_K2) != (unsigned char)0x00)
                {
                    if (k2_long_fired == (unsigned char)0x01)
                    {
                        /* Bug1 防御: 本次按键已被长按吞掉, 释放时绝对不触发短按.
                           显式写一条跳过路径, 避免任何逻辑修改后意外进入短按分支. */
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
                                /* 预热中: 显示倒计时 */
                                TM1628_SetPm25Display((unsigned int)Gas_GetWarmupRemaining());
                            }
                        }
                        /* 短按仅切换显示视图, 不刷新 LED 状态 — 避免
                           因 4a2 定时闪烁修改了 led_state 但尚未写入硬件,
                           导致此处 SetLeds 意外将闪烁中的 led_state 刷到硬件,
                           表现为"K2 点按翻转 LED1"。 */
                    }
                    /* 关机状态下的短按: 直接忽略 (不切视图也不改 LED) */
                }

                /* 释放/未按: 重置计数与标志 */
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
                /* ---- 按下态: 累计 tick, 长按清空定时 ---- */
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
                /* ---- 非按下态 ---- */

                /* 上一轮是按下态 → 检测到释放边沿 */
                if ((key_last & KEY_MASK_K1) != (unsigned char)0x00 && system_on)
                {
                    if (k1_long_fired == (unsigned char)0x00
                        && k1_hold_ticks >= (unsigned int)2
                        && k1_hold_ticks < (K2_LONG_PRESS_TICKS - (unsigned int)50))
                    {
                        /* 短按: 小时数 0→24 循环递增
                         * 增加 k1_hold_ticks < (阈值-50) 条件, 防止长按过程中
                         * 触摸键偶发抖动(瞬间跳变为松开)误触发短按递增。
                         * 达 ~450 tick 后即使抖动也只被判定为"准备长按中", 不触发短按,
                         * 最终达到 ~500 tick 触发长按清空。 */
                        if (timer_hours < TIMER_MAX_HOURS)
                            timer_hours++;
                        else
                            timer_hours = (unsigned char)0x00;

                        timer_seconds = (unsigned int)0;
                        timer_k1_idle_sec = (unsigned char)0;   /* 重置空闲计数 */
                        timer_led1_blink  = (unsigned char)0;   /* 退出闪烁 */

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

        /* ---- K4 风速键状态机 (长按恢复默认 LOW / 短按循环递增) ---- */
        {
            static unsigned int  k4_hold_ticks = 0;
            static unsigned char k4_long_fired = 0;
            unsigned char system_on;

            system_on = (unsigned char)(((led_state & LED_MASK_3) != (unsigned char)0x00));

            if ((key_now & KEY_MASK_K4) != (unsigned char)0x00 && system_on)
            {
                /* ---- 按下态: 累计 tick, 长按恢复默认 ---- */
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
                /* ---- 非按下态 ---- */

                /* 上一轮是按下态 → 检测到释放边沿 */
                if ((key_last & KEY_MASK_K4) != (unsigned char)0x00 && system_on)
                {
                    if (k4_long_fired == (unsigned char)0x00
                        && k4_hold_ticks >= (unsigned int)2)
                    {
                        /* 短按: 档位 0→3 循环递增，0 时关风速 LED */
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
            /* K3 电源键（短按）：开关机切换并上报云端 */
            if ((key_down & KEY_MASK_K3) != (unsigned char)0x00)
            {
                if ((led_state & LED_MASK_3) != (unsigned char)0x00)
                {
                    led_state = (unsigned char)0x00;
                    fan_speed_level = FAN_SPEED_LEVEL_OFF;
                    timer_hours = (unsigned char)0x00;
                    timer_seconds = (unsigned int)0;
                    pm25_sec_cnt = (unsigned char)0x00;
                    filter_sub_hr = (unsigned int)0;            /* 关机清零微小时累计, 主值保留 */
                    Timer0_ResetTick();
                    Gas_PowerOff();                                /* 异味传感器断电 */
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
                    filter_sub_hr = (unsigned int)0;              /* 开机从零累计微小时 */
                    Timer0_ResetTick();
                    Gas_PowerOn();                                 /* 异味传感器上电 */
                    Gas_StartWarmup();                             /* 启动 30 秒预热倒计时 */
                    TM1628_SetDefaultDisplay();
                    /* 预热期间显示倒计时秒数 */
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

            /* 所有 SetLeds 出口叠加 led_force_mask(滤网>90告警强制亮LED3),
               不改动用户态 led_state，保证关机判定等逻辑不受影响。 */
            TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
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
                                pm25_sec_cnt = (unsigned char)0x00;
                                filter_sub_hr = (unsigned int)0;          /* 开机清零微小时累计 */
                                Timer0_ResetTick();
                                Gas_PowerOn();                                 /* 异味传感器上电 */
                                Gas_StartWarmup();                             /* 启动 30 秒预热倒计时 */
                                TM1628_SetDefaultDisplay();
                                /* 预热期间显示倒计时秒数 */
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

                        /* 关机：无条件关闭所有输出 */
                        case VOICE_CMD_TURN_OFF:
                            led_state = (unsigned char)0x00;
                            fan_speed_level = FAN_SPEED_LEVEL_OFF;
                            timer_hours = (unsigned char)0x00;
                            timer_seconds = (unsigned int)0;
                            pm25_sec_cnt = (unsigned char)0x00;
                            filter_sub_hr = (unsigned int)0;              /* 关机清零微小时累计，主值保留 */
                            Timer0_ResetTick();
                            Gas_PowerOff();                                    /* 异味传感器断电 */
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
                                timer_k1_idle_sec = (unsigned char)0;   /* 语音设置定时, 重置空闲计数 */
                                timer_led1_blink  = (unsigned char)0;   /* 退出闪烁 */
                                /* 注意: 不调 Timer0_ResetTick(), 否则预热倒计时会停滞约 1 秒 */

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

            /* 所有 SetLeds 出口叠加 led_force_mask(滤网>90告警强制亮LED3),
               不改动用户态 led_state，保证关机判定等逻辑不受影响。 */
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
                        pm25_sec_cnt = (unsigned char)0x00;
                        filter_sub_hr = (unsigned int)0;                /* 关机清零微小时累计，主值保留 */
                        timer_k1_idle_sec = (unsigned char)0;           /* 定时归零, 复位空闲计数 */
                        timer_led1_blink  = (unsigned char)0;           /* 停止闪烁 */
                        Timer0_ResetTick();
                        Gas_PowerOff();                                /* 异味传感器断电 */
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
                    /* 定时运行中: 累计空闲秒数 */
                    if (timer_k1_idle_sec < (unsigned char)5)
                        timer_k1_idle_sec++;

                    if (timer_k1_idle_sec >= (unsigned char)5)
                    {
                        /* 超过 5s 无 K1 操作: LED1 闪烁 */
                        timer_led1_blink = (unsigned char)1;
                        led_state = (unsigned char)(led_state ^ LED_MASK_1);
                        TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
                    }
                    else
                    {
                        timer_led1_blink = (unsigned char)0;
                        /* 未进入闪烁: 确保 LED1 常亮 */
                        led_state = (unsigned char)(led_state | LED_MASK_1);
                        TM1628_SetLeds((unsigned char)(led_state | led_force_mask));
                    }
                }
                else
                {
                    /* 定时关闭: 清空闪烁状态 (LED1 由外部逻辑关闭) */
                    timer_k1_idle_sec = (unsigned char)0;
                    timer_led1_blink  = (unsigned char)0;
                }
            }

            /* 4b: PM2.5 栏每 2 秒刷新一次；显示模式 filter_view_mode 决定内容
             *   VIEW_MODE_PM25   → 刷新异味传感器实时 PM2.5 数值 (需要 ADC 采样)
             *   VIEW_MODE_FILTER → 刷新滤网使用度显示 (本地变量 filter_usage)
             * 每秒都会进入 4c（滤网使用度累算）；但只每 2 秒刷新一次 PM2.5 栏。
             */
            if ((led_state & LED_MASK_3) != (unsigned char)0x00)
            {
                pm25_sec_cnt++;
                if (pm25_sec_cnt >= PM25_REFRESH_INTERVAL_SEC)
                {
                    pm25_sec_cnt = (unsigned char)0x00;

                    if (filter_view_mode == VIEW_MODE_PM25)
                    {
                        /* 预热未完成时不读取传感器, 显示由 4e 每秒更新 */
                        if (Gas_IsWarmupDone())
                        {
                            unsigned int _pm;

                            _pm = Gas_ReadPm25();
                            if (_pm <= GAS_PM25_MAX)               /* 合法值 → 显示 PM2.5 数值 + PM2.5 两 DP 灯亮 */
                            {
                                TM1628_SetPm25Display(_pm);
                            }
                            else
                            {
                                /* Bug2 修复: 传感器开路/拔掉/异常时, 必须主动写一次无效值消隐,
                                   而不是"跳过不刷新" — 否则保留上一帧旧数值, 用户拔掉传感器后
                                   看到的还是上一时刻的 PM2.5 假数据。 */
                                TM1628_SetPm25Display((unsigned int)GAS_PM25_INVALID);
                            }
                        }
                    }
                    else
                    {
                        /* 滤网模式：以 2s 节奏重绘，filter_usage 可能通过 4c 进位后改变 */
                        TM1628_SetFilterUsageDisplay(filter_usage);
                    }
                }
            }
            else
            {
                /* 关机状态: 不累计, 避免开机时立刻就触发刷新 */
                pm25_sec_cnt = (unsigned char)0x00;
            }

            /* 4c: 滤网使用度累算 (仅开机状态)
             * 依据当前 PM2.5 值分档，每秒向 filter_sub_hr 加上对应档的"微单位"：
             *   PM2.5 < 100        → 轻度污染, 每小时 +2 使用度 → 每秒 sub += 2, 满 3600 即 +2
             *   100 ≤ PM2.5 < 200  → 中度污染, 每小时 +4       → 每秒 sub += 4, 满 3600 即 +4
             *   PM2.5 ≥ 200        → 重度污染, 每小时 +8       → 每秒 sub += 8, 满 3600 即 +8
             * filter_usage 饱和上限 FILTER_USAGE_MAX = 100；超过后不继续累计 (归零需用户自行设置或换滤网后重启)
             */
            if ((led_state & LED_MASK_3) != (unsigned char)0x00
                && filter_usage < FILTER_USAGE_MAX)
            {
                unsigned int  _pm_now;
                unsigned char add_sub;

                /* 预热未完成时不读取传感器, 不累计滤网使用度 */
                if (Gas_IsWarmupDone())
                {
                    _pm_now = Gas_ReadPm25();
                }
                else
                {
                    _pm_now = GAS_PM25_INVALID;
                }

                /* Bug2 关联修复: 传感器开路/未上电/异常时, PM 值不可用 → 不累计滤网使用度
                   (不加 add_sub=0, 直接跳过本段), 避免拔掉传感器后被误判成 "重污染档 +8"
                   造成 1 小时净涨 8 使用度的荒谬现象。 */
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
                    /* 每秒加入对应数量的"微单位"，最大不超过 FILTER_SUB_HR_PER_UNIT*8
                       极端情况 filter_sub_hr 可达 (3600*2-1 + 8)，unsigned int 足够装 */
                    filter_sub_hr = filter_sub_hr + (unsigned int)add_sub;

                    /* 满 FILTER_SUB_HR_PER_UNIT (3600) 微单位 → 进位 1 使用度
                       但 add_sub 是 2/4/8，一次可能跨过多个 3600，用 while 一次性处理所有进位
                       同时保证每个进位单元都带来 1 使用度 */
                    while (filter_sub_hr >= FILTER_SUB_HR_PER_UNIT)
                        {
                            filter_sub_hr = filter_sub_hr - FILTER_SUB_HR_PER_UNIT;
                            if (filter_usage < FILTER_USAGE_MAX)
                            {
                                filter_usage++;
                                EEPROM_SaveFilterUsage(filter_usage);
                            }
                        else
                        {
                            /* 已到上限，丢弃剩余微单位防止溢出 */
                            filter_sub_hr = (unsigned int)0;
                            break;
                        }
                    }
                }
            }
            else if ((led_state & LED_MASK_3) == (unsigned char)0x00)
            {
                /* 关机：不累计也不清零主值；仅清微小时进位缓存 (避免下次开机秒级立刻进位) */
                filter_sub_hr = (unsigned int)0;
            }

            /* 4d: 滤网使用度告警 LED3 (独立于开关机) —
               filter_usage > FILTER_USAGE_WARN_THRESH(90) 时, 强制叠加 LED3 亮.
               用独立 mask 不污染 led_state (不影响用户开关机判定逻辑),
               所有 SetLeds 出口统一 OR mask 后输出, 保证 <=90 时立刻恢复用户原状态. */
            if (filter_usage > FILTER_USAGE_WARN_THRESH)
            {
                led_force_mask = (unsigned char)(led_force_mask | LED_MASK_3);
            }
            else
            {
                led_force_mask = (unsigned char)(led_force_mask & (unsigned char)(~LED_MASK_3));
            }

            /* 4e: TP-401W 传感器预热倒计时 (每秒更新, 仅开机状态)
             * 预热期间在 PM2.5 三位数码管显示剩余秒数 (30→1),
             * 预热完成后自动读取首次有效 PM2.5 数值并显示
             * 加 (led_state & LED_MASK_3) 条件, 防止关机时因 warmup_counter
             * 未复位仍继续写 TM1628 显示。 */
            if ((led_state & LED_MASK_3) != (unsigned char)0x00
                && !Gas_IsWarmupDone())
            {
                Gas_TickWarmupSecond();
                if (filter_view_mode == VIEW_MODE_PM25)
                {
                    if (Gas_IsWarmupDone())
                    {
                        /* 预热完成: 读取首次有效 PM2.5 数值 */
                        unsigned int _pm25 = Gas_ReadPm25();
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
                        /* 仍在预热: 显示剩余秒数 */
                        TM1628_SetPm25Display((unsigned int)Gas_GetWarmupRemaining());
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
