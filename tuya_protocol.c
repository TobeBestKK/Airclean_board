#include "tuya_protocol.h"

/*=============================================================================
 * 涂鸦串口通讯协议 (MCU 端)
 *
 * 参考文档: protocol_xidx1m5hf2birubg_20260711.pdf
 * 示例代码: (示例代码)TuyaV3_Demo/tuya.c + uart.c
 *
 * 适配芯片: CMS79F723 (EUSART on RC0/TX, RC1/RX, 9600bps)
 *
 * 帧结构:
 *   [55 AA] [版本 03] [命令] [数据长度H] [数据长度L] [数据...] [校验和]
 *
 * 校验和 = (帧头 + 版本 + 命令 + 数据长度H + 数据长度L + 数据各字节) % 256
 *
 * 注意事项:
 *   1. 本文件中的 ProductkeyMcuv[] 包含占位 PID (YOUR_PID_HERE),
 *      必须替换为用户在涂鸦 IoT 平台创建的实际产品 ID.
 *   2. DP ID 定义见 tuya_protocol.h, 必须与平台一致.
 *   3. 语音模块 (SU-03T) 与 WiFi 模块共享同一 UART, 通过字节值区分:
 *      - 0x55 开头 → WiFi 协议帧
 *      - 0xA0~0xA6 → 语音命令, 存入语音队列供 main.c 读取
 *=============================================================================*/

/*=============================================================================
 * 常量数据: 涂鸦协议预置帧
 *=============================================================================*/

/*
 * 产品 ID / MCU 版本信息
 * JSON: {"p":"xidx1m5hf2birubg","v":"1.0.0","m":0}
 */
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
 *=============================================================================*/
#define UART_BUF_SIZE  64

typedef struct {
    unsigned char head;
    unsigned char tail;
    unsigned char buf[UART_BUF_SIZE];
} CircleBuffer;

static CircleBuffer uart_rx_buf;

/* 压入一字节 (由 ISR 调用) */
static void UART_BufPush(unsigned char dat)
{
    uart_rx_buf.buf[uart_rx_buf.tail] = dat;
    if (++uart_rx_buf.tail >= UART_BUF_SIZE)
        uart_rx_buf.tail = 0;
    /* 缓冲区满时覆盖最早的数据 */
    if (uart_rx_buf.tail == uart_rx_buf.head)
    {
        if (++uart_rx_buf.head >= UART_BUF_SIZE)
            uart_rx_buf.head = 0;
    }
}

/* 弹出一字节 (由主循环调用), 返回 1=成功, 0=缓冲区空 */
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
 * WiFi 协议解析器在状态 0 遇到 0xA0~0xA6 的字节时,
 * 不丢弃而是存入此队列供 main.c 的语音处理逻辑消费.
 *=============================================================================*/
#define VOICE_CMD_QUEUE_SIZE  8

static unsigned char voice_cmd_queue[VOICE_CMD_QUEUE_SIZE];
static unsigned char voice_cmd_head = 0;
static unsigned char voice_cmd_tail = 0;

static void VoiceCmd_Push(unsigned char cmd)
{
    voice_cmd_queue[voice_cmd_tail] = cmd;
    if (++voice_cmd_tail >= VOICE_CMD_QUEUE_SIZE)
        voice_cmd_tail = 0;
    /* 队列满时丢弃最旧命令 (正常情况下不应发生) */
    if (voice_cmd_tail == voice_cmd_head)
    {
        if (++voice_cmd_head >= VOICE_CMD_QUEUE_SIZE)
            voice_cmd_head = 0;
    }
}

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
 *=============================================================================*/

/* 发送一字节 */
static void UART_SendByte(unsigned char dat)
{
    TXREG = dat;
    while (TRMT == 0)
    {
        /* 等待发送移位寄存器空 */
    }
}

/* 发送指定长度的数据 (不追加 CR/LF, 与示例不同) */
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
 * UART 初始化
 *=============================================================================*/
void WIFI_Init(void)
{
    /* RC0 = TX (输出), RC1 = RX (输入) */
    TRISC0 = 0;
    TRISC1 = 1;

    /* 9600 bps @ 16MHz Fosc, 公式: Baud = Fosc / (16 * (SPBRG + 1)) */
    SPBRG = 103;        /* 16MHz / (16 * 104) ≈ 9615 bps, 误差 0.16% */

    /*
     * UART 寄存器配置 (直接写寄存器, 避免位名兼容性问题)
     * TXSTA: TXEN=1(bit5), SYNC=0(bit4), BRGH=0(bit2) → 0x20
     * RCSTA: SPEN=1(bit7), CREN=1(bit4) → 0x90
     */
    TXSTA = 0x20;
    RCSTA = 0x90;

    /* PIE1: RCIE=1(bit5), TXIE=0(bit4) — 保留 TMR2IE(bit1) 不动 */
    PIE1 = (unsigned char)(PIE1 | 0x20);   /* RCIE = 1 */
    PIE1 = (unsigned char)(PIE1 & ~0x10);  /* TXIE = 0 */

    /* 清空环形缓冲区 */
    uart_rx_buf.head = 0;
    uart_rx_buf.tail = 0;

    /* 清空语音命令队列 */
    voice_cmd_head = 0;
    voice_cmd_tail = 0;
}

/*=============================================================================
 * 中断服务: 接收字节压入环形缓冲区
 * 由 main.c 的 ISR 在检测到 RCIF 时调用
 *=============================================================================*/
void WIFI_ISR_Rx(void)
{
    /*
     * 接收字节并压入环形缓冲区.
     * 错误处理:
     *   1. FERR: 帧错误, 读取并丢弃当前字节
     *   2. OERR: 溢出错误, 重置接收器 (必须处理, 否则后续无法接收)
     */
    if (FERR)
    {
        (void)RCREG;    /* 读取丢弃, 清 RCIF */
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
 *=============================================================================*/

/* 前向声明 */
static void dpInfHandle(unsigned char dpId, unsigned long dpData);
static void alldpUpdate(void);

/* 协议解析状态 */
static unsigned char wifi_buf_state = 0;
static unsigned char wifi_buf_idx = 0;
static unsigned int  wifi_checksum = 0;
static unsigned char wifi_dat_count = 0;
static unsigned char wifi_rx_buf[20];  /* 接收帧缓冲区, 最大支持 14 字节数据 */

/* WiFi 模组下发的命令字 */
volatile unsigned char wifi_cmd = 0;
/* WiFi 连接状态 (由模组上报) */
volatile unsigned char wifi_state = 5;
/* DP 上报请求标志 */
volatile unsigned char wifi_dp_flag = 0;

/*
 * wifi_UART_Service
 * 从环形缓冲区逐字节读取并解析 WiFi 协议帧.
 * 遇到 0xA0~0xA6 范围的语音命令字节时自动分流到语音队列.
 */
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
            else if (dat >= VOICE_CMD_WAKEUP && dat <= VOICE_CMD_FILTER)
            {
                /* 语音命令: 分流到语音队列 */
                VoiceCmd_Push(dat);
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
                /* 重新开始 */
                wifi_rx_buf[0] = 0x55;
                wifi_checksum = dat;
                wifi_buf_idx = 1;
            }
            else
            {
                wifi_buf_state = 0;
                wifi_buf_idx = 0;
            }
            break;

        case 2:  /* 检查版本号 (应为 0x03) */
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
                /* 非法命令, 丢弃整帧 */
                wifi_buf_state = 0;
                wifi_buf_idx = 0;
            }
            break;

        case 4:  /* 数据长度 (高位 + 低位) */
            if (wifi_buf_idx == 5)
            {
                /* 数据长度低字节 */
                if (dat == 0x00 || dat == 0x01)
                {
                    wifi_buf_state = 6;  /* 0x03 类命令: 仅 1 字节状态 */
                }
                else
                {
                    wifi_buf_state = 5;  /* 0x06/0x07 命令: 含 DP 数据 */
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
                /* 数据接收完毕, 校验 */
                wifi_checksum = (unsigned char)(wifi_checksum % 256);

                if (wifi_checksum == dat)
                {
                    /*
                     * 校验通过: 处理 DP 下发的数据
                     * 帧结构:
                     *   [0]=0x55 [1]=0xAA [2]=ver [3]=cmd [4]=lenH [5]=lenL
                     *   [6]=dpid [7]=dptype [8]=dpLenH [9]=dpLenL [10..]=dpData
                     */
                    if (wifi_rx_buf[6] != 0xFF)  /* 非广播 DP */
                    {
                        /* 根据数据类型长度组装 dp_data */
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

                        /* 调用 DP 下发处理函数 (用户需实现) */
                        dpInfHandle(wifi_rx_buf[6], dp_data);
                    }

                    /*
                     * 回复 DP 下发应答:
                     *   版本 0x00→0x03 (+3), 命令 0x06→0x07 (+1), 校验和总共 +4
                     */
                    wifi_rx_buf[2] = 0x03;  /* MCU 版本号 */
                    wifi_rx_buf[3] = 0x07;  /* 应答命令 */
                    wifi_rx_buf[wifi_buf_idx] = (unsigned char)(dat + 4);
                    UART_SendBuf(wifi_rx_buf,
                                 (unsigned char)(7 + wifi_rx_buf[5]));
                }

                /* 复位状态机 */
                wifi_checksum = 0;
                wifi_buf_idx = 0;
                wifi_buf_state = 0;
            }
            break;

        case 6:  /* 简短响应帧 (命令 0x00~0x05, 0x08) */
            /* 状态数据已收完 (通常只有 0~1 字节) */
            wifi_cmd = wifi_rx_buf[3];
            wifi_state = wifi_rx_buf[6];

            /* 复位状态机 */
            wifi_checksum = 0;
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
 *=============================================================================*/

/* 配网模式: 0=SmartConfig, 1=AP */
static unsigned char wifi_pair_mode = 0;

/*
 * Settuyawifi
 * 根据 WiFi 模组下发的命令字, 做出相应应答.
 * 由 WIFI_Process() 调用, 通常每 20ms 执行一次.
 */
static void Settuyawifi(void)
{
    switch (wifi_cmd)
    {
    case 0x00:
        /* 模组查询心跳: 回复心跳包 */
        UART_SendBuf(HeartbeatAck, 8);
        wifi_cmd = 0xFF;
        break;

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
 *=============================================================================*/

/*
 * mcudpUpdate
 * 构建 DP 上报帧并通过 UART 发送.
 *
 * 帧结构 (MCU → WiFi):
 *   55 AA 03 07 lenH lenL dpid dptype dpLenH dpLenL [dpData] checksum
 *
 * bool/enum: dpLen=0x01, dpData 占 1 字节, 帧总长 = 7 + 5 = 12
 * value:     dpLen=0x04, dpData 占 4 字节, 帧总长 = 7 + 8 = 15
 */
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
        tx_buf[5] = 0x05;    /* 数据长度 = 5 (dpid + type + lenH + lenL + 1字节数据) */
        tx_buf[6] = dpid;
        tx_buf[7] = dptype;
        tx_buf[8] = 0x00;    /* dp 数据长度高字节 */
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
        tx_buf[5] = 0x08;    /* 数据长度 = 8 (dpid + type + lenH + lenL + 4字节数据) */
        tx_buf[6] = dpid;
        tx_buf[7] = dptype;
        tx_buf[8] = 0x00;    /* dp 数据长度高字节 */
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
        /* 不支持的类型, 暂不处理 */
        return;
    }

    UART_SendBuf(tx_buf, tx_len);
}

/*=============================================================================
 * 全部 DP 分次上报 (避免一次发送过多数据阻塞 UART)
 *=============================================================================*/

/*
 * DP 状态变量 (由 dpInfHandle / 本地逻辑更新)
 */
volatile unsigned char wifi_dp_changed = 0;  /* 云端下发变更标志 */
volatile unsigned char dp_power       = 0;  /* DP 101: 开关 */

/*
 * alldpUpdate
 * 上报全部 DP (当前仅 1 个 DP, 后续添加 DP 后改为分次上报)
 */
static void alldpUpdate(void)
{
    if (wifi_dp_flag == 0)
        return;

    mcudpUpdate(DPID_POWER, DP_TYPE_BOOL, dp_power);
    wifi_dp_flag = 0;
}

/*=============================================================================
 * DP 下发处理: 云 → 本地
 *
 * !!重要!! 用户必须根据自己的功能点定义在此函数中添加/修改处理逻辑.
 * 当 WiFi 模组下发 DP 数据时, 此函数被调用, 传入 dpId 和解析后的 dpData.
 *=============================================================================*/
static void dpInfHandle(unsigned char dpId, unsigned long dpData)
{
    switch (dpId)
    {
    case DPID_POWER:
        dp_power = (unsigned char)(dpData & 0xFF);
        wifi_dp_changed = 1;
        break;

    default:
        break;
    }
}

/*=============================================================================
 * 单个 DP 上报函数 — 供外部 (main.c) 在本地状态变化时调用
 *=============================================================================*/

void WIFI_ReportPower(unsigned char on)
{
    dp_power = (on != 0) ? 1 : 0;
    mcudpUpdate(DPID_POWER, DP_TYPE_BOOL, dp_power);
}

void WIFI_ReportAll(void)
{
    wifi_dp_flag = 1;
}

/*=============================================================================
 * 主处理入口
 *=============================================================================*/

/*
 * WIFI_Process
 * 每 ~20ms 从主循环调用一次, 处理:
 *   1. 从环形缓冲区解析 WiFi 协议帧
 *   2. 响应涂鸦协议命令
 */
void WIFI_Process(void)
{
    wifi_UART_Service();
    Settuyawifi();
}

/*
 * WIFI_Reset
 * 复位 WiFi 模组, 进入 SmartConfig 配网模式
 * 用法: 按键长按等触发, APP 通过 SmartConfig 配网
 */
void WIFI_Reset(void)
{
    wifi_pair_mode = 0;  /* SmartConfig */
    UART_SendBuf(ResetWifi, 7);
}

/*
 * WIFI_ResetAP
 * 复位 WiFi 模组, 进入 AP 配网模式
 * 用法: 设备开启一个 AP 热点, 手机连接热点后通过 APP 配网
 */
void WIFI_ResetAP(void)
{
    wifi_pair_mode = 1;  /* AP 模式 */
    UART_SendBuf(ResetWifi, 7);
}

/*=============================================================================
 * DP 状态读取函数
 *=============================================================================*/

unsigned char WIFI_GetDpPower(void) { return dp_power; }
