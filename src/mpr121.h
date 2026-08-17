/*
 * MPR121 Touch Controller Header
 */

#ifndef MPR121_H
#define MPR121_H

#include <stdint.h>
#include <stdbool.h>

namespace Chuni245Tof {

void mpr121_init();
void mpr121_set_thresholds(uint8_t touch_thr, uint8_t release_thr);
void mpr121_update();
uint32_t mpr121_get_touch_state(uint8_t device);
bool mpr121_is_touched(uint8_t device, uint8_t channel);
void mpr121_debug_print();  // 打印 Baseline/FilteredData/Delta 调试信息
void mpr121_reset_baseline();  // 重置所有通道的 Baseline
uint32_t mpr121_get_error_count(uint8_t device);  // 获取 I2C 错误计数

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* MPR121_H */