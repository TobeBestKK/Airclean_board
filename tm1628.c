#include <cms.h>
#include "board.h"
#include "tm1628.h"

static unsigned char tm1628_ram[14];

/*
 * 简单短延时。
 * 如果显示不稳定，可以把 i < 10 改大，例如 i < 30。
 */
static void TM1628_Delay(void)
{
    unsigned char i;

    for (i = 0; i < 10; i++)
    {
        asm("nop");
    }
}

/*
 * 流水测试用长延时。
 */
static void TM1628_LongDelay(void)
{
    unsigned int d;

    for (d = 0; d < 30000; d++)
    {
        asm("clrwdt");
    }
}

/*
 * TM1628A 通信开始。
 */
static void TM1628_Start(void)
{
    TM1628_STB = 1;
    TM1628_CLK = 1;
    TM1628_DIO = 1;
    TM1628_Delay();

    TM1628_STB = 0;
    TM1628_Delay();
}

/*
 * TM1628A 通信结束。
 */
static void TM1628_Stop(void)
{
    TM1628_CLK = 1;
    TM1628_Delay();

    TM1628_STB = 1;
    TM1628_Delay();
}

/*
 * 向 TM1628A 写 1 字节。
 * TM1628/TM1628A 通常低位先发。
 */
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

/*
 * 写 TM1628A 命令。
 */
static void TM1628_WriteCmd(unsigned char cmd)
{
    TM1628_Start();
    TM1628_WriteByte(cmd);
    TM1628_Stop();
}

/*
 * 固定地址写显示 RAM。
 *
 * addr: 0x00 ~ 0x0D
 * dat : 写入该地址的数据
 */
void TM1628_WriteByteToAddr(unsigned char addr, unsigned char dat)
{
    unsigned char cmd;

    if (addr < (unsigned char)14)
    {
        tm1628_ram[addr] = dat;
    }

    /*
     * 0x44：固定地址写模式
     */
    TM1628_WriteCmd((unsigned char)0x44);

    /*
     * 0xC0 + addr：设置显示 RAM 地址
     */
    cmd = (unsigned char)((unsigned char)0xC0 + addr);

    TM1628_Start();
    TM1628_WriteByte(cmd);
    TM1628_WriteByte(dat);
    TM1628_Stop();
}

/*
 * 清屏。
 */
void TM1628_Clear(void)
{
    unsigned char i;

    for (i = 0; i < 14; i++)
    {
        TM1628_WriteByteToAddr(i, (unsigned char)0x00);
    }
}

/*
 * 全灭。
 * 和 Clear 作用一样，保留这个函数名方便 main.c 里读起来清楚。
 */
void TM1628_AllOff(void)
{
    TM1628_Clear();
}

#define TM1628_LED_SEG1_MASK        ((unsigned char)0x01)
#define TM1628_LED_PIN2_ADDR  ((unsigned char)0x02)
#define TM1628_LED_PIN3_ADDR  ((unsigned char)0x00)
#define TM1628_LED_PIN4_ADDR  ((unsigned char)0x04)
#define TM1628_LED_PIN5_ADDR        ((unsigned char)0x06)
#define TM1628_LED_SEG1_CLEAR_MASK  ((unsigned char)0xFE)
#define TM1628_DIGIT_SEG_LOW_MASK   ((unsigned char)0xFE)
#define TM1628_DIGIT_SEG_HIGH_MASK  ((unsigned char)0x01)
#define TM1628_DIG4_GRID            ((unsigned char)0x04)
#define TM1628_DIG5_GRID            ((unsigned char)0x03)
#define TM1628_DIG6_GRID            ((unsigned char)0x02)
#define TM1628_DIGIT_DP_MASK        ((unsigned char)0x80)
#define TM1628_SPEED_MAX_LEVEL      ((unsigned char)3)
#define TM1628_TIMER_MAX_HOURS      ((unsigned char)24)

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

void TM1628_SetLeds(unsigned char leds)
{
    TM1628_WriteLedByAddr(TM1628_LED_PIN2_ADDR, (unsigned char)(leds & (unsigned char)0x01));
    TM1628_WriteLedByAddr(TM1628_LED_PIN3_ADDR, (unsigned char)(leds & (unsigned char)0x02));
    TM1628_WriteLedByAddr(TM1628_LED_PIN4_ADDR, (unsigned char)(leds & (unsigned char)0x04));
    TM1628_WriteLedByAddr(TM1628_LED_PIN5_ADDR, (unsigned char)(leds & (unsigned char)0x08));
}

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

void TM1628_SetSpeedDisplay(unsigned char value)
{
    if (value > TM1628_SPEED_MAX_LEVEL)
    {
        value = TM1628_SPEED_MAX_LEVEL;
    }

    TM1628_SetDigitByGrid(TM1628_DIG4_GRID, value, (unsigned char)0x01);
}

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

    /*
     * DP 作为 DIG5/DIG6 的单位图标（TIME / h），常亮，不随 enabled/value 变化。
     * enabled 参数保留以维持函数签名兼容，不再用于 DP 控制。
     */
    dp_on = (unsigned char)0x01;

    TM1628_SetDigitByGrid(TM1628_DIG5_GRID, tens, dp_on);
    TM1628_SetDigitByGrid(TM1628_DIG6_GRID, ones, dp_on);
}

/*
 * TM1628_SetDefaultDisplay
 * 开机默认显示：6 位数码管全部显示 0，DP 全亮作为单位图标。
 * 对应布局 "000 0 00"：
 *   DIG1/DIG2/DIG3 = PM2.5（000），DIG1/DIG2 的 DP=PM2.5 图标，DIG3 的 DP=μg/m³ 图标
 *   DIG4           = 风速档位（0），DP=GEAR 图标
 *   DIG5/DIG6      = 定时小时（00），DIG5 的 DP=TIME 图标，DIG6 的 DP=h 图标
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

void TM1628_DigitsAllOn(void)
{
    TM1628_SetDigitAllOnByGrid((unsigned char)7);
    TM1628_SetDigitAllOnByGrid((unsigned char)6);
    TM1628_SetDigitAllOnByGrid((unsigned char)5);
    TM1628_SetDigitAllOnByGrid((unsigned char)4);
    TM1628_SetDigitAllOnByGrid((unsigned char)3);
    TM1628_SetDigitAllOnByGrid((unsigned char)2);
}

/*
 * 全亮测试。
 * 如果板子接线和 TM1628A 通信正常，应看到数码管/LED 有反应。
 * 注意：不是每个地址/bit 都一定接了实际 LED，所以可能不是所有灯都亮。
 */
void TM1628_AllOn(void)
{
    unsigned char i;

    for (i = 0; i < 14; i++)
    {
        TM1628_WriteByteToAddr(i, (unsigned char)0xFF);
    }
}

/*
 * TM1628A 初始化。
 */
void TM1628_Init(void)
{
    /*
     * 0x03：显示模式。
     * 先按常见 7 Grid / 11 Segment 模式测试。
     */
    TM1628_WriteCmd((unsigned char)0x03);

    /*
     * 0x40：地址自增写模式。
     */
    TM1628_WriteCmd((unsigned char)0x40);

    /*
     * 上电后显示 RAM 状态不确定，先清屏。
     */
    TM1628_Clear();

    /*
     * 0x8F：显示开，亮度最大。
     */
    TM1628_WriteCmd((unsigned char)0x8F);
}

/*
 * 地址/位流水测试。
 * 用来记录 TM1628A 显示 RAM 到实际 LED/数码管段位的映射。
 *
 * 注意：
 * 不要使用变量名 bit，CMS 编译器里 bit 可能是保留字。
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