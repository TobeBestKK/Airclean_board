#ifndef __KEY_H__
#define __KEY_H__

/* bit0=K1(定时) bit1=K2(指示) bit2=K3(电源) bit3=K4(风速) */
#define KEY_MASK_K1  ((unsigned char)0x01)
#define KEY_MASK_K2  ((unsigned char)0x02)
#define KEY_MASK_K3  ((unsigned char)0x04)
#define KEY_MASK_K4  ((unsigned char)0x08)

unsigned char Key_ReadStable(void);

#endif
