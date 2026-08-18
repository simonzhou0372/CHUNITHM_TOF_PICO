/*
 * Air Sensor Header
 *
 * 改进：
 * - 使用滞回机制避免边界抖动
 * - 使用真实的 TOF 数据时间戳
 * - 支持数据有效性检查
 */

#ifndef AIR_H
#define AIR_H

#include <stdint.h>
#include <stdbool.h>

namespace Chuni245Tof {

void air_init();
void air_update();
uint8_t air_get_bitmap();
bool air_is_triggered(uint8_t sensor);
uint16_t air_get_distance(uint8_t sensor);

// 设置阈值（保留接口）
void air_set_threshold(uint8_t sensor, uint16_t threshold_mm);

// 获取调试数据
typedef struct {
    uint16_t max_distance;       // 当前最大距离
    uint8_t sensor_bitmap;       // 传感器原始状态
    uint8_t hid_bitmap;          // HID 输出状态
    uint32_t timestamp_us;       // 更新时间戳
    uint16_t sensor_distances[5]; // 各传感器距离
    uint32_t sensor_ages[5];     // 各传感器数据年龄
    bool sensor_valid[5];        // 各传感器数据有效性
} air_debug_data_t;

air_debug_data_t air_get_debug_data();

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* AIR_H */