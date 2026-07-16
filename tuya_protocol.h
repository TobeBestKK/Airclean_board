#ifndef __TUYA_PROTOCOL_H__
#define __TUYA_PROTOCOL_H__

#include <cms.h>

/* WiFi 模块与 MCU 工作模式: WIFI_MODE_MCU 表示 MCU 负责协议处理 */
#define WIFI_MODE_MCU

/* 9600bps @ 16MHz */
#define UART_SPBRG_9600_16MHZ  ((unsigned char)103)

/*=============================================================================
 * DP (Data Point) 数据类型定义
 *===========================================================================*/
#define DP_TYPE_RAW      0x00   /* RAW */
#define DP_TYPE_BOOL     0x01   /* bool */
#define DP_TYPE_VALUE    0x02   /* value (4 字节整数) */
#define DP_TYPE_STRING   0x03   /* string */
#define DP_TYPE_ENUM     0x04   /* enum */
#define DP_TYPE_BITMAP   0x05   /* fault / bitmap */

/*=============================================================================
 * DP ID 定义 — 必须与涂鸦 IoT 平台产品功能点一致
 *===========================================================================*/
#define DPID_POWER        101   /* 开关, bool */
#define DPID_TIMER        102   /* 定时, value (0~24 h) */
#define DPID_FAN_SPEED    103   /* 风速档位, enum (0~3) */
#define DPID_INDICATOR    104   /* 状态指示灯/滤网, bool */
#define DPID_BRIGHTNESS   105   /* 显示屏亮度, enum (0~3; 3=省电熄屏) */
#define DPID_PM25         106   /* PM2.5, value (0~999) */
#define DPID_FILTER_USAGE 107   /* 滤网使用度, value (0~100) */

/*=============================================================================
 * WiFi 模块连接状态 (由模组上报)
 *===========================================================================*/
#define WIFI_STATE_SMARTCONFIG  0  /* SmartConfig 配网模式 */
#define WIFI_STATE_AP           1  /* AP 配网模式 */
#define WIFI_STATE_CONNECTING   2  /* WiFi 已连接, 未连上云端 */
#define WIFI_STATE_CLOUD_LINKED 3  /* 已连上云端 */
#define WIFI_STATE_CLOUD_ACTIVE 4  /* 云端在线, 设备已激活 */

/*=============================================================================
 * 语音命令值 (SU-03T 语音模块)
 *===========================================================================*/
#define VOICE_CMD_WAKEUP     ((unsigned char)0xA0)
#define VOICE_CMD_TURN_ON    ((unsigned char)0xA1)
#define VOICE_CMD_TURN_OFF   ((unsigned char)0xA2)
#define VOICE_CMD_SPEED_UP   ((unsigned char)0xA3)
#define VOICE_CMD_SPEED_DOWN ((unsigned char)0xA4)
#define VOICE_CMD_TIMER_ON   ((unsigned char)0xA5)
#define VOICE_CMD_FILTER     ((unsigned char)0xA6)
#define VOICE_CMD_TIMER_DOWN ((unsigned char)0xA7)
#define VOICE_CMD_TIME_1     ((unsigned char)0xA8)
#define VOICE_CMD_TIME_2     ((unsigned char)0xA9)
#define VOICE_CMD_TIME_3     ((unsigned char)0xB0)
#define VOICE_CMD_TIME_4     ((unsigned char)0xB1)
#define VOICE_CMD_TIME_5     ((unsigned char)0xB2)
#define VOICE_CMD_TIME_6     ((unsigned char)0xB3)
#define VOICE_CMD_TIME_7     ((unsigned char)0xB4)
#define VOICE_CMD_TIME_8     ((unsigned char)0xB5)
#define VOICE_CMD_TIME_9     ((unsigned char)0xB6)
#define VOICE_CMD_TIME_10    ((unsigned char)0xB7)
#define VOICE_CMD_TIME_11    ((unsigned char)0xB8)
#define VOICE_CMD_TIME_12    ((unsigned char)0xB9)
#define VOICE_CMD_TIME_13    ((unsigned char)0xC0)
#define VOICE_CMD_TIME_14    ((unsigned char)0xC1)
#define VOICE_CMD_TIME_15    ((unsigned char)0xC2)
#define VOICE_CMD_TIME_16    ((unsigned char)0xC3)
#define VOICE_CMD_TIME_17    ((unsigned char)0xC4)
#define VOICE_CMD_TIME_18    ((unsigned char)0xC5)
#define VOICE_CMD_TIME_19    ((unsigned char)0xC6)
#define VOICE_CMD_TIME_20    ((unsigned char)0xC7)
#define VOICE_CMD_TIME_21    ((unsigned char)0xC8)
#define VOICE_CMD_TIME_22    ((unsigned char)0xC9)
#define VOICE_CMD_TIME_23    ((unsigned char)0xD0)
#define VOICE_CMD_TIME_24    ((unsigned char)0xD1)
#define VOICE_CMD_TIMER_OFF  ((unsigned char)0xD2)
#define VOICE_CMD_SPEED_OFF  ((unsigned char)0xD3)
#define VOICE_CMD_SPEED_1    ((unsigned char)0xD4)
#define VOICE_CMD_SPEED_2    ((unsigned char)0xD5)
#define VOICE_CMD_SPEED_3    ((unsigned char)0xD6)

#define VOICE_CMD_IS_VALID(value) \
    ((((value) >= (unsigned char)0xA0) && ((value) <= (unsigned char)0xA9)) || \
     (((value) >= (unsigned char)0xB0) && ((value) <= (unsigned char)0xB9)) || \
     (((value) >= (unsigned char)0xC0) && ((value) <= (unsigned char)0xC9)) || \
     (((value) >= (unsigned char)0xD0) && ((value) <= (unsigned char)0xD6)))

/*=============================================================================
 * 全局变量
 *===========================================================================*/
extern volatile unsigned char wifi_state;       /* WiFi 连接状态 */
extern volatile unsigned char wifi_dp_flag;     /* DP 数据上报请求标志 */
extern volatile unsigned char wifi_dp_changed;  /* 云端 DP 下发变更标志 (main.c 消费后清零) */

/* DP 状态变量 (云端下发 → main.c 读取并应用) */
extern volatile unsigned char dp_power;         /* DP 101: 开关 */
extern volatile unsigned char dp_timer;         /* DP 102: 定时, 0~24 h */
extern volatile unsigned char dp_fan_speed;     /* DP 103: 风速, 0~3 */
extern volatile unsigned char dp_indicator;     /* DP 104: 状态指示灯/滤网 */
extern volatile unsigned char dp_brightness;    /* DP 105: 亮度, 0~3 (3=省电熄屏) */
extern volatile unsigned int  dp_pm25;          /* DP 106: PM2.5, 0~999 */
extern volatile unsigned char dp_filter_usage;  /* DP 107: 滤网使用度, 0~100 */

/*=============================================================================
 * 公开 API
 *===========================================================================*/

/* 初始化: 配置 EUSART (RC0/RC1), 9600bps, 使能接收中断 */
void WIFI_Init(void);

/* 中断服务: 从 RCREG 读取字节压入环形缓冲区 (由 ISR 调用) */
void WIFI_ISR_Rx(void);

/* 主循环处理: 解析 WiFi 协议帧 + 响应涂鸦命令 (由主循环周期调用) */
void WIFI_Process(void);

/* 复位 WiFi: 进入 SmartConfig / AP 配网模式 */
void WIFI_Reset(void);
void WIFI_ResetAP(void);

/* 上报全部 DP 数据到云端 */
void WIFI_ReportAll(void);

/* DP 上报 — 当本地状态变化时调用 */
void WIFI_ReportPower(unsigned char on);
void WIFI_ReportTimer(unsigned char hours);
void WIFI_ReportFanSpeed(unsigned char gear);
void WIFI_ReportIndicator(unsigned char on);
void WIFI_ReportBrightness(unsigned char level);

void WIFI_SetPm25(unsigned int value);
void WIFI_SetFilterUsage(unsigned char usage);
void WIFI_ReportPm25(unsigned int value);
void WIFI_ReportFilterUsage(unsigned char usage);

/* 提取语音命令字节 (语音与 WiFi 共享同一 UART)。
   返回 0 = 无语音命令, 否则返回合法的 A0~D6 语音命令码 */
unsigned char WIFI_GetVoiceCommand(void);

#endif /* __TUYA_PROTOCOL_H__ */
