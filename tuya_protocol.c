#include "tuya_protocol.h"

/*=============================================================================
 * 涂鸦串口通讯协议 (MCU 端), 适配 CMS79F723 EUSART (RC0/TX, RC1/RX, 9600bps)
 *
 * 帧结构: [55 AA] [版本字段] [命令] [长度H] [长度L] [数据...] [校验和]
 * 校验和 = (帧头 + 版本 + 命令 + 长度H + 长度L + 数据各字节) % 256
 *
 * 注意:
 *   1. ProductkeyMcuv[] 内 PID 须替换为涂鸦 IoT 平台实际产品 ID
 *   2. DP ID 定义见 tuya_protocol.h, 须与平台一致
 *   3. 语音模块 (SU-03T) 与 WiFi 模块共享同一 UART, 按字节值区分:
 *      0x55 开头 → WiFi 协议帧; 合法 A0~D6 命令字节 → 语音队列
 *===========================================================================*/

/*=============================================================================
 * 常量数据: 涂鸦协议预置帧
 *===========================================================================*/

/* 产品 ID / MCU 版本信息。JSON: {"p":"xidx1m5hf2birubg","v":"1.0.0","m":0} */
const unsigned char ProductkeyMcuv[] = {
    0x55, 0xAA, 0x03, 0x01, 0x00, 0x2A,
    0x7B, 0x22, 0x70, 0x22, 0x3A, 0x22,
    0x78, 0x69, 0x64, 0x78, 0x31, 0x6D, 0x35, 0x68, 0x66, 0x32,
    0x62, 0x69, 0x72, 0x75, 0x62, 0x67,
    0x22, 0x2C, 0x22, 0x76, 0x22, 0x3A, 0x22, 0x31, 0x2E, 0x30,
    0x2E, 0x30, 0x22, 0x2C,
    0x22, 0x6D, 0x22, 0x3A, 0x30, 0x7D,
    0xFA  /* checksum: sum(55..7D) % 256 = 0xFA */
};

/* 心跳包应答 */
const unsigned char HeartbeatAck[] = {
    0x55, 0xAA, 0x03, 0x00, 0x00, 0x01, 0x01, 0x04
};

/* WiFi 状态查询应答 (无 DP 数据时) */
const unsigned char WifistaAck[] = {
    0x55, 0xAA, 0x03, 0x03, 0x00, 0x00, 0x05
};

/* 复位 WiFi 指令 */
const unsigned char ResetWifi[] = {
    0x55, 0xAA, 0x03, 0x04, 0x00, 0x00, 0x06
};

/* 配网模式选择: SmartConfig */
const unsigned char ResetWifiModeSmart[] = {
    0x55, 0xAA, 0x03, 0x05, 0x00, 0x01, 0x00, 0x08
};

/* 配网模式选择: AP */
const unsigned char ResetWifiModeAp[] = {
    0x55, 0xAA, 0x03, 0x05, 0x00, 0x01, 0x01, 0x09
};

/* WiFi 模组自处理模式 (模组自己管理 DP) */
const unsigned char WifiModeAuto[] = {
    0x55, 0xAA, 0x03, 0x02, 0x00, 0x02, 0x05, 0x00, 0x0B
};

/* WiFi 模组 MCU 处理模式 (MCU 管理 DP, 使用此模式) */
const unsigned char WifiModeMcu[] = {
    0x55, 0xAA, 0x03, 0x02, 0x00, 0x00, 0x04
};

/*=============================================================================
 * UART 环形缓冲区 (中断接收 → 主循环消费)
 *===========================================================================*/
#define UART_BUF_SIZE  16    /* 当前支持的 value 帧最长 15B (含校验), 16B 可容纳一帧 */

typedef struct {
    unsigned char head;
    unsigned char tail;
    unsigned char buf[UART_BUF_SIZE];
} CircleBuffer;

static CircleBuffer uart_rx_buf;

/* 压入一字节 (ISR 调用), 满时覆盖最早数据 */
static void UART_BufPush(unsigned char dat)
{
    uart_rx_buf.buf[uart_rx_buf.tail] = dat;
    if (++uart_rx_buf.tail >= UART_BUF_SIZE)
        uart_rx_buf.tail = 0;
    if (uart_rx_buf.tail == uart_rx_buf.head)   /* 缓冲区满: 覆盖最早数据 */
    {
        if (++uart_rx_buf.head >= UART_BUF_SIZE)
            uart_rx_buf.head = 0;
    }
}

/* 弹出一字节 (主循环调用), 返回 1=成功, 0=空 */
static unsigned char UART_BufPop(unsigned char *dat)
{
    if (uart_rx_buf.head == uart_rx_buf.tail)
        return 0;

    *dat = uart_rx_buf.buf[uart_rx_buf.head];
    if (++uart_rx_buf.head >= UART_BUF_SIZE)
        uart_rx_buf.head = 0;
    return 1;
}

/*=============================================================================
 * 语音命令队列
 * 协议解析器遇合法语音命令字节时分流到此队列, 供 main.c 消费。
 *===========================================================================*/
#define VOICE_CMD_QUEUE_SIZE  4   /* 用户每秒最多 1~2 条语音命令, 4 条容量够 */

static unsigned char voice_cmd_queue[VOICE_CMD_QUEUE_SIZE];
static unsigned char voice_cmd_head = 0;  /* 读指针 */
static unsigned char voice_cmd_tail = 0;  /* 写指针 */

/* 入队一字节, 满时丢弃最旧命令 */
static void VoiceCmd_Push(unsigned char cmd)
{
    voice_cmd_queue[voice_cmd_tail] = cmd;
    if (++voice_cmd_tail >= VOICE_CMD_QUEUE_SIZE)
        voice_cmd_tail = 0;
    if (voice_cmd_tail == voice_cmd_head)   /* 队列满: 丢弃最旧命令 */
    {
        if (++voice_cmd_head >= VOICE_CMD_QUEUE_SIZE)
            voice_cmd_head = 0;
    }
}

/* 出队一个语音命令, 返回 0 表示队列空 */
unsigned char WIFI_GetVoiceCommand(void)
{
    unsigned char cmd;
    if (voice_cmd_head == voice_cmd_tail)
        return 0;
    cmd = voice_cmd_queue[voice_cmd_head];
    if (++voice_cmd_head >= VOICE_CMD_QUEUE_SIZE)
        voice_cmd_head = 0;
    return cmd;
}

/*=============================================================================
 * UART 底层发送
 *===========================================================================*/

/* 发送一字节, 阻塞等待移位寄存器空 */
static void UART_SendByte(unsigned char dat)
{
    TXREG = dat;
    while (TRMT == 0)
    {
    }
}

/* 发送指定长度数据 (不追加 CR/LF) */
static void UART_SendBuf(const unsigned char *buf, unsigned char len)
{
    while (len)
    {
        UART_SendByte(*buf);
        buf++;
        len--;
    }
}

/*=============================================================================
 * UART 初始化 (9600bps @ 16MHz)
 *===========================================================================*/
void WIFI_Init(void)
{
    /* RC0=TX (输出), RC1=RX (输入) */
    TRISC0 = 0;
    TRISC1 = 1;

    /* Baud = Fosc / (16*(SPBRG+1)) → 16MHz/(16*104) ≈ 9615 bps (误差 0.16%) */
    SPBRG = UART_SPBRG_9600_16MHZ;

    /* TXSTA: TXEN=1, SYNC=0, BRGH=0 → 0x20
       RCSTA: SPEN=1, CREN=1 → 0x90 */
    TXSTA = 0x20;
    RCSTA = 0x90;

    /* RCIE=1, TXIE=0; 保留 TMR2IE(bit1) 不动 */
    PIE1 = (unsigned char)(PIE1 | 0x20);
    PIE1 = (unsigned char)(PIE1 & ~0x10);

    /* 清空环形缓冲区与语音命令队列 */
    uart_rx_buf.head = 0;
    uart_rx_buf.tail = 0;
    voice_cmd_head = 0;
    voice_cmd_tail = 0;
}

/*=============================================================================
 * UART 接收 ISR: 由 main.c 的中断在 RCIF 置位时调用
 *===========================================================================*/
void WIFI_ISR_Rx(void)
{
    /* FERR: 帧错误 → 读取丢弃, 清 RCIF
       OERR: 溢出错误 → 必须重置 CREN, 否则后续无法接收 */
    if (FERR)
    {
        (void)RCREG;
    }
    else
    {
        UART_BufPush(RCREG);
    }

    if (OERR)
    {
        CREN = 0;
        CREN = 1;
    }
}

/*=============================================================================
 * 涂鸦协议状态机: 解析 WiFi 模组发来的帧
 *===========================================================================*/

static void dpInfHandle(unsigned char dpId, unsigned long dpData);
static void alldpUpdate(void);

static unsigned char wifi_buf_state = 0;   /* 状态机当前状态 */
static unsigned char wifi_buf_idx = 0;     /* 当前帧已收字节数 */
static unsigned char wifi_checksum = 0;    /* 累加校验和 (8-bit) */
static unsigned char wifi_dat_count = 0;   /* DP 数据部分已收字节数 */
static unsigned char wifi_rx_buf[16];      /* 当前支持的 value 帧最长 15B (含校验), 16B 足够 */

volatile unsigned char wifi_cmd = 0;       /* WiFi 模组下发的命令字 */
volatile unsigned char wifi_state = 5;     /* WiFi 连接状态 (由模组上报) */
volatile unsigned char wifi_dp_flag = 0;   /* DP 全量上报请求标志 */

/* wifi_UART_Service: 从环形缓冲区逐字节解析 WiFi 协议帧;
   遇合法语音字节分流到语音队列。 */
static void wifi_UART_Service(void)
{
    unsigned char dat;
    unsigned long dp_data = 0;

    while (UART_BufPop(&dat) == 1)
    {
        wifi_rx_buf[wifi_buf_idx] = dat;

        switch (wifi_buf_state)
        {
        case 0:  /* 等待帧头 0x55 */
            if (dat == 0x55)
            {
                wifi_buf_state = 1;
                wifi_checksum = dat;
                wifi_buf_idx = 1;
            }
            else if (VOICE_CMD_IS_VALID(dat))
            {
                VoiceCmd_Push(dat);          /* 语音命令: 分流到语音队列 */
                wifi_buf_idx = 0;
            }
            /* 其他字节静默丢弃 */
            break;

        case 1:  /* 等待帧头 0xAA */
            if (dat == 0xAA)
            {
                wifi_buf_state = 2;
                wifi_checksum += dat;
                wifi_buf_idx++;
            }
            else if (dat == 0x55)
            {
                wifi_rx_buf[0] = 0x55;       /* 重新开始 */
                wifi_checksum = dat;
                wifi_buf_idx = 1;
            }
            else
            {
                wifi_buf_state = 0;
                wifi_buf_idx = 0;
            }
            break;

        case 2:  /* 检查版本号 (接收允许 0x00/0x03, 上报使用 0x03) */
            if (dat == 0x00 || dat == 0x03)
            {
                wifi_buf_state = 3;
                wifi_checksum += dat;
                wifi_buf_idx++;
            }
            else
            {
                wifi_buf_state = 0;
                wifi_buf_idx = 0;
            }
            break;

        case 3:  /* 检查命令字 */
            if (dat == 0x00 || dat == 0x01 || dat == 0x02 ||
                dat == 0x03 || dat == 0x04 || dat == 0x05 ||
                dat == 0x06 || dat == 0x07 || dat == 0x08)
            {
                wifi_buf_state = 4;
                wifi_checksum += dat;
                wifi_buf_idx++;
            }
            else
            {
                wifi_buf_state = 0;          /* 非法命令, 丢弃整帧 */
                wifi_buf_idx = 0;
            }
            break;

        case 4:  /* 数据长度 (高位 + 低位) */
            if (wifi_buf_idx == 5)
            {
                if (dat == 0x00 || dat == 0x01)
                {
                    wifi_buf_state = 6;      /* 0x03 类命令: 仅 1 字节状态 */
                }
                else
                {
                    wifi_buf_state = 5;      /* 0x06/0x07 命令: 含 DP 数据 */
                }
                wifi_checksum += dat;
                wifi_buf_idx++;
            }
            else  /* wifi_buf_idx == 4, 数据长度高字节, 当前版本总是 0x00 */
            {
                wifi_checksum += dat;
                wifi_buf_idx++;
                wifi_dat_count = 0;
            }
            break;

        case 5:  /* 读取 DP 数据部分 (命令 0x06/0x07) */
            if (++wifi_dat_count <= wifi_rx_buf[5])
            {
                wifi_checksum += dat;
                wifi_buf_idx++;
            }
            else
            {
                wifi_checksum = (unsigned char)(wifi_checksum % 256);

                if (wifi_checksum == dat)   /* 校验通过 */
                {
                    /* 帧结构: [0]=55 [1]=AA [2]=ver [3]=cmd [4]=lenH [5]=lenL
                               [6]=dpid [7]=dptype [8]=dpLenH [9]=dpLenL [10..]=dpData */
                    if (wifi_rx_buf[6] != 0xFF)  /* 非广播 DP */
                    {
                        if (wifi_rx_buf[9] == 4)
                        {
                            /* value 类型: 4 字节大端 */
                            dp_data  = (unsigned long)wifi_rx_buf[10] << 24;
                            dp_data |= (unsigned long)wifi_rx_buf[11] << 16;
                            dp_data |= (unsigned long)wifi_rx_buf[12] << 8;
                            dp_data |= (unsigned long)wifi_rx_buf[13];
                        }
                        else if (wifi_rx_buf[9] == 1)
                        {
                            /* bool/enum 类型: 1 字节 */
                            dp_data = wifi_rx_buf[10];
                        }

                        dpInfHandle(wifi_rx_buf[6], dp_data);
                    }

                    /* 回复 DP 下发应答: 版本 +3, 命令 +1, 校验和共 +4 */
                    wifi_rx_buf[2] = 0x03;
                    wifi_rx_buf[3] = 0x07;
                    wifi_rx_buf[wifi_buf_idx] = (unsigned char)(dat + 4);
                    UART_SendBuf(wifi_rx_buf,
                                 (unsigned char)(7 + wifi_rx_buf[5]));
                }

                wifi_checksum = 0;           /* 复位状态机 */
                wifi_buf_idx = 0;
                wifi_buf_state = 0;
            }
            break;

        case 6:  /* 简短响应帧 (命令 0x00~0x05, 0x08) */
            wifi_cmd = wifi_rx_buf[3];
            wifi_state = wifi_rx_buf[6];

            wifi_checksum = 0;               /* 复位状态机 */
            wifi_buf_idx = 0;
            wifi_buf_state = 0;
            break;

        default:
            wifi_buf_state = 0;
            wifi_buf_idx = 0;
            break;
        }
    }
}

/*=============================================================================
 * 涂鸦协议命令处理
 *===========================================================================*/

static unsigned char wifi_pair_mode = 0;    /* 配网模式: 0=SmartConfig, 1=AP */

/* Settuyawifi: 按 wifi_cmd 做相应应答。由 WIFI_Process() 周期性调用。 */
static void Settuyawifi(void)
{
    switch (wifi_cmd)
    {
    case 0x00:
    {
        /* 模组查询心跳: 第 1 次回复 0x00, 后续回复 0x01。
           若不区分, 模块可能无法进入正常工作状态。 */
        static unsigned char heartbeat_first = 1;

        if (heartbeat_first)
        {
            UART_SendByte(0x55);
            UART_SendByte(0xAA);
            UART_SendByte(0x03);
            UART_SendByte(0x00);
            UART_SendByte(0x00);
            UART_SendByte(0x01);
            UART_SendByte(0x00);   /* 第 1 次 data = 0x00 */
            UART_SendByte(0x03);   /* checksum */
            heartbeat_first = 0;
        }
        else
        {
            UART_SendBuf(HeartbeatAck, 8);  /* data = 0x01, checksum = 0x04 */
        }
        wifi_cmd = 0xFF;
        break;
    }

    case 0x01:
        /* 模组查询产品 ID 和 MCU 版本: 回复产品信息 */
        UART_SendBuf(ProductkeyMcuv, sizeof(ProductkeyMcuv));
        wifi_cmd = 0xFF;
        break;

    case 0x02:
        /* 模组查询工作模式 */
#ifdef WIFI_MODE_MCU
        UART_SendBuf(WifiModeMcu, 7);    /* MCU 处理模式 */
#else
        UART_SendBuf(WifiModeAuto, 9);   /* 模组自处理模式 */
#endif
        wifi_cmd = 0xFF;
        break;

    case 0x03:
        /* WiFi 状态上报确认 */
        UART_SendBuf(WifistaAck, 7);
        wifi_cmd = 0xFF;
        break;

    case 0x04:
        /* 收到 0x04 复位指令, 根据 wifi_pair_mode 选择配网模式 */
        if (wifi_pair_mode == 0)
        {
            UART_SendBuf(ResetWifiModeSmart, 8);  /* SmartConfig */
        }
        else
        {
            UART_SendBuf(ResetWifiModeAp, 8);     /* AP 模式 */
        }
        wifi_cmd = 0xFF;
        break;

    case 0x05:
        /* 配网模式选择确认 (无需额外处理) */
        wifi_cmd = 0xFF;
        break;

    case 0x08:
        /* 模组查询 MCU 状态: 设置上报标志, 由 alldpUpdate 分次上报 */
        wifi_dp_flag = 1;
        wifi_cmd = 0xFF;
        break;

    case 0xFF:
        /* 空闲: 检查是否有待上报的 DP */
        alldpUpdate();
        break;

    default:
        break;
    }
}

/*=============================================================================
 * DP 上报: 构建帧并发送
 *===========================================================================*/

/* mcudpUpdate: 构建 DP 上报帧并发送。
   bool/enum: dpLen=0x01, 帧总长 12; value: dpLen=0x04, 帧总长 15。 */
static void mcudpUpdate(unsigned char dpid,
                        unsigned char dptype,
                        unsigned long data)
{
    unsigned char  tx_buf[15];
    unsigned char  tx_len;
    unsigned int   checksum;
    unsigned char  i;

    tx_buf[0] = 0x55;
    tx_buf[1] = 0xAA;
    tx_buf[2] = 0x03;    /* 版本号 */
    tx_buf[3] = 0x07;    /* 命令: MCU 上报 DP */
    tx_buf[4] = 0x00;    /* 数据长度高字节 */

    if (dptype == DP_TYPE_BOOL || dptype == DP_TYPE_ENUM)
    {
        tx_buf[5] = 0x05;    /* 数据长度 = 5 (dpid+type+lenH+lenL+1字节数据) */
        tx_buf[6] = dpid;
        tx_buf[7] = dptype;
        tx_buf[8] = 0x00;
        tx_buf[9] = 0x01;    /* dp 数据长度低字节 (1 字节) */
        tx_buf[10] = (unsigned char)data;

        checksum = 0;
        for (i = 0; i < 11; i++)
            checksum += tx_buf[i];
        tx_buf[11] = (unsigned char)(checksum % 256);

        tx_len = 12;
    }
    else if (dptype == DP_TYPE_VALUE)
    {
        tx_buf[5] = 0x08;    /* 数据长度 = 8 (dpid+type+lenH+lenL+4字节数据) */
        tx_buf[6] = dpid;
        tx_buf[7] = dptype;
        tx_buf[8] = 0x00;
        tx_buf[9] = 0x04;    /* dp 数据长度低字节 (4 字节) */
        tx_buf[10] = (unsigned char)(data >> 24);
        tx_buf[11] = (unsigned char)(data >> 16);
        tx_buf[12] = (unsigned char)(data >> 8);
        tx_buf[13] = (unsigned char)(data);

        checksum = 0;
        for (i = 0; i < 14; i++)
            checksum += tx_buf[i];
        tx_buf[14] = (unsigned char)(checksum % 256);

        tx_len = 15;
    }
    else
    {
        return;              /* 不支持的类型 */
    }

    UART_SendBuf(tx_buf, tx_len);
}

/*=============================================================================
 * 全部 DP 分次上报 (避免一次发送过多数据阻塞 UART)
 *===========================================================================*/

/* DP 状态变量 (由 dpInfHandle 更新 / main.c 读取) */
volatile unsigned char wifi_dp_changed = 0;  /* 云端下发变更标志 */
volatile unsigned char dp_power       = 0;  /* DP 101: 开关 */
volatile unsigned char dp_timer       = 0;  /* DP 102: 定时 0~24 */
volatile unsigned char dp_fan_speed   = 0;  /* DP 103: 风速 0~3 */
volatile unsigned char dp_indicator   = 0;  /* DP 104: 状态指示灯 */
volatile unsigned char dp_brightness  = 2;  /* DP 105: 亮度, 默认 2 (高); 3=省电熄屏 */

volatile unsigned int  dp_pm25         = 0;
volatile unsigned char dp_filter_usage = 0;

/* alldpUpdate: 分次上报全部 DP (每次调用只报一个, 靠主循环周期调用完成全量上报) */
static void alldpUpdate(void)
{
    static unsigned char dp_index = 0;

    if (wifi_dp_flag == 0)
    {
        dp_index = 0;
        return;
    }

    dp_index++;

    switch (dp_index)
    {
    case 1:
        mcudpUpdate(DPID_POWER, DP_TYPE_BOOL, dp_power);
        break;
    case 2:
        mcudpUpdate(DPID_TIMER, DP_TYPE_VALUE, dp_timer);
        break;
    case 3:
        mcudpUpdate(DPID_FAN_SPEED, DP_TYPE_ENUM, dp_fan_speed);
        break;
    case 4:
        mcudpUpdate(DPID_INDICATOR, DP_TYPE_BOOL, dp_indicator);
        break;
    case 5:
        mcudpUpdate(DPID_BRIGHTNESS, DP_TYPE_ENUM, dp_brightness);
        break;
    case 6:
        mcudpUpdate(DPID_PM25, DP_TYPE_VALUE, dp_pm25);
        break;
    case 7:
        mcudpUpdate(DPID_FILTER_USAGE, DP_TYPE_VALUE, dp_filter_usage);
        break;
    case 8:
        wifi_dp_flag = 0;     /* 上报完毕 */
        dp_index = 0;
        break;
    default:
        dp_index = 0;
        wifi_dp_flag = 0;
        break;
    }
}

/*=============================================================================
 * DP 下发处理: 云 → 本地
 * WiFi 模组下发 DP 时调用, 传入 dpId 与解析后的 dpData。
 *===========================================================================*/
static void dpInfHandle(unsigned char dpId, unsigned long dpData)
{
    switch (dpId)
    {
    case DPID_POWER:                         /* 101: 开关 */
        dp_power = (unsigned char)(dpData & 0xFF);
        wifi_dp_changed = 1;
        break;

    case DPID_TIMER:                         /* 102: 定时, value 0~24 */
        if (dpData <= 24)
            dp_timer = (unsigned char)dpData;
        wifi_dp_changed = 1;
        break;

    case DPID_FAN_SPEED:                     /* 103: 风速, enum 0~3 */
        if (dpData <= 3)
            dp_fan_speed = (unsigned char)dpData;
        wifi_dp_changed = 1;
        break;

    case DPID_INDICATOR:                     /* 104: 状态指示灯, bool */
        dp_indicator = (unsigned char)(dpData & 0xFF);
        wifi_dp_changed = 1;
        break;

    case DPID_BRIGHTNESS:                    /* 105: 亮度, enum 0~3 (0=低,1=中,2=高,3=熄屏) */
        if (dpData <= 3)
            dp_brightness = (unsigned char)dpData;
        wifi_dp_changed = 1;
        break;

    default:
        break;
    }
}

/*=============================================================================
 * 单个 DP 上报 — 供 main.c 在本地状态变化时调用
 *===========================================================================*/

void WIFI_ReportPower(unsigned char on)
{
    dp_power = (on != 0) ? 1 : 0;
    mcudpUpdate(DPID_POWER, DP_TYPE_BOOL, dp_power);
}

void WIFI_ReportTimer(unsigned char hours)
{
    dp_timer = hours;
    mcudpUpdate(DPID_TIMER, DP_TYPE_VALUE, dp_timer);
}

void WIFI_ReportFanSpeed(unsigned char gear)
{
    dp_fan_speed = gear;
    mcudpUpdate(DPID_FAN_SPEED, DP_TYPE_ENUM, dp_fan_speed);
}

void WIFI_ReportIndicator(unsigned char on)
{
    dp_indicator = (on != 0) ? 1 : 0;
    mcudpUpdate(DPID_INDICATOR, DP_TYPE_BOOL, dp_indicator);
}

void WIFI_ReportBrightness(unsigned char level)
{
    dp_brightness = level;
    mcudpUpdate(DPID_BRIGHTNESS, DP_TYPE_ENUM, dp_brightness);
}

void WIFI_SetPm25(unsigned int value)
{
    if (value > (unsigned int)999)
    {
        value = (unsigned int)0;
    }
    dp_pm25 = value;
}

void WIFI_SetFilterUsage(unsigned char usage)
{
    if (usage > (unsigned char)100)
    {
        usage = (unsigned char)100;
    }
    dp_filter_usage = usage;
}

void WIFI_ReportPm25(unsigned int value)
{
    WIFI_SetPm25(value);
    mcudpUpdate(DPID_PM25, DP_TYPE_VALUE, dp_pm25);
}

void WIFI_ReportFilterUsage(unsigned char usage)
{
    WIFI_SetFilterUsage(usage);
    mcudpUpdate(DPID_FILTER_USAGE, DP_TYPE_VALUE, dp_filter_usage);
}

/* 触发全量 DP 上报 (置标志, 由 alldpUpdate 分次完成) */
void WIFI_ReportAll(void)
{
    wifi_dp_flag = 1;
}

/*=============================================================================
 * 主处理入口
 *===========================================================================*/

/* WIFI_Process: 由主循环周期调用, 解析 WiFi 协议帧并响应涂鸦命令 */
void WIFI_Process(void)
{
    wifi_UART_Service();
    Settuyawifi();
}

/* WIFI_Reset: 复位 WiFi 模组进入 SmartConfig 配网模式 */
void WIFI_Reset(void)
{
    wifi_pair_mode = 0;
    UART_SendBuf(ResetWifi, 7);
}

/* WIFI_ResetAP: 复位 WiFi 模组进入 AP 配网模式 */
void WIFI_ResetAP(void)
{
    wifi_pair_mode = 1;
    UART_SendBuf(ResetWifi, 7);
}
