#include <cms.h>
#include "Touch_Kscan_Library.h"
#include "key.h"

static unsigned char key_stable_value;

static void Key_Delay(void)
{
    unsigned int i;

    for (i = 0; i < 3000; i++)
    {
        asm("clrwdt");
    }
}

static unsigned char Key_ReadRaw(void)
{
    return (unsigned char)(_CMS_KeyFlag[0] & 0B00001111);
}

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
