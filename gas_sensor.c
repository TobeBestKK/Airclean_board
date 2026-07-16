#include <cms.h>
#include "board.h"
#include "gas_sensor.h"

/* H3:SMELL 外设模拟输入驱动实现。
   当前工程按 TP-401W 行为模型实现，外接模块型号和 RB1/CND 的实际功能仍需实机确认。

   ADC 采样与 TMR2 触摸扫描 ISR 的并发保护 (PEIE 原子保护 + 超时):
   所有 ADC 操作在关 PEIE 窗口内完成, 防止 ISR 改写 ADCON0 导致死循环.

   算法流程:
   1. ADC 12-bit 左对齐读取 (ADFM=0): (ADRESH<<4)|(ADRESL>>4) → 原始范围 0~4095, 当前软件有效范围按 0~4092 处理
   2. 6 次采样去最小/最大后 4 次平均
   3. 一阶 IIR 低通滤波 (α=64)
   4. 开短路检测: 连续 8 次确认防误判
   5. 分段线性映射: MapToDisplay */

/* ---------- 位掩码 ---------- */
#define ADC_GO_MASK     ((unsigned char)0x02)   /* ADCON0.1 GO/DONE */
#define ADC_ADON_MASK   ((unsigned char)0x01)   /* ADCON0.0 ADON    */

/* ---------- 超时保护 ---------- */
#define ADC_TIMEOUT_MAX   ((unsigned int)0x01FF)

/* ---------- ADON 稳定延时 ---------- */
#define ADC_TAD_STABLE()  do { asm("nop"); asm("nop"); asm("nop"); asm("nop"); \
                               asm("nop"); asm("nop"); asm("nop"); asm("nop"); } while (0)

/* ---------- ADC 通道: AN13 (RB2) ---------- */
#define GAS_SENSOR_ADCH     ((unsigned char)13)

/* ---------- 开短路防抖计数 (连续次数) ---------- */
#define FAULT_DEBOUNCE_CNT  ((unsigned char)8)

/* 内部: 原子性写 CHS + 置 GO 启动转换 (调用者必须已关 PEIE) */
static void Gas_StartConvert_Locked(unsigned char chan)
{
    ADCON0 = (unsigned char)((ADCON0 & (unsigned char)0xC3)
                 | (unsigned char)((unsigned char)(chan & 0x0F) << 2));
    ADCON0 = (unsigned char)(ADCON0 | ADC_GO_MASK);
}

/* 内部: 等待 GO/DONE 硬件清零, 带超时 (调用者必须已关 PEIE)。
   返回 0=成功, 非 0=超时 */
static unsigned char Gas_WaitDone_Timeout(void)
{
    unsigned int cnt;
    for (cnt = (unsigned int)0; cnt < ADC_TIMEOUT_MAX; cnt++)
    {
        if ((ADCON0 & ADC_GO_MASK) == (unsigned char)0x00)
            return (unsigned char)0x00;
    }
    return (unsigned char)0x01;
}

/* Gas_Init: ADC 初始化 (Fosc/8, 左对齐 ADFM=0, Vref=Vdd, 开 AN13) */
void Gas_Init(void)
{
    unsigned char old_peie;
    unsigned char rc;

    old_peie = (unsigned char)PEIE;
    PEIE = 0;

    /* ADCON0: 时钟=Fosc/8 (01), 通道暂 AN0, GO=0, ADON=1
       ADCON1: 左对齐 (ADFM=0), Vref=Vdd */
    ADCON0 = (unsigned char)0x41;
    ADCON1 = (unsigned char)0x00;

    ADC_TAD_STABLE();

    /* 预热丢弃首次转换 */
    Gas_StartConvert_Locked(GAS_SENSOR_ADCH);
    rc = Gas_WaitDone_Timeout();
    if (rc == (unsigned char)0x00)
    {
        (void)ADRESH;
        (void)ADRESL;
    }
    else
    {
        ADCON0 = (unsigned char)(ADCON0 & (unsigned char)(~ADC_GO_MASK));
    }

    PEIE = (old_peie != (unsigned char)0) ? (unsigned char)1 : (unsigned char)0;
}

/* 内部: 单次 ADC 读取 (左对齐, 原始返回 0~4095), 调用者必须已关 PEIE。
   返回 0~4095 (正常) 或 0xFFFF (超时); 上层按当前工程有效范围筛选 */
static unsigned int Gas_ReadOnce_Locked(void)
{
    unsigned char adres_h;
    unsigned char adres_l;
    unsigned int  ad_val;

    Gas_StartConvert_Locked(GAS_SENSOR_ADCH);
    if (Gas_WaitDone_Timeout() != (unsigned char)0x00)
    {
        ADCON0 = (unsigned char)(ADCON0 & (unsigned char)(~ADC_GO_MASK));
        return (unsigned int)0xFFFF;
    }

    /* ADFM=0 的 12-bit 左对齐结果:
       ADRESH[7:0]=ADC[11:4], ADRESL[7:4]=ADC[3:0]
       组合 (ADRESH<<4)|(ADRESL>>4) → 0~4095; 必须先读 ADRESH 再读 ADRESL 以锁定结果寄存器对 */
    adres_h = (unsigned char)ADRESH;
    adres_l = (unsigned char)ADRESL;
    ad_val  = ((unsigned int)adres_h << 4) | ((unsigned int)adres_l >> 4);

    return ad_val;
}

/* 内部: 线性标定映射。
   输入滤波后原始浓度值, 低于 ZERO 显示 0, 达到 FULL 显示 500, 中间按比例映射。
   直接使用原始值, 避免 /20 量化、二次曲线和额外放大导致的跳变与过早封顶。 */
static unsigned int MapToDisplay(unsigned int value)
{
    unsigned int span;

    if (value <= GAS_PM25_DISPLAY_ZERO)
        return (unsigned int)0;
    if (value >= GAS_PM25_DISPLAY_FULL)
        return (unsigned int)500;

    span = GAS_PM25_DISPLAY_FULL - GAS_PM25_DISPLAY_ZERO;
    return (unsigned int)(
        ((unsigned long)(value - GAS_PM25_DISPLAY_ZERO) * 500UL
         + (unsigned long)span / 2UL) / (unsigned long)span
    );
}

/* 内部状态变量 (跨调用保持) */
static unsigned char  open_delay   = 0;  /* 开路防抖计数 */
static unsigned char  short_delay  = 0;  /* 短路防抖计数 */
static unsigned int   filtered_val = 0;  /* 一阶低通滤波输出 (raw_conc ×10) */

/* Gas_ReadPm25 (主接口)。
   流程: 1.检查供电 2.PEIE 保护下 6 次采样去极值平均
         3.开短路防抖判定 4.电压换算(mV)+原始浓度(×10)
         5.一阶 IIR 低通 6.分段线性映射 → 0~500
   返回 0~500 (有效值) 或 GAS_PM25_INVALID */
unsigned int Gas_ReadPm25(void)
{
    unsigned char old_peie;
    unsigned char i;
    unsigned char valid_cnt;
    unsigned int  ad_val;
    unsigned int  ad_max;
    unsigned int  ad_min;
    unsigned int  ad_sum;

    unsigned int  raw_conc;

    /* 1) 传感器未上电 */
    if (GAS_VCC != (unsigned char)0x00)
        return GAS_PM25_INVALID;

    /* ========== PEIE 保护: 开始原子 ADC ========== */
    old_peie = (unsigned char)PEIE;
    PEIE = 0;

    ad_max  = (unsigned int)0;
    ad_min  = (unsigned int)0xFFFF;
    ad_sum  = (unsigned int)0;
    valid_cnt = (unsigned char)0;

    for (i = (unsigned char)0; i < GAS_ADC_SAMPLE_CNT; i++)
    {
        ad_val = Gas_ReadOnce_Locked();
        if (ad_val == (unsigned int)0xFFFF)
        {
            continue;              /* 单次超时: 跳过此帧 */
        }
        if (ad_val > (unsigned int)4092)
        {
            continue;              /* 超范围保护 */
        }

        if (valid_cnt == (unsigned char)0)
        {
            ad_max = ad_val;
            ad_min = ad_val;
        }
        else
        {
            if (ad_val > ad_max) ad_max = ad_val;
            if (ad_val < ad_min) ad_min = ad_val;
        }
        ad_sum = (unsigned int)(ad_sum + ad_val);
        valid_cnt++;
    }

    PEIE = (old_peie != (unsigned char)0) ? (unsigned char)1 : (unsigned char)0;
    /* ========== PEIE 保护: 结束 ========== */

    /* 有效采样不足 6 次 (去极值后不足 4 次) */
    if (valid_cnt < GAS_ADC_SAMPLE_CNT)
        return GAS_PM25_INVALID;

    /* 去最小/最大, 剩余 4 次平均 */
    ad_sum = ad_sum - ad_max - ad_min;
    ad_val = ad_sum / 4U;          /* 当前软件有效范围 0~4092 */

    /* 3) 开短路防抖判定 (连续 8 次确认) */
    if (ad_val <= GAS_ADC_OPEN)
    {
        if (++open_delay >= FAULT_DEBOUNCE_CNT)
        {
            open_delay   = (unsigned char)0;
            short_delay  = (unsigned char)0;
            filtered_val = (unsigned int)0;
            return GAS_PM25_INVALID;
        }
        /* 防抖未满: 返回上次有效值 (若 filtered_val 有效) */
        if (filtered_val > (unsigned int)0)
        {
            return MapToDisplay(filtered_val);
        }
        return GAS_PM25_INVALID;
    }
    else if (ad_val >= GAS_ADC_SHORT)
    {
        if (++short_delay >= FAULT_DEBOUNCE_CNT)
        {
            open_delay   = (unsigned char)0;
            short_delay  = (unsigned char)0;
            filtered_val = (unsigned int)0;
            return GAS_PM25_INVALID;
        }
        if (filtered_val > (unsigned int)0)
        {
            return MapToDisplay(filtered_val);
        }
        return GAS_PM25_INVALID;
    }
    else
    {
        open_delay  = (unsigned char)0;   /* 正常范围: 清防抖计数 */
        short_delay = (unsigned char)0;
    }

    /* 4) 原始浓度 (×10): raw_conc = ad_val*5000/4096*5/10, 饱和到 999 */
    raw_conc = (unsigned int)(
        (unsigned long)ad_val * 5000UL / 4096UL * 5UL / 10UL
    );
    if (raw_conc > (unsigned int)999)
        raw_conc = (unsigned int)999;

    /* 5) 一阶低通 IIR: filtered_val = (α×raw + (256-α)×filtered) >> 8 */
    if (filtered_val == (unsigned int)0)
    {
        filtered_val = raw_conc;          /* 首次有效值: 直接赋值, 跳过滤波 */
    }
    else
    {
        unsigned long _tmp;
        _tmp  = (unsigned long)GAS_FILTER_ALPHA * (unsigned long)raw_conc;
        _tmp += (256UL - (unsigned long)GAS_FILTER_ALPHA) * (unsigned long)filtered_val;
        filtered_val = (unsigned int)(_tmp >> 8);
    }
    if (filtered_val > (unsigned int)999)
        filtered_val = (unsigned int)999;

    /* 6) 线性标定映射到 0~500 */
    return MapToDisplay(filtered_val);
}

/* ---------- 预热状态 ---------- */
static unsigned char warmup_counter = (unsigned char)0;

void Gas_StartWarmup(void)
{
    warmup_counter = GAS_WARMUP_SECONDS;
}

unsigned char Gas_IsWarmupDone(void)
{
    return (warmup_counter == (unsigned char)0) ? (unsigned char)1 : (unsigned char)0;
}

unsigned char Gas_GetWarmupRemaining(void)
{
    return warmup_counter;
}

void Gas_TickWarmupSecond(void)
{
    if (warmup_counter > (unsigned char)0)
        warmup_counter--;
}

/* ---------- H3 外设控制 (当前固件按 GAS_VCC=0 开启, =1 关闭) ---------- */
void Gas_PowerOn(void)
{
    GAS_VCC = 0;
}

void Gas_PowerOff(void)
{
    GAS_VCC = 1;
    warmup_counter = (unsigned char)0;  /* 关机复位预热, 防 4e 段在关机时继续写显示 */
}
