#include "board.h"

void Board_Init(void)
{
    asm("nop");
    asm("clrwdt");

    /*
     * 内部 16MHz。
     * 该配置参考 CMS79F726 demo，迁移用于 CMS79F723。
     */
    OSCCON = 0x71;

    /*
     * 今天只测试 TM1628A 显示，不使用 ADC。
     * 所以先关闭模拟功能，避免 RA/其他口被配置成模拟输入。
     */
    ANSEL0 = 0x00;
    ANSEL1 = 0x00;
    ANSEL2 = 0x00;

    /*
     * RA4/RA5/RA6 设置为输出。
     * TRISx: 0 = 输出，1 = 输入。
     *
     * 只清除 bit4/bit5/bit6，避免误改其他 RA 引脚。
     */
    /* RA0-RA3: key inputs; RA4-RA6: TM1628 outputs. */
    TRISA = (unsigned char)((TRISA | 0B00001111) & 0B10001111);

    /*
     * TM1628A 三线空闲状态：
     * STB = 1
     * CLK = 1
     * DIO = 1
     */
    TM1628_STB = 1;
    TM1628_CLK = 1;
    TM1628_DIO = 1;
}