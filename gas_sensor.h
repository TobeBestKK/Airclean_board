#ifndef __GAS_SENSOR_H__
#define __GAS_SENSOR_H__

/* H3:SMELL 外设模拟输入驱动模块。
   当前工程按 TP-401W 行为模型实现；外接模块型号、供电方式和有效电平需以实机确认。

   硬件连接 (CMS79F723):
     CND  -> RB1      (GPIO 控制线, 当前固件按低电平为开启态)
     SMELL-> RB2/AN13 (ADC 模拟输入候选)
     GND  -> 系统地

   算法说明:
     ADC : 12-bit 左对齐读取 (ADFM=0), 原始范围 0~4095
     采样: 6 次去最小/最大后 4 次平均, 抗噪性强于简单平均
     滤波: 一阶 IIR 低通 (α=64), 抑制读数抖动
     映射: 分段线性映射, 使用当前标定点换算显示值
     保护: 开短路 8 次连续确认防误判, PEIE 原子保护防 ISR 冲突 */

/* PM2.5 显示上限与异常返回值 (>999 视为无效) */
#define GAS_PM25_MAX        ((unsigned int)999)
#define GAS_PM25_INVALID    ((unsigned int)1000)

/* PM2.5 显示线性标定点: 360 为低端归零阈值, 999 为有效输入上限 */
#define GAS_PM25_DISPLAY_ZERO  ((unsigned int)360)
#define GAS_PM25_DISPLAY_FULL  ((unsigned int)999)

/* 开短路检测阈值 (当前 ADC 结果范围内的工程阈值, 需结合外设实测确认) */
#define GAS_ADC_OPEN        ((unsigned int)30)
#define GAS_ADC_SHORT       ((unsigned int)4090)

/* 一阶低通滤波系数: 值越大对新数据响应越快 (推荐 32~128) */
#define GAS_FILTER_ALPHA    ((unsigned char)64)

/* H3 外设预热时间 (当前按 TP-401W 行为模型, 单位: 秒) */
#define GAS_WARMUP_SECONDS  ((unsigned char)30)

/* ADC 采样次数: 6 次采集, 去最小/最大后 4 次平均 */
#define GAS_ADC_SAMPLE_CNT  ((unsigned char)6)

/* ---------- 预热状态管理 ---------- */
void Gas_StartWarmup(void);
unsigned char Gas_IsWarmupDone(void);
unsigned char Gas_GetWarmupRemaining(void);
void Gas_TickWarmupSecond(void);

/* ========== 公开 API ========== */

/* Gas_Init: 初始化 ADC 模块, ADC 时钟 Fosc/8, 参考 Vdd, 左对齐 */
void Gas_Init(void);

/* Gas_ReadPm25: 综合接口 = 6 次采样(去极值平均) + 低通滤波 + 分段线性映射。
   返回 0~500 (有效浓度值) 或 GAS_PM25_INVALID (开路/短路/未上电时) */
unsigned int Gas_ReadPm25(void);

/* Gas_PowerOn / Gas_PowerOff: H3 外设控制 (当前固件按 GAS_VCC=0 开启, =1 关闭) */
void Gas_PowerOn(void);
void Gas_PowerOff(void);

#endif /* __GAS_SENSOR_H__ */
