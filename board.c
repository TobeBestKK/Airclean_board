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

    /* 关闭所有模拟功能，使 RA/RB/RC 口可作为数字 GPIO */
    ANSEL0 = 0x00;
    ANSEL1 = 0x00;
    ANSEL2 = 0x00;

    /*
     * RA0-RA3: 触摸通道（输入，由触摸库配置）
     * RA4-RA6: TM1628 三线输出（STB/CLK/DIO）
     * TRISx: 0=输出, 1=输入。仅清 bit0-2 置为输出，保留 bit7 不动。
     */
    TRISA = (unsigned char)(TRISA & 0B10000000);

    /* RB0=风扇电源, RB3=风扇 PWM 使能；开机强制风扇关闭 */
    TRISB0 = 0;
    TRISB3 = 0;
    FAN_VCC = 0;
    FAN_PWM = 0;

    /* TM1628A 三线空闲电平：STB/CLK/DIO 均拉高 */
    TM1628_STB = 1;
    TM1628_CLK = 1;
    TM1628_DIO = 1;
}
