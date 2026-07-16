#include "board.h"

/* 板级硬件初始化: 时钟、模拟/数字口、TRIS 方向、初始电平 (CMS79F723) */
void Board_Init(void)
{
    asm("nop");
    asm("clrwdt");

    /* 内部 16MHz 主频 */
    OSCCON = 0x71;

    /* 模拟口配置: ANSEL0/2 全关, ANSEL1 仅开 AN13(bit5) → RB2/AN13 连接 H3:SMELL */
    ANSEL0 = 0x00;
    ANSEL1 = 0x20;
    ANSEL2 = 0x00;

    /* 清除 TRISA bit0~bit6, 保留 bit7; RA0~RA3 的最终触摸输入方向由触摸库后续配置 */
    TRISA = (unsigned char)(TRISA & 0B10000000);

    /* RB0=风扇电源, RB1=H3:CND 控制, RB2=H3:SMELL 模拟输入, RB3=风扇 PWM */
    TRISB0 = 0;
    TRISB1 = 0;
    TRISB2 = 1;
    TRISB3 = 0;
    FAN_VCC = 0;
    FAN_PWM = 0;
    GAS_VCC = 0;                /* 当前工程按低电平为外设开启态处理 */

    /* TM1628A 三线空闲电平: STB/CLK/DIO 先置高; 读键时 DIO 需由驱动释放为输入 */
    TM1628_STB = 1;
    TM1628_CLK = 1;
    TM1628_DIO = 1;
}
