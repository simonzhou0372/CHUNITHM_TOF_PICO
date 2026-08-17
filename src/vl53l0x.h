/*
 * VL53L0X TOF Sensor Header
 */

#ifndef VL53L0X_H
#define VL53L0X_H

#include <stdint.h>
#include <stdbool.h>

namespace Chuni245Tof {

void vl53l0x_init();
void vl53l0x_start_ranging(uint8_t index);
void vl53l0x_stop_ranging(uint8_t index);
uint16_t vl53l0x_read_distance(uint8_t index);
uint16_t vl53l0x_get_distance(uint8_t index);
bool vl53l0x_is_ready(uint8_t index);
uint32_t vl53l0x_get_error_count(uint8_t index);  // 获取 I2C 错误计数

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* VL53L0X_H */