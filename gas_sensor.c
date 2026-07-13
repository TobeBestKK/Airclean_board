#include <cms.h>
#include "board.h"
#include "gas_sensor.h"

/*
 * TP-401W 异味传感器驱动实现
 *
 * =======================================================================
 * 【崩溃根因修复保留】
 * ADC 采样与 TMR2 触摸扫描 ISR 的并发冲突修复 (PEIE 原子保护 + 超时):
 *   所有 ADC 操作在关 PEIE 窗口内完成, 防止 ISR 改写 ADCON0 导致死循环.
 * =======================================================================
 * 【算法适配自 air1.6.1 已验证方案】
 *   1. ADC 左对齐读取 (ADFM=0): (ADRESH<<4)|(ADRESL>>4) → 0~4092
 *   2. 6 次采样去最小/最大后 4 次平均
 *   3. 一阶 IIR 低通滤波 (α=64)
 *   4. 开短路检测: 连续 8 次确认防误判
 *   5. 非线性分段映射: MapToDisplay
 * =======================================================================
 */

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

/*=============================================================================
 * 内部: 原子性写 CHS + 置 GO 启动转换 (调用者必须已关 PEIE)
 *============================================================================*/
static void Gas_StartConvert_Locked(unsigned char chan)
{
    ADCON0 = (unsigned char)((ADCON0 & (unsigned char)0xC3)
                 | (unsigned char)((unsigned char)(chan & 0x0F) << 2));
    ADCON0 = (unsigned char)(ADCON0 | ADC_GO_MASK);
}

/*=============================================================================
 * 内部: 等待 GO/DONE 硬件清零, 带超时 (调用者必须已关 PEIE)
 * 返回: 0 = 成功, 非 0 = 超时
 *============================================================================*/
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

/*=============================================================================
 * Gas_Init
 * ADC 初始化: Fosc/8, 左对齐 (ADFM=0), Vref=Vdd, 开 AN13
 *============================================================================*/
void Gas_Init(void)
{
    unsigned char old_peie;
    unsigned char rc;

    old_peie = (unsigned char)PEIE;
    PEIE = 0;

    /* ADCON0: 时钟=Fosc/8 (01), 通道暂 AN0, GO=0, ADON=1
     * ADCON1: 左对齐 (ADFM=0), Vref=Vdd */
    ADCON0 = (unsigned char)0x41;       /* 0100_0001 */
    ADCON1 = (unsigned char)0x00;       /* 0000_0000: ADFM=0 */

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

/*=============================================================================
 * 内部: 单次 ADC 读取 (左对齐, 返回 0~4092)
 * 调用者必须已关 PEIE
 * 返回: 0~4092 (正常) 或 0xFFFF (超时)
 *============================================================================*/
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

    /* 左对齐: ADRESH[7:0] = ADC[9:2], ADRESL[7:6] = ADC[1:0]
       组合: (ADRESH << 4) | (ADRESL >> 4) → 等效 ADC × 4 = 0~4092
       必须先读 ADRESH 再读 ADRESL, 以锁定 ADC 结果寄存器对 */
    adres_h = (unsigned char)ADRESH;
    adres_l = (unsigned char)ADRESL;
    ad_val  = ((unsigned int)adres_h << 4) | ((unsigned int)adres_l >> 4);

    return ad_val;
}

/*=============================================================================
 * 内部: 非线性映射 (移植自 air1.6.1 已验证方案)
 *
 * 输入 x = filtered_conc / 20, 范围 0~49 (0~999 / 20)
 *   x ≤ 30 段: 线性映射 (y = x*5/3 - 30), x<18 时返回 0
 *   x > 30 段: 二次曲线拟合 (适应 TP-401W 高浓度非线性)
 * 输出: 0~500
 *============================================================================*/
static unsigned int MapToDisplay(unsigned int x)
{
    unsigned int y;

    if (x <= (unsigned int)30)
    {
        if (x < (unsigned int)18)
            return (unsigned int)0;
        y = x * 5U / 3U;
        if (y < 30U) y = 0U;
        else y = y - 30U;
        if (y > 500U) y = 500U;
        return y;
    }
    else
    {
        unsigned int d = x - (unsigned int)30;
        y = (d * d * 155U) / 125U;
        y = y + (d * 5U / 3U);
        y = y + 22U;
        if (y > 500U) y = 500U;
        return y;
    }
}

/*=============================================================================
 * 内部状态变量 (跨调用保持)
 *============================================================================*/
static unsigned char  open_delay   = 0;  /* 开路防抖计数 */
static unsigned char  short_delay  = 0;  /* 短路防抖计数 */
static unsigned int   filtered_val = 0;  /* 一阶低通滤波输出 (raw_conc ×10) */

/*=============================================================================
 * Gas_ReadPm25 (主接口)
 *
 * 流程:
 *   1. 检查传感器供电
 *   2. PEIE 保护下: 6 次采样, 去最小/最大, 4 次平均
 *   3. 开短路防抖判定
 *   4. 电压换算 (mV) + 原始浓度 (×10)
 *   5. 一阶 IIR 低通滤波
 *   6. 非线性映射 → 返回 0~500
 *
 * 返回: 0~500 (有效值) 或 GAS_PM25_INVALID
 *============================================================================*/
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
    unsigned int  raw_display;

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
            /* 单次超时: 跳过此帧 */
            continue;
        }
        if (ad_val > (unsigned int)4092)
        {
            /* 超范围保护 */
            continue;
        }

        /* 更新最小/最大/累加 */
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

    /* 有效采样不足 4 次 (至少需要 4+2=6 次, 去极值后剩 4) */
    if (valid_cnt < GAS_ADC_SAMPLE_CNT)
        return GAS_PM25_INVALID;

    /* 去最小/最大, 剩余 4 次平均 */
    ad_sum = ad_sum - ad_max - ad_min;
    ad_val = ad_sum / 4U;   /* 0~4092 */

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
            raw_display = filtered_val / 20U;
            return MapToDisplay(raw_display);
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
            raw_display = filtered_val / 20U;
            return MapToDisplay(raw_display);
        }
        return GAS_PM25_INVALID;
    }
    else
    {
        /* 正常范围: 清防抖计数 */
        open_delay  = (unsigned char)0;
        short_delay = (unsigned char)0;
    }

    /* 4) 原始浓度 (×10): 公式同 air1.6.1
       raw_conc = ad_val * 5000 / 4096 * 5 / 10, 再饱和到 999 */
    raw_conc = (unsigned int)(
        (unsigned long)ad_val * 5000UL / 4096UL * 5UL / 10UL
    );
    if (raw_conc > (unsigned int)999)
        raw_conc = (unsigned int)999;

    /* 5) 一阶低通 IIR 滤波
       filtered_val = (α × raw_conc + (256-α) × filtered_val) >> 8 */
    if (filtered_val == (unsigned int)0)
    {
        /* 首次有效值: 直接赋值, 跳过滤波 */
        filtered_val = raw_conc;
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

    /* 6) 非线性映射
       raw_display = filtered_val / 20 → 0~49
       MapToDisplay(raw_display) → 0~500, 再 ×4 放大 (用户要求扩大 4 倍) */
    raw_display = filtered_val / 20U;
    {
        unsigned int _display = MapToDisplay(raw_display);
        _display = _display * 3U;
        if (_display > 500U) _display = 500U;
        return _display;
    }
}

/*=============================================================================
 * 预热状态
 *============================================================================*/
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

/*=============================================================================
 * Gas_PowerOn / Gas_PowerOff
 *============================================================================*/
void Gas_PowerOn(void)
{
    GAS_VCC = 0;
}

void Gas_PowerOff(void)
{
    GAS_VCC = 1;
    warmup_counter = (unsigned char)0;  /* 关机复位预热状态，防止 4e 段在关机时继续写显示 */
}
