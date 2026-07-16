#ifndef __EEPROM_H__
#define __EEPROM_H__

#include <cms.h>

/* CMS79F723 内置 128 字节 Data EEPROM 读写封装。
   EEPROM 寄存器与 cms79f726.h 共用:
     EEADR @ 0x010D, EEDAT @ 0x010C, EEADRH @ 0x010F, EEDATH @ 0x010E,
     EECON1 @ 0x011B, EECON2 @ 0x011C
   地址范围: 0x00 ~ 0x7F

   数据布局:
     0x00  MAGIC_BYTE   校验字节 (0x5A = 已初始化)
     0x01  filter_usage 滤网使用度 (0~100) */
#define EEPROM_ADDR_MAGIC       ((unsigned char)0x00)
#define EEPROM_ADDR_FILTER      ((unsigned char)0x01)

#define EEPROM_MAGIC_VALID      ((unsigned char)0x5A)

/* ---------- 底层读写 API ---------- */

unsigned char EEPROM_ReadByte(unsigned char addr);
void EEPROM_WriteByte(unsigned char addr, unsigned char dat);

/* ---------- 高层保存/恢复 API ---------- */

/* 上电调用: 从 EEPROM 恢复滤网使用度, MAGIC 无效则返回默认值 40 */
unsigned char EEPROM_LoadFilterUsage(void);

/* 滤网使用度变化时调用: 保存到 EEPROM */
void EEPROM_SaveFilterUsage(unsigned char val);

#endif /* __EEPROM_H__ */
