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

/* 数字→低字节 7 段码表 (本板段位定义，非标准 7 段) */
static unsigned char TM1628_GetDigitLow(unsigned char value)
{
    unsigned char dat;

    switch (value)
    {
        case 0:
            dat = (unsigned char)0x6E;
            break;
        case 1:
            dat = (unsigned char)0x60;
            break;
        case 2:
            dat = (unsigned char)0x56;
            break;
        case 3:
            dat = (unsigned char)0x72;
            break;
        case 4:
            dat = (unsigned char)0x78;
            break;
        case 5:
            dat = (unsigned char)0x3A;
            break;
        case 6:
            dat = (unsigned char)0x3E;
            break;
        case 7:
            dat = (unsigned char)0x60;
            break;
        case 8:
            dat = (unsigned char)0x7E;
            break;
        case 9:
            dat = (unsigned char)0x7A;
            break;
        default:
            dat = (unsigned char)0x00;
            break;
    }

    return dat;
}

/* 数字→高字节段位 (仅 bit0，部分数字需要) */
static unsigned char TM1628_GetDigitHigh(unsigned char value)
{
    unsigned char dat;

    if ((value == (unsigned char)0x00)
        || (value == (unsigned char)0x02)
        || (value == (unsigned char)0x03)
        || (value == (unsigned char)0x05)
        || (value == (unsigned char)0x06)
        || (value == (unsigned char)0x07)
        || (value == (unsigned char)0x08)
        || (value == (unsigned char)0x09))
    {
        dat = (unsigned char)0x01;
    }
    else
    {
        dat = (unsigned char)0x00;
    }

    return dat;
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
    if (dp_on != (unsigned char)0x00)
    {
        low_dat = (unsigned char)(low_dat | TM1628_DIGIT_DP_MASK);
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
