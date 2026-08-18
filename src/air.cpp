/*
 * Air Sensor Implementation
 * Height level detection with VL53L0X
 *
 * 改进：
 * 1. 使用真实的 TOF 数据时间戳
 * 2. 实现滞回机制避免边界抖动
 * 3. 使用数据有效性检查
 * 4. 完善最小按下持续时间
 */

#include "air.h"
#include "tof_reader.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

namespace Chuni245Tof {

// Air 传感器状态 (6 bits for height levels)
static uint8_t air_state = 0;  // 传感器状态 bitmap

// HID 输出状态 (经过最小按下时间处理)
static uint8_t hid_air_bitmap = 0;

// 当前最大距离
static uint16_t current_distance = 0;

// 每个 AIR 的按下时间 (ms)
static uint32_t air_press_time[6] = {0};

// 调试数据
static air_debug_data_t debug_data = {0};

// Valid range constants
#define MIN_VALID_MM    50      // Minimum valid distance
#define MAX_VALID_MM    800     // Maximum valid distance (800mm)
#define MAX_DATA_AGE_MS 50      // 最大数据年龄，超过此值认为数据过时

// 滞回参数（mm）
#define HYSTERESIS_MM   5       // 滞回区间大小

void air_init() {
    tof_reader_init();  // 启动 Core 1 读取任务
    air_state = 0;
    hid_air_bitmap = 0;
    current_distance = 0;
    for (int i = 0; i < 6; i++) {
        air_press_time[i] = 0;
    }
    memset(&debug_data, 0, sizeof(debug_data));
}

void air_update() {
    // Get config parameters (in mm)
    // Default: offset=120mm, pitch=30mm, air6_range=150mm
    uint16_t offset_mm = cfg ? cfg->tof_offset : 120;
    uint16_t pitch_mm = cfg ? cfg->tof_pitch : 30;
    uint16_t air6_range_mm = cfg ? cfg->air6_range : 150;
    uint16_t min_hold_ms = cfg ? cfg->air_min_hold_ms : 100;

    // ===== 收集传感器数据并计算最大值 =====
    uint16_t max_value = 0;
    uint32_t now = to_ms_since_boot(get_absolute_time());

    int valid_sensor_count = 0;

    // 从 Core 1 缓冲区读取最新数据（无阻塞）
    for (int i = 0; i < 5; i++) {
        // 使用新的快照接口，保证数据一致性
        tof_data_snapshot_t snapshot = tof_reader_get_snapshot(i);

        // 保存调试数据
        debug_data.sensor_distances[i] = snapshot.distance;
        debug_data.sensor_ages[i] = snapshot.valid ? (now - snapshot.timestamp_ms) : 0xFFFFFFFF;
        debug_data.sensor_valid[i] = snapshot.valid;

        // 检查数据有效性
        if (!snapshot.valid) {
            continue;  // 数据无效，跳过
        }

        // 检查数据新鲜度（使用真实时间戳）
        uint32_t age = now - snapshot.timestamp_ms;
        if (age > MAX_DATA_AGE_MS) {
            continue;  // 数据过时，跳过
        }

        uint16_t dist = snapshot.distance;

        // 只取有效范围内且大于当前最大值的
        if (dist < MIN_VALID_MM || dist > MAX_VALID_MM) {
            continue;  // 无效值，跳过
        }

        if (dist > max_value) {
            max_value = dist;
        }

        valid_sensor_count++;
    }

    current_distance = max_value;

    // ===== 计算 AIR 分段（带滞回） =====
    uint8_t sensor_bitmap = 0;

    // 计算各个 AIR 层的阈值
    // Air1: [offset, offset + pitch]
    // Air2: [offset + pitch, offset + pitch*2]
    // ...
    // Air6: [offset + pitch*5, offset + pitch*5 + air6_range]

    uint16_t thresholds[7];  // 6个层的边界
    thresholds[0] = offset_mm;
    for (int i = 1; i <= 5; i++) {
        thresholds[i] = offset_mm + pitch_mm * i;
    }
    thresholds[6] = offset_mm + pitch_mm * 5 + air6_range_mm;

    // 使用滞回机制判断各个 AIR 层
    for (int layer = 0; layer < 6; layer++) {
        uint16_t enter_threshold = thresholds[layer];
        uint16_t exit_threshold = thresholds[layer] - HYSTERESIS_MM;

        // 检查当前是否已经在这个层
        bool was_in_layer = (air_state >> layer) & 1;

        bool in_layer = false;
        if (was_in_layer) {
            // 已经在这一层，使用较低的退出阈值
            in_layer = (max_value >= exit_threshold && max_value < thresholds[layer + 1]);
        } else {
            // 不在这一层，使用进入阈值
            in_layer = (max_value >= enter_threshold && max_value < thresholds[layer + 1]);
        }

        if (in_layer) {
            sensor_bitmap |= (1 << layer);
        }
    }

    air_state = sensor_bitmap;

    // ===== 最小按下持续时间处理 =====
    // 当传感器检测到 Air 时，HID 立即 ON
    // ON 后至少保持 min_hold_ms
    // 最小保持时间到了以后，如果传感器已经 OFF，则立即 OFF

    for (int i = 0; i < 6; i++) {
        bool sensor_on = (sensor_bitmap >> i) & 1;
        bool hid_on = (hid_air_bitmap >> i) & 1;

        if (sensor_on && !hid_on) {
            // 传感器从 OFF → ON：立即按下，记录时间
            hid_air_bitmap |= (1 << i);
            air_press_time[i] = now;
        }
        else if (!sensor_on && hid_on) {
            // 传感器已经 OFF，但 HID 还在 ON
            // 检查是否达到最小按下时间
            uint32_t hold_time = now - air_press_time[i];
            if (hold_time >= min_hold_ms) {
                // 达到最小时间，立即释放
                hid_air_bitmap &= ~(1 << i);
                air_press_time[i] = 0;
            }
            // 否则继续保持
        }
        // 其他情况：
        // - sensor_on && hid_on：继续按下，不更新时间
        // - !sensor_on && !hid_on：继续释放
    }

    // 更新调试数据
    debug_data.max_distance = max_value;
    debug_data.sensor_bitmap = sensor_bitmap;
    debug_data.hid_bitmap = hid_air_bitmap;
    debug_data.timestamp_us = time_us_32();
}

uint8_t air_get_bitmap() {
    return hid_air_bitmap;  // 返回 HID 输出状态
}

bool air_is_triggered(uint8_t sensor) {
    if (sensor < 6) {
        return (hid_air_bitmap >> sensor) & 1;
    }
    return false;
}

uint16_t air_get_distance(uint8_t sensor) {
    // 兼容性：返回当前最大距离
    return current_distance;
}

void air_set_threshold(uint8_t sensor, uint16_t threshold_mm) {
    // 保留接口，暂不实现
    (void)sensor;
    (void)threshold_mm;
}

air_debug_data_t air_get_debug_data() {
    return debug_data;
}

} // namespace Chuni245Tof