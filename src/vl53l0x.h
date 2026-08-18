/*
 * VL53L0X TOF Sensor Header
 */

#ifndef VL53L0X_H
#define VL53L0X_H

#include <stdint.h>
#include <stdbool.h>

namespace Chuni245Tof {

// 初始化 VL53L0X 传感器
void vl53l0x_init();

// 读取距离数据
// 返回值: true = 成功读取新数据, false = 数据未就绪或读取失败
// distance: 输出参数，返回距离值(mm)
// 注意: 只有返回 true 时，distance 才是新测量值
bool vl53l0x_read_distance(uint8_t index, uint16_t *distance);

// 获取上一次有效的距离数据（不触发新测量）
uint16_t vl53l0x_get_last_distance(uint8_t index);

// 检查传感器是否已初始化就绪
bool vl53l0x_is_ready(uint8_t index);

// 检查传感器是否有新数据就绪（基于中断状态寄存器）
bool vl53l0x_has_new_data(uint8_t index);

// 获取 I2C 错误计数
uint32_t vl53l0x_get_error_count(uint8_t index);

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* VL53L0X_H */