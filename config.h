#ifndef __AIRCLEAN_CONFIG_H__
#define __AIRCLEAN_CONFIG_H__

/* ===== 定时与触摸扫描 ===== */
#define TIMER_SECONDS_PER_HOUR           ((unsigned int)3600)
#define TIMER_MAX_HOURS                  ((unsigned char)24)
#define TOUCH_TMR2_TICKS_PER_SCAN        ((unsigned char)32)

/* ===== 长按阈值 (主循环 ~4ms × N, 约 2 秒) ===== */
#define K3_LONG_PRESS_TICKS              ((unsigned int)500)   /* K3 长按配网 */
#define K2_LONG_PRESS_TICKS              ((unsigned int)500)   /* K2 长按滤网指示 */

/* ===== PM2.5 显示刷新 (秒, 由 Timer0 秒节拍累计) ===== */
#define PM25_REFRESH_INTERVAL_SEC        ((unsigned char)2)

/* ===== 滤网使用度 (0~100) =====
   FILTER_SUB_HR_PER_UNIT: 每 1% 使用度对应累计秒数 (1 微小时 = 3600s)
   FILTER_ADD_*:           按污染等级每秒累计的微小时数 */
#define FILTER_USAGE_MAX                 ((unsigned char)100)
#define FILTER_SUB_HR_PER_UNIT           ((unsigned int)3600)
#define FILTER_ADD_LIGHT                 ((unsigned char)2)
#define FILTER_ADD_MEDIUM                ((unsigned char)4)
#define FILTER_ADD_HEAVY                 ((unsigned char)8)
#define PM25_LEVEL_LIGHT_MAX             ((unsigned int)150)
#define PM25_LEVEL_MEDIUM_MAX            ((unsigned int)300)

/* 滤网使用度告警阈值 */
#define FILTER_USAGE_WARN_THRESH         ((unsigned char)90)

/* PM2.5 / 滤网使用度 显示切换模式 */
#define VIEW_MODE_PM25                   ((unsigned char)0x00)
#define VIEW_MODE_FILTER                 ((unsigned char)0x01)


#endif /* __AIRCLEAN_CONFIG_H__ */
