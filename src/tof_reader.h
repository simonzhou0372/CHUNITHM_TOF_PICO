/*
 * TOF Reader - Core 1 Task
 * 专用核心读取 VL53L0X，避免阻塞主循环
 *
 * 数据一致性保证：
 * - 使用 sequence counter 确保 Core 0 读取到一致的 distance + timestamp
 * - 只有 VL53L0X 真正产生新数据时才更新 timestamp
 */

#ifndef TOF_READER_H
#define TOF_READER_H

#include <stdint.h>
#include <stdbool.h>

namespace Chuni245Tof {

// 单个传感器的数据快照（保证一致性）
typedef struct {
    uint16_t distance;      // 距离值 (mm)
    uint32_t timestamp_ms;  // 数据更新时间戳 (ms)
    uint32_t sequence;      // 序列号（每次新数据递增）
    bool valid;             // 数据是否有效
} tof_data_snapshot_t;

// 初始化 Core 1 TOF 读取任务
void tof_reader_init();

// 获取单个传感器的数据快照（原子操作，保证一致性）
tof_data_snapshot_t tof_reader_get_snapshot(uint8_t index);

// 兼容旧接口
uint16_t tof_reader_get_distance(uint8_t index);
uint32_t tof_reader_get_age(uint8_t index);

// 检查 Core 1 是否运行
bool tof_reader_is_running();

// 获取读取统计
uint32_t tof_reader_get_count();
uint32_t tof_reader_get_error_count();

// 获取性能统计
uint32_t tof_reader_get_avg_poll_interval_us();
uint32_t tof_reader_get_max_poll_interval_us();
uint32_t tof_reader_get_new_data_count();

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* TOF_READER_H */