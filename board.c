#include "board.h"

/*
 * 板级硬件初始化：时钟、模拟/数字口、TRIS 方向、初始电平。
 * 适配 CMS79F723，配置参考 CMS79F726 demo。
 */
void Board_Init(void)
{
    asm("nop");
    asm("clrwdt");

    /* 内部 16MHz 主频 */
    OSCCON = 0x71;

    /* 模拟功能配置：
     * ANSEL0 全关 (RA0~RA7 数字口)
     * ANSEL1 仅开 AN13(bit5) → RB2 作为 TP-401W 模拟输入，其余 AN8~AN15 关
     * ANSEL2 全关
     */
    ANSEL0 = 0x00;
    ANSEL1 = 0x20;               /* bit5 = AN13 enable */
    ANSEL2 = 0x00;

    /*
     * RA0-RA3: 触摸通道（输入，由触摸库配置）
     * RA4-RA6: TM1628 三线输出（STB/CLK/DIO）
     * TRISx: 0=输出, 1=输入。仅清 bit0-2 置为输出，保留 bit7 不动。
     */
    TRISA = (unsigned char)(TRISA & 0B10000000);

    /* RB0 = 风扇电源 (输出, 初始关闭)
     * RB1 = TP-401W 电源 (输出, 初始低=给传感器供电)
     * RB2 = TP-401W 模拟输出 (输入, 由 ADC 采集)
     * RB3 = 风扇 PWM (输出, 初始关闭)
     */
    TRISB0 = 0;
    TRISB1 = 0;
    TRISB2 = 1;
    TRISB3 = 0;
    FAN_VCC = 0;
    FAN_PWM = 0;
    GAS_VCC = 0;                /* 传感器上电：低电平供电 */

    /* TM1628A 三线空闲电平：STB/CLK/DIO 均拉高 */
    TM1628_STB = 1;
    TM1628_CLK = 1;
    TM1628_DIO = 1;
}
