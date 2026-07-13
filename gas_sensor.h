#ifndef __GAS_SENSOR_H__
#define __GAS_SENSOR_H__

/*
 * TP-401W 异味/空气质量传感器驱动模块
 *
 * 硬件连接 (CMS79F723):
 *   - TP-401W VCC   -> RB1 (GPIO 输出, 低电平 0 = 传感器上电供电)
 *   - TP-401W DATA  -> RB2 / AN13 (ADC 模拟输入, 读取异味浓度模拟量)
 *   - TP-401W GND   -> 系统地
 *
 * 算法说明 (适配自 air1.6.1 已验证方案):
 *   - ADC: 左对齐读取 (ADFM=0), 10-bit × 4 → 等效 0~4092 范围
 *   - 采样: 6 次去最小/最大后 4 次平均, 抗噪性强于简单平均
 *   - 滤波: 一阶 IIR 低通 (α=64), 抑制读数抖动
 *   - 映射: 非线性分段映射, 更符合 TP-401W 真实输出特性
 *   - 保护: 开短路 8 次连续确认防误判, PEIE 原子保护防 ISR 冲突
 */

/* 映射上下限 (防止端点越界) */
#define GAS_PM25_MAX      ((unsigned int)999)    /* PM2.5 显示上限 */
#define GAS_PM25_INVALID  ((unsigned int)1000)   /* Gas_ReadPm25 异常返回 (>999=无效) */

/* 开短路检测阈值 (左对齐 ADC×4 等效值, 对应 10-bit 的 ~7.5 和 ~1022) */
#define GAS_ADC_OPEN      ((unsigned int)30)     /* ADC(左对齐) < 30 判开路 */
#define GAS_ADC_SHORT     ((unsigned int)4090)   /* ADC(左对齐) > 4090 判短路 */

/* 一阶低通滤波器系数: 值越大对新数据的响应越快 (推荐 32~128) */
#define GAS_FILTER_ALPHA  ((unsigned char)64)

/* TP-401W 传感器上电预热时间 (秒) */
#define GAS_WARMUP_SECONDS    ((unsigned char)30)

/* ADC 采样次数: 6 次采集, 去最小/最大后 4 次平均 */
#define GAS_ADC_SAMPLE_CNT    ((unsigned char)6)

/*
 * 预热状态管理
 */
void Gas_StartWarmup(void);
unsigned char Gas_IsWarmupDone(void);
unsigned char Gas_GetWarmupRemaining(void);
void Gas_TickWarmupSecond(void);

/*=============================================================================
 * 公开 API
 *============================================================================*/

/*
 * Gas_Init
 * 功能: 初始化 ADC 模块, ADC 时钟 Fosc/8, 参考 Vdd, 左对齐.
 */
void Gas_Init(void);

/*
 * Gas_ReadPm25
 * 功能: 综合接口 = 6 次采样(去极值平均) + 低通滤波 + 非线性映射
 * 返回: 0~500 (有效浓度值) 或 GAS_PM25_INVALID (异常)
 * 说明: 传感器开路/短路/未上电时返回 GAS_PM25_INVALID
 */
unsigned int Gas_ReadPm25(void);

/*
 * Gas_PowerOn / Gas_PowerOff
 * 传感器电源控制 (GAS_VCC = 0 供电, =1 断电)
 */
void Gas_PowerOn(void);
void Gas_PowerOff(void);

#endif /* __GAS_SENSOR_H__ */
