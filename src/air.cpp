/*
 * Air Sensor Implementation
 * Height level detection with VL53L0X
 *
 * 改进：
 * 1. 使用真实的 TOF 数据时间戳
 * 2. 实现滞回机制避免边界抖动
 * 3. 使用数据有效性检查
 * 4. 完善最小按下持续时间
 * 5. AIR Overlay Mode - 扩展到 12 层 AIR 检测
 *    - AIR1~AIR6: 原始距离层
 *    - AIR7~AIR12: AIR6 上方的扩展层 (仅当 overlay_enabled)
 *    - HID 映射: AIR1/AIR7 -> key1, AIR2/AIR8 -> key2, ...
 */

#include "air.h"
#include "tof_reader.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

namespace Chuni245Tof {

// Air 传感器状态 (12-bit bitmap)
// bit0 = AIR1, bit1 = AIR2, ..., bit5 = AIR6
// bit6 = AIR7, bit7 = AIR8, ..., bit11 = AIR12
static uint16_t air_state = 0;

// HID 输出状态 (6-bit bitmap, 经过最小按下时间处理)
// bit0 = key1, bit1 = key2, ..., bit5 = key6
// key1 = AIR1 || AIR7, key2 = AIR2 || AIR8, ...
static uint8_t hid_air_bitmap = 0;

// 当前最大距离
static uint16_t current_distance = 0;

// 每个 AIR 的按下时间 (ms) - 扩展到 12 个
static uint32_t air_press_time[12] = {0};

// 调试数据
static air_debug_data_t debug_data = {0};

// Valid range constants
#define MIN_VALID_MM    50      // Minimum valid distance
#define MAX_VALID_MM    800     // Maximum valid distance (800mm)
#define MAX_DATA_AGE_MS 50      // 最大数据年龄，超过此值认为数据过时

// 滞回参数（mm）
#define HYSTERESIS_MM   5       // 滞回区间大小

// AIR 层数定义
#define AIR_LAYERS_ORIGINAL  6   // 原始 AIR 层数 (AIR1~AIR6)
#define AIR_LAYERS_OVERLAY   12  // 扩展 AIR 层数 (AIR1~AIR12)

void air_init() {
    tof_reader_init();  // 启动 Core 1 读取任务
    air_state = 0;
    hid_air_bitmap = 0;
    current_distance = 0;
    for (int i = 0; i < 12; i++) {
        air_press_time[i] = 0;
    }
    memset(&debug_data, 0, sizeof(debug_data));
}

void air_update() {
    // Get config parameters (in mm)
    // Default: offset=120mm, pitch=30mm, air12_range=150mm
    uint16_t offset_mm = cfg ? cfg->tof_offset : 120;
    uint16_t pitch_mm = cfg ? cfg->tof_pitch : 30;
    uint16_t air12_range_mm = cfg ? cfg->air12_range : 150;
    uint16_t min_hold_ms = cfg ? cfg->air_min_hold_ms : 100;
    bool overlay_enabled = cfg ? (cfg->air_overlay_enabled != 0) : true;

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
    // 确定检测层数
    int num_layers = overlay_enabled ? AIR_LAYERS_OVERLAY : AIR_LAYERS_ORIGINAL;

    uint16_t sensor_bitmap = 0;

    // 计算各个 AIR 层的阈值
    // ========================================
    // 新设计：AIR1~AIR11 线性，AIR12 特殊范围
    // ========================================
    // AIR1:  [offset, offset + pitch)
    // AIR2:  [offset + pitch, offset + pitch*2)
    // ...
    // AIR11: [offset + pitch*10, offset + pitch*11)
    // AIR12: [offset + pitch*11, offset + pitch*11 + air12_range)
    //
    // 所有 AIR 层（包括 overlay）都是线性的，
    // 只有 AIR12 有更大的检测范围

    // 阈值数组: 13 个边界值定义 12 个区间
    uint16_t thresholds[13];
    thresholds[0] = offset_mm;

    // AIR1~AIR11 边界（线性，每层高度 = pitch）
    for (int i = 1; i <= 11; i++) {
        thresholds[i] = offset_mm + pitch_mm * i;
    }

    // AIR12 上边界（特殊范围）
    thresholds[12] = offset_mm + pitch_mm * 11 + air12_range_mm;

    // 使用滞回机制判断各个 AIR 层
    for (int layer = 0; layer < num_layers; layer++) {
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

    // ===== HID 输出计算 =====
    //
    // HID key 映射 (OR 逻辑):
    //   key1 = AIR1 || AIR7
    //   key2 = AIR2 || AIR8
    //   key3 = AIR3 || AIR9
    //   key4 = AIR4 || AIR10
    //   key5 = AIR5 || AIR11
    //   key6 = AIR6 || AIR12
    //
    // 最小按下时间处理在 OR 之后的 key 上进行

    // 计算原始 HID 状态 (OR 映射)
    uint8_t raw_hid_bitmap = 0;
    for (int key = 0; key < 6; key++) {
        bool key_pressed = false;

        // AIR1~AIR6 对应 key0~key5
        if (sensor_bitmap & (1 << key)) {
            key_pressed = true;
        }

        // AIR7~AIR12 对应 key0~key5 (overlay)
        if (overlay_enabled && (sensor_bitmap & (1 << (key + 6)))) {
            key_pressed = true;
        }

        if (key_pressed) {
            raw_hid_bitmap |= (1 << key);
        }
    }

    // ===== 最小按下持续时间处理 =====
    // 当传感器检测到 Air 时，HID 立即 ON
    // ON 后至少保持 min_hold_ms
    // 最小保持时间到了以后，如果传感器已经 OFF，则立即 OFF

    for (int i = 0; i < 6; i++) {
        bool sensor_on = (raw_hid_bitmap >> i) & 1;
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
    debug_data.sensor_bitmap = sensor_bitmap;  // 12-bit
    debug_data.hid_bitmap = hid_air_bitmap;    // 6-bit
    debug_data.timestamp_us = time_us_32();
}

uint8_t air_get_bitmap() {
    return hid_air_bitmap;  // 返回 HID 输出状态 (6-bit)
}

uint16_t air_get_air_state() {
    return air_state;  // 返回完整的 AIR 状态 (12-bit, 用于调试)
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