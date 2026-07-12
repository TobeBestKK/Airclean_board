#include <cms.h>
#include "Touch_Kscan_Library.h"
#include "key.h"

/*
 * 触摸按键软件去抖读取模块。
 * 触摸库把 K1~K4 状态放在 _CMS_KeyFlag[0] 的低 4 位，
 * 本模块在软件层再做三次采样一致确认，输出稳定键值。
 */

/* 上次确认稳定的键值缓存（无新按键确认时维持原值） */
static unsigned char key_stable_value;

/* 去抖延时：循环中喂看门狗，约数毫秒 */
static void Key_Delay(void)
{
    unsigned int i;

    for (i = 0; i < 3000; i++)
    {
        asm("clrwdt");
    }
}

/* 读取触摸库当前键值，取低 4 位 (bit0=K1 .. bit3=K4) */
static unsigned char Key_ReadRaw(void)
{
    return (unsigned char)(_CMS_KeyFlag[0] & 0B00001111);
}

/*
 * 三次采样去抖读取：连续三次读到的原始值一致才更新稳定值，
 * 否则维持上次结果。返回当前稳定的键值。
 */
unsigned char Key_ReadStable(void)
{
    unsigned char first;
    unsigned char second;
    unsigned char third;

    first = Key_ReadRaw();
    Key_Delay();
    second = Key_ReadRaw();
    Key_Delay();
    third = Key_ReadRaw();

    if ((first == second) && (second == third))
    {
        key_stable_value = third;
    }

    return key_stable_value;
}
