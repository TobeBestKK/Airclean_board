#include "eeprom.h"

/* CMS79F723 Data EEPROM 读写实现。

   写时序 (参考 CMS 标准流程):
     1. 等待 WR 位清零 (前一次写完成)
     2. 置 EEADR, EEDAT
     3. EEPGD = 0 (选 Data EEPROM)
     4. WREN = 1
     5. 关 PEIE (禁止中断, 防止写时序被打断)
     6. EECON2 = 0x55, EECON2 = 0xAA
     7. WR = 1
     8. 开 PEIE
     9. 等待 WR 位清零 (写入完成)
    10. WREN = 0

   读时序:
     1. 等待 WR 位清零
     2. 置 EEADR
     3. EEPGD = 0
     4. RD = 1
     5. 读 EEDAT (下一条指令即有效) */

/* ---------- 底层读写 ---------- */

unsigned char EEPROM_ReadByte(unsigned char addr)
{
    while (WR != 0)              /* 等待前一次写完成 */
    {
        asm("clrwdt");
    }

    EEADR = addr;
    EEPGD = 0;                   /* 选 Data EEPROM */
    RD = 1;                      /* 发起读操作 */

    /* RD 置位后下一条指令即可读 EEDAT (硬件自动完成) */
    return (unsigned char)EEDAT;
}

void EEPROM_WriteByte(unsigned char addr, unsigned char dat)
{
    while (WR != 0)              /* 等待前一次写完成 */
    {
        asm("clrwdt");
    }

    EEADR = addr;
    EEDAT = dat;
    EEPGD = 0;                   /* 选 Data EEPROM */
    WREN = 1;                    /* 允许写 */

    /* 标准解锁序列: 必须在连续指令中完成, 中间不能中断 */
    PEIE = 0;
    EECON2 = (unsigned char)0x55;
    EECON2 = (unsigned char)0xAA;
    WR = 1;                      /* 触发写入 */
    PEIE = 1;

    while (WR != 0)              /* 等待写入完成 */
    {
        asm("clrwdt");
    }

    WREN = 0;                    /* 禁止写 */
}

/* ---------- 高层保存/恢复 ---------- */

unsigned char EEPROM_LoadFilterUsage(void)
{
    unsigned char magic;
    unsigned char val;

    magic = EEPROM_ReadByte(EEPROM_ADDR_MAGIC);

    if (magic == EEPROM_MAGIC_VALID)
    {
        val = EEPROM_ReadByte(EEPROM_ADDR_FILTER);

        if (val > (unsigned char)100)   /* 合法性检查: 上限保护 */
        {
            val = (unsigned char)40;
        }
    }
    else
    {
        /* 首次上电或 EEPROM 未初始化 → 使用默认值 40, 并写入 MAGIC 标记 */
        val = (unsigned char)40;
        EEPROM_WriteByte(EEPROM_ADDR_MAGIC,  EEPROM_MAGIC_VALID);
        EEPROM_WriteByte(EEPROM_ADDR_FILTER, val);
    }

    return val;
}

void EEPROM_SaveFilterUsage(unsigned char val)
{
    if (val > (unsigned char)100)
    {
        val = (unsigned char)100;
    }

    EEPROM_WriteByte(EEPROM_ADDR_FILTER, val);
}
