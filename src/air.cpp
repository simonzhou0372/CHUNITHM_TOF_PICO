/*
 * Air Sensor Implementation
 * Height level detection with VL53L0X
 * 只取最大值，无状态变化检测，无重叠分段
 * 增加：最小按下持续时间 (Minimum Hold Time)
 *
 * 性能优化：使用 Core 1 专用读取任务，避免阻塞主循环
 */

#include "air.h"
#include "tof_reader.h"
#include "config.h"
#include <stdio.h>
#include "pico/stdlib.h"

namespace Chuni245Tof {

// Air 传感器状态 (6 bits for height levels)
static uint8_t air_state = 0;  // 传感器状态 bitmap

// HID 输出状态 (经过最小按下时间处理)
static uint8_t hid_air_bitmap = 0;

// 当前最大距离
static uint16_t current_distance = 0;

// 每个 AIR 的按下时间 (ms)
static uint32_t air_press_time[6] = {0, 0, 0, 0, 0, 0};

// Valid range constants
#define MIN_VALID_MM    50      // Minimum valid distance
#define MAX_VALID_MM    800     // Maximum valid distance (800mm)
#define MAX_DATA_AGE_MS 50      // 最大数据年龄，超过此值认为数据过时

void air_init() {
    tof_reader_init();  // 启动 Core 1 读取任务
    air_state = 0;
    hid_air_bitmap = 0;
    current_distance = 0;
    for (int i = 0; i < 6; i++) {
        air_press_time[i] = 0;
    }
}

void air_update() {
    // Get config parameters (in mm)
    // Default: offset=120mm, pitch=40mm, air6_range=150mm
    uint16_t offset_mm = cfg ? cfg->tof_offset : 120;
    uint16_t pitch_mm = cfg ? cfg->tof_pitch : 40;
    uint16_t air6_range_mm = cfg ? cfg->air6_range : 150;
    uint16_t min_hold_ms = cfg ? cfg->air_min_hold_ms : 50;

    // ===== 只取最大值 =====
    uint16_t max_value = 0;
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // 从 Core 1 缓冲区读取最新数据（无阻塞）
    for (int i = 0; i < 5; i++) {
        // 检查数据新鲜度
        uint32_t age = tof_reader_get_age(i);
        if (age > MAX_DATA_AGE_MS) {
            // 数据过时，跳过
            continue;
        }

        uint16_t dist = tof_reader_get_distance(i);

        // 只取有效范围内且大于当前最大值的
        if (dist < MIN_VALID_MM || dist > MAX_VALID_MM) {
            continue;  // 无效值，跳过
        }
        if (dist > max_value) {
            max_value = dist;
        }
    }

    current_distance = max_value;

    // ===== 计算传感器状态 bitmap =====
    uint8_t sensor_bitmap = 0;

    // Air1: [offset, offset + pitch]
    if (max_value >= offset_mm && max_value <= offset_mm + pitch_mm) {
        sensor_bitmap |= (1 << 0);
    }
    // Air2: [offset + pitch, offset + pitch*2]
    if (max_value >= offset_mm + pitch_mm && max_value <= offset_mm + pitch_mm * 2) {
        sensor_bitmap |= (1 << 1);
    }
    // Air3: [offset + pitch*2, offset + pitch*3]
    if (max_value >= offset_mm + pitch_mm * 2 && max_value <= offset_mm + pitch_mm * 3) {
        sensor_bitmap |= (1 << 2);
    }
    // Air4: [offset + pitch*3, offset + pitch*4]
    if (max_value >= offset_mm + pitch_mm * 3 && max_value <= offset_mm + pitch_mm * 4) {
        sensor_bitmap |= (1 << 3);
    }
    // Air5: [offset + pitch*4, offset + pitch*5]
    if (max_value >= offset_mm + pitch_mm * 4 && max_value <= offset_mm + pitch_mm * 5) {
        sensor_bitmap |= (1 << 4);
    }
    // Air6: [offset + pitch*5, offset + pitch*5 + air6_range]
    if (max_value >= offset_mm + pitch_mm * 5 &&
        max_value <= offset_mm + pitch_mm * 5 + air6_range_mm) {
        sensor_bitmap |= (1 << 5);
    }

    air_state = sensor_bitmap;

    // ===== 最小按下持续时间处理 =====
    // 使用之前计算的 now 变量

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

} // namespace Chuni245Tof