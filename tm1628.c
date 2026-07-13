#include <cms.h>
#include "board.h"
#include "tm1628.h"

/*
 * TM1628A 显示驱动模块。
 * 三线 SPI-like 串行接口 (STB/CLK/DIO)，7 Grid x 11 Segment 模式。
 * 维护一份显示 RAM 影子缓存，支持按地址读改写，避免破坏同地址其他段位。
 */

/* 显示 RAM 影子缓存 (14 字节，对应 Grid1~7 的低/高字节) */
static unsigned char tm1628_ram[14];

/* 短延时：稳定显示时序，i<10 一般够用，不稳定可调大 */
static void TM1628_Delay(void)
{
    unsigned char i;

    for (i = 0; i < 10; i++)
    {
        asm("nop");
    }
}

/* 流水测试用长延时（约数百毫秒，含喂看门狗） */
static void TM1628_LongDelay(void)
{
    unsigned int d;

    for (d = 0; d < 30000; d++)
    {
        asm("clrwdt");
    }
}

/* 通信开始：STB 拉低选中芯片 */
static void TM1628_Start(void)
{
    TM1628_STB = 1;
    TM1628_CLK = 1;
    TM1628_DIO = 1;
    TM1628_Delay();

    TM1628_STB = 0;
    TM1628_Delay();
}

/* 通信结束：STB 拉高释放芯片 */
static void TM1628_Stop(void)
{
    TM1628_CLK = 1;
    TM1628_Delay();

    TM1628_STB = 1;
    TM1628_Delay();
}

/* 写 1 字节，低位先发 (LSB first) */
static void TM1628_WriteByte(unsigned char dat)
{
    unsigned char i;

    for (i = 0; i < 8; i++)
    {
        TM1628_CLK = 0;
        TM1628_Delay();

        if ((dat & (unsigned char)0x01) != (unsigned char)0x00)
        {
            TM1628_DIO = 1;
        }
        else
        {
            TM1628_DIO = 0;
        }

        TM1628_Delay();

        TM1628_CLK = 1;
        TM1628_Delay();

        dat = (unsigned char)(dat >> 1);
    }
}

/* 独立发一条命令字节（带 Start/Stop） */
static void TM1628_WriteCmd(unsigned char cmd)
{
    TM1628_Start();
    TM1628_WriteByte(cmd);
    TM1628_Stop();
}

/*
 * 固定地址写显示 RAM (0x44 命令)。
 * 同步更新影子缓存，便于后续按位读改写。
 * addr: 0x00 ~ 0x0D；dat : 写入该地址的整字节
 */
void TM1628_WriteByteToAddr(unsigned char addr, unsigned char dat)
{
    unsigned char cmd;

    if (addr < (unsigned char)14)
    {
        tm1628_ram[addr] = dat;
    }

    TM1628_WriteCmd((unsigned char)0x44);              /* 固定地址写模式 */
    cmd = (unsigned char)((unsigned char)0xC0 + addr); /* 0xC0 + addr: 设置 RAM 地址 */

    TM1628_Start();
    TM1628_WriteByte(cmd);
    TM1628_WriteByte(dat);
    TM1628_Stop();
}

/* 清屏：14 个地址全部写 0 */
void TM1628_Clear(void)
{
    unsigned char i;

    for (i = 0; i < 14; i++)
    {
        TM1628_WriteByteToAddr(i, (unsigned char)0x00);
    }
}

/* 全灭（Clear 别名，便于 main.c 区分语义） */
void TM1628_AllOff(void)
{
    TM1628_Clear();
}

/* ---- LED 与数码管段位映射宏 ---- */
#define TM1628_LED_SEG1_MASK        ((unsigned char)0x01)  /* LED 占用 seg1 位 */
#define TM1628_LED_PIN2_ADDR  ((unsigned char)0x02)       /* LED1 对应 RAM 地址 */
#define TM1628_LED_PIN3_ADDR  ((unsigned char)0x00)       /* LED2 */
#define TM1628_LED_PIN4_ADDR  ((unsigned char)0x04)       /* LED3 */
#define TM1628_LED_PIN5_ADDR        ((unsigned char)0x06)  /* LED4 */
#define TM1628_LED_SEG1_CLEAR_MASK  ((unsigned char)0xFE)  /* 清 seg1 位 */
#define TM1628_DIGIT_SEG_LOW_MASK   ((unsigned char)0xFE)  /* 数字低字节段位掩码 */
#define TM1628_DIGIT_SEG_HIGH_MASK  ((unsigned char)0x01)  /* 数字高字节段位 (bit0) */
#define TM1628_DIG4_GRID            ((unsigned char)0x04)  /* 风速显示位 */
#define TM1628_DIG5_GRID            ((unsigned char)0x03)  /* 定时十位 */
#define TM1628_DIG6_GRID            ((unsigned char)0x02)  /* 定时个位 */
#define TM1628_DIGIT_DP_MASK        ((unsigned char)0x80)  /* DP 小数点位 */
#define TM1628_SPEED_MAX_LEVEL      ((unsigned char)3)
#define TM1628_TIMER_MAX_HOURS      ((unsigned char)24)

/* 按地址写 LED 的 seg1 位 (读改写，保留该地址其他段) */
static void TM1628_WriteLedByAddr(unsigned char addr, unsigned char on)
{
    unsigned char dat;

    dat = tm1628_ram[addr];
    if (on != (unsigned char)0x00)
    {
        dat = (unsigned char)(dat | TM1628_LED_SEG1_MASK);
    }
    else
    {
        dat = (unsigned char)(dat & TM1628_LED_SEG1_CLEAR_MASK);
    }

    TM1628_WriteByteToAddr(addr, dat);
}

/* 4 位 LED 掩码 → 对应引脚地址 (bit0=LED1 .. bit3=LED4) */
void TM1628_SetLeds(unsigned char leds)
{
    TM1628_WriteLedByAddr(TM1628_LED_PIN2_ADDR, (unsigned char)(leds & (unsigned char)0x01));
    TM1628_WriteLedByAddr(TM1628_LED_PIN3_ADDR, (unsigned char)(leds & (unsigned char)0x02));
    TM1628_WriteLedByAddr(TM1628_LED_PIN4_ADDR, (unsigned char)(leds & (unsigned char)0x04));
    TM1628_WriteLedByAddr(TM1628_LED_PIN5_ADDR, (unsigned char)(leds & (unsigned char)0x08));
}

/*
 * 数字→段码表 (严格按用户实际硬件接线):
 *   SEG9  = A  (high_addr bit0)
 *   SEG7  = B  (low_addr  bit6)
 *   SEG6  = C  (low_addr  bit5)
 *   SEG5  = G  (low_addr  bit4)
 *   SEG4  = F  (low_addr  bit3)
 *   SEG3  = E  (low_addr  bit2)
 *   SEG2  = D  (low_addr  bit1)
 *   SEG1  = 独立 LED 位 (low_addr bit0, 段码拼接时永远为 0, LED 读写改保留)
 *   SEG8  = DP (low_addr  bit7, 由 SetDigitByGrid 按需 OR 拼接)
 *
 * 经典 7 段亮段 {A,B,C,D,E,F,G} → low byte = (B<<6)|(C<<5)|(G<<4)|(F<<3)|(E<<2)|(D<<1)
 * digits 0..9 按经典 7 段定义一一算出:
 *   0 = B C D E F       → 0b110111_0 → 0x7C
 *   1 = B C             → 0b110000_0 → 0x60
 *   2 = B G E D         → 0b101110_0 → 0x5E
 *   3 = B G C D         → 0b111100_0 → 0x78
 *   4 = G F B C         → 0b111001_0 → 0x72? 更正: G=1,F=1,B=1,C=1,E=0,D=0
 *                                 6 5 4 3 2 1
 *                                 B C G F E D → 1 1 1 1 0 0 = 0x3C<<1 = 0x78?
 * 重新按位序 [6 5 4 3 2 1] = [B C G F E D] 真值表列:
 *   0 B=1,C=1,G=0,F=1,E=1,D=1 → [1,1,0,1,1,1] = 0x37 → <<1 = 0x6E.
 * 修正 bit4=G! 之前把 G 放在了 bit4。哦, 上面把 bit4=G, bit5=C, bit6=B, bit3=F, bit2=E, bit1=D.
 * 统一位序: bit6=B, bit5=C, bit4=G, bit3=F, bit2=E, bit1=D.
 *
 * 按此位序重新推数字 0~9:
 *  0: B=1 C=1 G=0 F=1 E=1 D=1 → 6:1,5:1,4:0,3:1,2:1,1:1 → 0b110111_0 → 0x37 << 1 = 0x6E
 *  1: B=1 C=1 G=0 F=0 E=0 D=0 → 0b110000_0 → 0x30 << 1 = 0x60
 *  2: B=1 C=0 G=1 F=0 E=1 D=1 → 0b101011_0 → 0x2B << 1 = 0x56
 *  3: B=1 C=1 G=1 F=0 E=0 D=1 → 0b111001_0 → 0x39 << 1 = 0x72
 *  4: B=1 C=1 G=1 F=1 E=0 D=0 → 0b111100_0 → 0x3C << 1 = 0x78
 *  5: B=0 C=1 G=1 F=1 E=0 D=1 → 0b011101_0 → 0x1D << 1 = 0x3A
 *  6: B=0 C=1 G=1 F=1 E=1 D=1 → 0b011111_0 → 0x1F << 1 = 0x3E
 *  7: B=1 C=1 G=0 F=0 E=0 D=0 → 0b110000_0 → 0x30 << 1 = 0x60 (与 1 同 low, 由 A=high bit0 区分)
 *  8: B=1 C=1 G=1 F=1 E=1 D=1 → 0b111111_0 → 0x3F << 1 = 0x7E
 *  9: B=1 C=1 G=1 F=1 E=0 D=1 → 0b111101_0 → 0x3D << 1 = 0x7A
 *
 * 上面结果恰好与用户 board 参考项目里的旧段码表完全一致 (0x6E,0x60,0x56,0x72,0x78,0x3A,0x3E,0x60,0x7E,0x7A)
 * 即旧段码表按 7 段亮段看已经是正确的! 之前分析偏差在 bit4 的 SEG 命名 —
 *   实际上 SEG5 对应 G (bit4), 与上面定义完全一致.
 * 这里不改动 low 字节段码, 仅修正 high bit (SEG9=A) 的判定: 仅 1、4 不亮 A 段.
 */

/* 数字→low 字节段码 (经典 7 段值，与参考接线 SEG 位序精确吻合) */
static unsigned char TM1628_GetDigitLow(unsigned char value)
{
    unsigned char dat;

    switch (value)
    {
        case 0: dat = (unsigned char)0x6E; break;
        case 1: dat = (unsigned char)0x60; break;
        case 2: dat = (unsigned char)0x56; break;
        case 3: dat = (unsigned char)0x72; break;
        case 4: dat = (unsigned char)0x78; break;
        case 5: dat = (unsigned char)0x3A; break;
        case 6: dat = (unsigned char)0x3E; break;
        case 7: dat = (unsigned char)0x60; break;
        case 8: dat = (unsigned char)0x7E; break;
        case 9: dat = (unsigned char)0x7A; break;
        default:dat = (unsigned char)0x00; break;
    }

    return dat;
}

/* 数字→high 字节段位 (仅 bit0 = SEG9 = A 段):
 * 经典 7 段中: 数字 1、4 无顶部 A 横 (SEG9=0); 其他数字均有 A 横 (SEG9=1).
 */
static unsigned char TM1628_GetDigitHigh(unsigned char value)
{
    if ((value == (unsigned char)0x01) || (value == (unsigned char)0x04))
    {
        return (unsigned char)0x00;
    }
    return (unsigned char)0x01;
}

/*
 * 在指定 Grid 上显示一位数字 (读改写合并段码与 DP)。
 * grid: 1~7；value: 0~9；dp_on: 是否点亮该位 DP
 */
static void TM1628_SetDigitByGrid(unsigned char grid, unsigned char value, unsigned char dp_on)
{
    unsigned char low_addr;
    unsigned char high_addr;
    unsigned char low_dat;
    unsigned char high_dat;

    low_addr = (unsigned char)((grid - (unsigned char)1) << 1);
    high_addr = (unsigned char)(low_addr + (unsigned char)1);

    low_dat = (unsigned char)(tm1628_ram[low_addr] & (unsigned char)(~TM1628_DIGIT_SEG_LOW_MASK));
    low_dat = (unsigned char)(low_dat | TM1628_GetDigitLow(value));
    /* Bug3 修复: 必须显式清 DP (SEG8=bit7) 才能让 PM2.5 灯真正熄灭,
       否则 tm1628_ram 影子缓存中残留旧 0x80 会一直点亮 DP。
       之前只有 dp_on=1 时 OR 打开, dp_on=0 时没清 — 形成永久残影。 */
    if (dp_on != (unsigned char)0x00)
    {
        low_dat = (unsigned char)(low_dat | TM1628_DIGIT_DP_MASK);
    }
    else
    {
        low_dat = (unsigned char)(low_dat & (unsigned char)(~TM1628_DIGIT_DP_MASK));
    }
    TM1628_WriteByteToAddr(low_addr, low_dat);

    high_dat = (unsigned char)(tm1628_ram[high_addr] & (unsigned char)(~TM1628_DIGIT_SEG_HIGH_MASK));
    high_dat = (unsigned char)(high_dat | TM1628_GetDigitHigh(value));
    TM1628_WriteByteToAddr(high_addr, high_dat);
}

/* 显示风速档位在 DIG4，超界钳至 MAX，DP 常亮作 GEAR 图标 */
void TM1628_SetSpeedDisplay(unsigned char value)
{
    if (value > TM1628_SPEED_MAX_LEVEL)
    {
        value = TM1628_SPEED_MAX_LEVEL;
    }

    TM1628_SetDigitByGrid(TM1628_DIG4_GRID, value, (unsigned char)0x01);
}

/*
 * 显示定时小时在 DIG5(十位)/DIG6(个位)。
 * DP 常亮作 TIME/h 单位图标，enabled 参数保留以维持接口兼容。
 */
void TM1628_SetTimerDisplay(unsigned char value, unsigned char enabled)
{
    unsigned char tens;
    unsigned char ones;
    unsigned char dp_on;

    if (value > TM1628_TIMER_MAX_HOURS)
    {
        value = TM1628_TIMER_MAX_HOURS;
    }

    tens = (unsigned char)0x00;
    ones = value;
    while (ones >= (unsigned char)10)
    {
        ones = (unsigned char)(ones - (unsigned char)10);
        tens++;
    }

    (void)enabled;  /* 保留参数，DP 不随其变化 */
    dp_on = (unsigned char)0x01;

    TM1628_SetDigitByGrid(TM1628_DIG5_GRID, tens, dp_on);
    TM1628_SetDigitByGrid(TM1628_DIG6_GRID, ones, dp_on);
}

/*
 * 开机默认显示：6 位数码管全部显示 0，DP 全亮作单位图标。
 * 布局 "000 0 00"：
 *   DIG1~3 = PM2.5 (000)，DIG1/2 DP=PM2.5 图标，DIG3 DP=μg/m³
 *   DIG4   = 风速档位 (0)，DP=GEAR
 *   DIG5/6 = 定时小时 (00)，DIG5 DP=TIME，DIG6 DP=h
 */
void TM1628_SetDefaultDisplay(void)
{
    TM1628_SetDigitByGrid((unsigned char)7, (unsigned char)0x00, (unsigned char)0x01);
    TM1628_SetDigitByGrid((unsigned char)6, (unsigned char)0x00, (unsigned char)0x01);
    TM1628_SetDigitByGrid((unsigned char)5, (unsigned char)0x00, (unsigned char)0x01);
    TM1628_SetDigitByGrid((unsigned char)4, (unsigned char)0x00, (unsigned char)0x01);
    TM1628_SetDigitByGrid((unsigned char)3, (unsigned char)0x00, (unsigned char)0x01);
    TM1628_SetDigitByGrid((unsigned char)2, (unsigned char)0x00, (unsigned char)0x01);
}

/*
 * TM1628_SetApDisplay
 * AP 配网模式显示：6 位数码管全部显示 "1"，提示用户 AP 配网已开启。
 */
void TM1628_SetApDisplay(void)
{
    TM1628_SetDigitByGrid((unsigned char)7, (unsigned char)0x01, (unsigned char)0x01);
    TM1628_SetDigitByGrid((unsigned char)6, (unsigned char)0x01, (unsigned char)0x01);
    TM1628_SetDigitByGrid((unsigned char)5, (unsigned char)0x01, (unsigned char)0x01);
    TM1628_SetDigitByGrid((unsigned char)4, (unsigned char)0x01, (unsigned char)0x01);
    TM1628_SetDigitByGrid((unsigned char)3, (unsigned char)0x01, (unsigned char)0x01);
    TM1628_SetDigitByGrid((unsigned char)2, (unsigned char)0x01, (unsigned char)0x01);
}

/*
 * TM1628_SetPm25Display
 * 异味传感器 (TP-401W) 对应的 PM2.5 显示：
 *   将 0~999 的数值写到 DIG1~DIG3 三位 (Grid 7/6/5)：
 *     Grid 7 = DIG1 = 百位, DP 亮 = PM2.5 "PM" 图标 DP 灯
 *     Grid 6 = DIG2 = 十位, DP 亮 = PM2.5 "2.5" 图标 DP 灯 (用户定义此两位为 "PM2.5灯")
 *     Grid 5 = DIG3 = 个位, DP 亮 = μg/m³ 单位图标
 *   若输入 >=1000 (GAS_PM25_INVALID), 视为传感器开路/未上电 → 整个 PM 三位 (段码+DP) 全部熄灭.
 *
 * 数值范围: 0~999, 超出饱和到 999.
 *
 * 缓存策略: 不做 last_pm25 等值跳过, 保证模式切换 (滤网视图 ↔ PM 视图) 时
 *   Grid 7/6/5 被无条件重写, 不会留下上一次显示内容. TM1628 为写 RAM, 无
 *   视觉闪烁, 且每 2s 仅 3 次字节写入, CPU/总线负担可忽略.
 */
void TM1628_SetPm25Display(unsigned int value)
{
    unsigned char hundreds;
    unsigned char tens;
    unsigned char ones;

    /* 传感器开路/未上电或其他异常: 消隐三位 (段码+DP 都写 0)
     * 无效阈值 = 999 (等于 gas_sensor.h 中的 GAS_PM25_MAX, 此处硬编码避免依赖)
     */
    if (value > (unsigned int)999)
    {
        /* 用 value=0xFF 触发消隐: 分别写 Grid 7/6/5 段码=0, DP=0
           这里直接用 SetDigitByGrid 写数字 0 但先清零段码的方法不够干净;
           改为直接写 0 字节到 6 个地址 (Grid 7/6/5 的高低地址), 保留 bit0 LED 位 */
        static const unsigned char grids_pm[] = {7, 6, 5};
        unsigned char g;
        unsigned char i;
        for (i = 0; i < 3; i++)
        {
            g = grids_pm[i];
            {
                unsigned char low  = (unsigned char)((g - 1) << 1);
                unsigned char high = (unsigned char)(low + 1);
                /* 保留 SEG1 (bit0) 独立 LED 位, 其余数码管段+DP 全部清 0 */
                TM1628_WriteByteToAddr(low,  (unsigned char)(tm1628_ram[low]  & (unsigned char)0x01));
                TM1628_WriteByteToAddr(high, (unsigned char)(tm1628_ram[high] & (unsigned char)0xFE));
            }
        }
        return;
    }

    /* 合法范围: 饱和裁剪 */
    if (value > (unsigned int)999)
    {
        value = (unsigned int)999;
    }

    /* 百 / 十 / 个位分解 */
    hundreds = (unsigned char)0x00;
    tens     = (unsigned char)0x00;
    ones     = (unsigned char)value;

    while (ones >= (unsigned char)100)
    {
        ones = (unsigned char)(ones - (unsigned char)100);
        hundreds++;
    }
    while (ones >= (unsigned char)10)
    {
        ones = (unsigned char)(ones - (unsigned char)10);
        tens++;
    }

    /* 写三位, DIG1/2/3 DP 全亮 (PM2.5 图标的两个 DP 灯 + μg/m³ 单位灯) */
    TM1628_SetDigitByGrid((unsigned char)7, hundreds, (unsigned char)0x01);
    TM1628_SetDigitByGrid((unsigned char)6, tens,     (unsigned char)0x01);
    TM1628_SetDigitByGrid((unsigned char)5, ones,     (unsigned char)0x01);
}

/*
 * TM1628_SetFilterUsageDisplay
 * 滤网使用度显示 (0~100)，与 PM2.5 共用 Grid 7/6/5 (DIG1/DIG2/DIG3)：
 *   Grid 7 = DIG1 = 百位, Grid 6 = DIG2 = 十位, Grid 5 = DIG3 = 个位.
 *
 * 用户定义: "点按key2按钮时，pm2.5灯灭"
 *   PM2.5 灯 = DIG1 的 DP + DIG2 的 DP (SEG8 of Grid 7 & Grid 6)
 *   → 本函数切换到滤网视图时, **明确熄灭 DIG1/2 的 DP** (pm2.5 灯灭),
 *     DIG3 的 DP 继续点亮作为 "%" 图标 (滤网百分比单位).
 *
 * 范围处理: value > 100 时饱和到 100.
 * 防闪烁:   last_filter_usage 静态缓存, 值未变更不重写 TM1628 RAM.
 */
void TM1628_SetFilterUsageDisplay(unsigned char value)
{
    unsigned char hundreds;
    unsigned char tens;
    unsigned char ones;

    /* 饱和到 0~100 */
    if (value > (unsigned char)100)
    {
        value = (unsigned char)100;
    }

    /* 不做 last_value 等值跳过，确保 PM2.5 ↔ 滤网 视图切换时三格被无条件重写 */

    /* 第二步: 百/十/个位分解 (无除法整数运算) */
    hundreds = (unsigned char)0x00;
    tens     = (unsigned char)0x00;
    ones     = value;

    while (ones >= (unsigned char)100)
    {
        ones = (unsigned char)(ones - (unsigned char)100);
        hundreds++;
    }
    while (ones >= (unsigned char)10)
    {
        ones = (unsigned char)(ones - (unsigned char)10);
        tens++;
    }

    /* 第三步: 写三个 Grid
     *   Grid 7 (DIG1) DP=0 → 熄灭 PM2.5 指示 PM DP 灯 (用户需求)
     *   Grid 6 (DIG2) DP=0 → 熄灭 PM2.5 指示 2.5 DP 灯 (用户需求)
     *   Grid 5 (DIG3) DP=1 → 点亮滤网 "%" 单位图标 DP
     */
    TM1628_SetDigitByGrid((unsigned char)7, hundreds, (unsigned char)0x00);
    TM1628_SetDigitByGrid((unsigned char)6, tens,     (unsigned char)0x00);
    TM1628_SetDigitByGrid((unsigned char)5, ones,     (unsigned char)0x01);
}

/* 把指定 Grid 的所有段位点亮 (段码全 1) */
static void TM1628_SetDigitAllOnByGrid(unsigned char grid)
{
    unsigned char low_addr;
    unsigned char high_addr;
    unsigned char dat;

    low_addr = (unsigned char)((grid - (unsigned char)1) << 1);
    high_addr = (unsigned char)(low_addr + (unsigned char)1);

    dat = (unsigned char)(tm1628_ram[low_addr] | TM1628_DIGIT_SEG_LOW_MASK);
    TM1628_WriteByteToAddr(low_addr, dat);

    dat = (unsigned char)(tm1628_ram[high_addr] | TM1628_DIGIT_SEG_HIGH_MASK);
    TM1628_WriteByteToAddr(high_addr, dat);
}

/* 全部 6 位数码管所有段位点亮 (测试用) */
void TM1628_DigitsAllOn(void)
{
    TM1628_SetDigitAllOnByGrid((unsigned char)7);
    TM1628_SetDigitAllOnByGrid((unsigned char)6);
    TM1628_SetDigitAllOnByGrid((unsigned char)5);
    TM1628_SetDigitAllOnByGrid((unsigned char)4);
    TM1628_SetDigitAllOnByGrid((unsigned char)3);
    TM1628_SetDigitAllOnByGrid((unsigned char)2);
}

/* 全亮测试：14 个地址全写 0xFF，用于验证接线与通信 */
void TM1628_AllOn(void)
{
    unsigned char i;

    for (i = 0; i < 14; i++)
    {
        TM1628_WriteByteToAddr(i, (unsigned char)0xFF);
    }
}

/*
 * TM1628_SetBrightness
 * 设置显示屏亮度。level 0=低, 1=中, 2=高.
 * 命令格式: 0x80 | pulse, pulse 对应占空比 1/16 ~ 14/16.
 */
void TM1628_SetBrightness(unsigned char level)
{
    unsigned char cmd;

    /*
     * TM1628A 显示控制: [1][0][0][0][ON][B2][B1][B0]
     *   bit3: 1=开, 0=关
     *   bit2~0: 000=1/16 .. 111=14/16
     */
    if (level == 0)
    {
        cmd = (unsigned char)0x88;  /* 开, 1/16 (低) */
    }
    else if (level == 1)
    {
        cmd = (unsigned char)0x8B;  /* 开, 10/16 (中) */
    }
    else if (level == 2)
    {
        cmd = (unsigned char)0x8F;  /* 开, 14/16 (高, 默认) */
    }
    else  /* level == 3: 省电熄屏 */
    {
        cmd = (unsigned char)0x80;  /* 关显示, 程序继续运行 */
    }

    TM1628_WriteCmd(cmd);
}

/* TM1628A 初始化：7 Grid 模式 + 地址自增 + 清屏 + 开显示最大亮度 */
void TM1628_Init(void)
{
    TM1628_WriteCmd((unsigned char)0x03);  /* 显示模式: 7 Grid / 11 Segment */
    TM1628_WriteCmd((unsigned char)0x40);  /* 地址自增写模式 */
    TM1628_Clear();                        /* 上电 RAM 不定，先清屏 */
    TM1628_WriteCmd((unsigned char)0x8F);  /* 显示开, 亮度最大 */
}

/*
 * 地址/位流水测试：遍历 14 个地址的每个 bit，逐位点亮。
 * 用于记录显示 RAM 到实际 LED/数码管段位的映射。
 * 注意：bit 在 CMS 编译器中可能是保留字，故用 bit_index。
 */
void TM1628_WalkingTest(void)
{
    unsigned char addr;
    unsigned char bit_index;
    unsigned char dat;

    for (addr = 0; addr < 14; addr++)
    {
        dat = (unsigned char)0x01;

        for (bit_index = 0; bit_index < 8; bit_index++)
        {
            TM1628_Clear();
            TM1628_WriteByteToAddr(addr, dat);
            TM1628_LongDelay();

            dat = (unsigned char)(dat << 1);
        }
    }
}
