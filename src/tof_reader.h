/*
 * TOF Reader - Core 1 Task
 * 专用核心读取 VL53L0X，避免阻塞主循环
 */

#ifndef TOF_READER_H
#define TOF_READER_H

#include <stdint.h>
#include <stdbool.h>

namespace Chuni245Tof {

// 初始化 Core 1 TOF 读取任务
void tof_reader_init();

// 获取最新距离数据（Core 0 调用，无锁读取）
uint16_t tof_reader_get_distance(uint8_t index);

// 获取数据新鲜度（ms）
uint32_t tof_reader_get_age(uint8_t index);

// 检查 Core 1 是否运行
bool tof_reader_is_running();

// 获取读取统计
uint32_t tof_reader_get_count();
uint32_t tof_reader_get_error_count();

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* TOF_READER_H */