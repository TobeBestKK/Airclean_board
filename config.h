#ifndef __AIRCLEAN_CONFIG_H__
#define __AIRCLEAN_CONFIG_H__

/* 定时与触摸扫描常量 */
#define TIMER_SECONDS_PER_HOUR           ((unsigned int)3600)
#define TIMER_MAX_HOURS                  ((unsigned char)24)
#define TOUCH_TMR2_TICKS_PER_SCAN        ((unsigned char)32)

/* K3 长按配网阈值：约 2 秒 (主循环 ~4ms × 500) */
#define K3_LONG_PRESS_TICKS              ((unsigned int)500)

/* K2 长按滤网指示灯阈值：约 2 秒 (与 K3 相同，统一操作手感) */
#define K2_LONG_PRESS_TICKS              ((unsigned int)500)

/* PM2.5 异味显示刷新间隔 (单位: 秒，使用 Timer0 秒节拍累计) */
#define PM25_REFRESH_INTERVAL_SEC        ((unsigned char)2)

/* 滤网使用度 (0~100)：上限与微小时进位阈值 */
#define FILTER_USAGE_MAX                 ((unsigned char)100)
#define FILTER_SUB_HR_PER_UNIT           ((unsigned int)3600)
#define FILTER_ADD_LIGHT                 ((unsigned char)2)
#define FILTER_ADD_MEDIUM                ((unsigned char)4)
#define FILTER_ADD_HEAVY                 ((unsigned char)8)
#define PM25_LEVEL_LIGHT_MAX             ((unsigned int)150)
#define PM25_LEVEL_MEDIUM_MAX            ((unsigned int)300)

/* 滤网使用度告警阈值 */
#define FILTER_USAGE_WARN_THRESH         ((unsigned char)90)

/* PM2.5 与滤网使用度显示模式 */
#define VIEW_MODE_PM25                   ((unsigned char)0x00)
#define VIEW_MODE_FILTER                 ((unsigned char)0x01)

#endif /* __AIRCLEAN_CONFIG_H__ */
