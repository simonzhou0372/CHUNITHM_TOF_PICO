/*
 * Air Sensor Implementation
 * Height level detection with VL53L0X
 *
 * 架构改进（完整重写）：
 * 1. 每个 TOF 独立进行 AIR 距离层判定
 * 2. 每个 TOF 独立维护 AIR layer 和 hysteresis 状态
 * 3. 5 个 TOF 的结果进行 OR 合并得到 12-bit AIR bitmap
 * 4. AIR1~AIR12 映射到 HID key1~key6
 * 5. min_hold 只作用于最终 HID key
 *
 * AIR 层设计：
 * - AIR1~AIR11: 线性，每层高度 = pitch
 * - AIR12: 特殊范围 (air12_range)
 *
 * HID 映射 (OR 逻辑):
 * - key1 = AIR1 || AIR7
 * - key2 = AIR2 || AIR8
 * - key3 = AIR3 || AIR9
 * - key4 = AIR4 || AIR10
 * - key5 = AIR5 || AIR11
 * - key6 = AIR6 || AIR12
 */

#include "air.h"
#include "tof_reader.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

namespace Chuni245Tof {

//==============================================================================
// 状态定义
//==============================================================================

// 无效 AIR layer 标记
#define AIR_LAYER_INVALID  0xFF

// Air 传感器状态 (12-bit bitmap)
// bit0 = AIR1, bit1 = AIR2, ..., bit11 = AIR12
static uint16_t air_state = 0;

// HID 输出状态 (6-bit bitmap, 经过最小按下时间处理)
// bit0 = key1, bit1 = key2, ..., bit5 = key6
static uint8_t hid_air_bitmap = 0;

// 当前最大距离 (仅用于调试，不参与 AIR 判定)
static uint16_t current_max_distance = 0;

// 每个 TOF 当前所在的 AIR layer (0~11 = AIR1~AIR12, 0xFF = 无效)
static uint8_t sensor_active_layer[5] = {AIR_LAYER_INVALID, AIR_LAYER_INVALID,
                                         AIR_LAYER_INVALID, AIR_LAYER_INVALID,
                                         AIR_LAYER_INVALID};

// HID key 按下时间 (ms) - 用于 min_hold
static uint32_t key_press_time[6] = {0};

// 调试数据
static air_debug_data_t debug_data = {0};

//==============================================================================
// 常量定义
//==============================================================================

#define MIN_VALID_MM       50      // 最小有效距离
#define MAX_VALID_MM       800     // 最大有效距离
#define MAX_DATA_AGE_MS    50      // 最大数据年龄
#define HYSTERESIS_MM      5       // 滞回区间大小

#define AIR_LAYERS_ORIGINAL  6     // 原始 AIR 层数
#define AIR_LAYERS_OVERLAY   12    // 扩展 AIR 层数

//==============================================================================
// 辅助函数
//==============================================================================

/**
 * 计算给定距离属于哪个 AIR layer
 *
 * @param distance 测量距离 (mm)
 * @param offset   起始偏移 (mm)
 * @param pitch    每层高度 (mm)
 * @param air12_range AIR12 特殊范围 (mm)
 * @param num_layers 检测层数 (6 或 12)
 *
 * @return AIR layer (0~11 = AIR1~AIR12), 或 AIR_LAYER_INVALID
 */
static inline uint8_t calculate_air_layer(
    uint16_t distance,
    uint16_t offset,
    uint16_t pitch,
    uint16_t air12_range,
    int num_layers
) {
    // 低于起始偏移，不在任何 AIR 层
    if (distance < offset) {
        return AIR_LAYER_INVALID;
    }

    // 计算相对偏移的距离
    uint16_t rel_dist = distance - offset;

    // AIR1~AIR11 (线性)
    if (rel_dist < pitch * 11) {
        uint8_t layer = rel_dist / pitch;
        if (layer < num_layers) {
            return layer;
        }
    }

    // AIR12 (特殊范围)
    if (num_layers == AIR_LAYERS_OVERLAY && rel_dist >= pitch * 11) {
        uint16_t air12_dist = rel_dist - pitch * 11;
        if (air12_dist < air12_range) {
            return 11;  // AIR12
        }
    }

    return AIR_LAYER_INVALID;
}

/**
 * 使用 hysteresis 更新单个 TOF 的 AIR layer
 *
 * @param sensor_idx  传感器索引 (0~4)
 * @param distance    当前距离
 * @param offset      起始偏移
 * @param pitch       每层高度
 * @param air12_range AIR12 特殊范围
 * @param num_layers  检测层数
 *
 * @return 更新后的 AIR layer
 */
static uint8_t update_sensor_air_layer(
    int sensor_idx,
    uint16_t distance,
    uint16_t offset,
    uint16_t pitch,
    uint16_t air12_range,
    int num_layers
) {
    uint8_t prev_layer = sensor_active_layer[sensor_idx];
    uint8_t new_layer = calculate_air_layer(distance, offset, pitch, air12_range, num_layers);

    // 如果没有有效的 AIR layer
    if (new_layer == AIR_LAYER_INVALID) {
        // 检查是否需要 hysteresis 保持
        if (prev_layer != AIR_LAYER_INVALID && prev_layer < num_layers) {
            // 计算退出阈值
            uint16_t layer_lower_bound;
            if (prev_layer < 11) {
                layer_lower_bound = offset + pitch * prev_layer;
            } else {
                layer_lower_bound = offset + pitch * 11;
            }

            // 如果距离在退出阈值之内（使用 hysteresis），保持原状态
            if (distance >= (layer_lower_bound - HYSTERESIS_MM)) {
                return prev_layer;
            }
        }
        return AIR_LAYER_INVALID;
    }

    // 如果有新的有效 layer
    // 检查是否需要 hysteresis 防止跳变
    if (prev_layer != AIR_LAYER_INVALID && prev_layer != new_layer) {
        // 计算 prev_layer 的退出阈值
        uint16_t prev_lower, prev_upper;
        if (prev_layer < 11) {
            prev_lower = offset + pitch * prev_layer;
            prev_upper = offset + pitch * (prev_layer + 1);
        } else {
            prev_lower = offset + pitch * 11;
            prev_upper = offset + pitch * 11 + air12_range;
        }

        // 如果距离还在 prev_layer 的 hysteresis 范围内，保持原状态
        if (distance >= (prev_lower - HYSTERESIS_MM) && distance < prev_upper) {
            return prev_layer;
        }
    }

    return new_layer;
}

//==============================================================================
// API 实现
//==============================================================================

void air_init() {
    tof_reader_init();

    air_state = 0;
    hid_air_bitmap = 0;
    current_max_distance = 0;

    for (int i = 0; i < 5; i++) {
        sensor_active_layer[i] = AIR_LAYER_INVALID;
    }

    for (int i = 0; i < 6; i++) {
        key_press_time[i] = 0;
    }

    memset(&debug_data, 0, sizeof(debug_data));
}

void air_update() {
    // ===== 读取配置参数 =====
    uint16_t offset_mm = cfg ? cfg->tof_offset : 120;
    uint16_t pitch_mm = cfg ? cfg->tof_pitch : 30;
    uint16_t air12_range_mm = cfg ? cfg->air12_range : 150;
    uint16_t min_hold_ms = cfg ? cfg->air_min_hold_ms : 100;
    bool overlay_enabled = cfg ? (cfg->air_overlay_enabled != 0) : true;

    int num_layers = overlay_enabled ? AIR_LAYERS_OVERLAY : AIR_LAYERS_ORIGINAL;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    // ===== Per-TOF AIR 检测 =====
    uint16_t sensor_bitmap = 0;  // 12-bit AIR bitmap
    uint16_t max_dist_for_debug = 0;  // 仅用于调试

    for (int i = 0; i < 5; i++) {
        // 获取快照
        tof_data_snapshot_t snapshot = tof_reader_get_snapshot(i);

        // 保存调试数据
        debug_data.sensor_distances[i] = snapshot.distance;
        debug_data.sensor_ages[i] = snapshot.valid ? (now - snapshot.timestamp_ms) : 0xFFFFFFFF;
        debug_data.sensor_valid[i] = snapshot.valid;

        // 检查数据有效性
        if (!snapshot.valid) {
            sensor_active_layer[i] = AIR_LAYER_INVALID;
            continue;
        }

        // 检查数据新鲜度
        uint32_t age = now - snapshot.timestamp_ms;
        if (age > MAX_DATA_AGE_MS) {
            sensor_active_layer[i] = AIR_LAYER_INVALID;
            continue;
        }

        uint16_t dist = snapshot.distance;

        // 检查距离范围
        if (dist < MIN_VALID_MM || dist > MAX_VALID_MM) {
            sensor_active_layer[i] = AIR_LAYER_INVALID;
            continue;
        }

        // 更新最大距离（仅用于调试）
        if (dist > max_dist_for_debug) {
            max_dist_for_debug = dist;
        }

        // ===== 核心：计算该 TOF 的 AIR layer (带 hysteresis) =====
        uint8_t layer = update_sensor_air_layer(
            i, dist, offset_mm, pitch_mm, air12_range_mm, num_layers
        );

        sensor_active_layer[i] = layer;

        // 如果该 TOF 有有效的 AIR layer，更新 bitmap
        if (layer != AIR_LAYER_INVALID && layer < num_layers) {
            sensor_bitmap |= (1 << layer);
        }

        // 保存到调试数据
        debug_data.sensor_air_layer[i] = layer;
    }

    // 保存最大距离（仅用于调试，不参与 AIR 判定）
    current_max_distance = max_dist_for_debug;

    // 保存 AIR 状态
    air_state = sensor_bitmap;

    // ===== HID OR 映射 =====
    // key1 = AIR1 || AIR7
    // key2 = AIR2 || AIR8
    // key3 = AIR3 || AIR9
    // key4 = AIR4 || AIR10
    // key5 = AIR5 || AIR11
    // key6 = AIR6 || AIR12

    uint8_t raw_hid_bitmap = 0;

    for (int key = 0; key < 6; key++) {
        bool key_pressed = false;

        // AIR1~AIR6 映射到 key0~key5
        if (sensor_bitmap & (1 << key)) {
            key_pressed = true;
        }

        // AIR7~AIR12 映射到 key0~key5 (overlay)
        if (overlay_enabled && (sensor_bitmap & (1 << (key + 6)))) {
            key_pressed = true;
        }

        if (key_pressed) {
            raw_hid_bitmap |= (1 << key);
        }
    }

    // ===== 最小按下持续时间处理 =====
    // 只作用于最终 HID key

    for (int i = 0; i < 6; i++) {
        bool raw_on = (raw_hid_bitmap >> i) & 1;
        bool hid_on = (hid_air_bitmap >> i) & 1;

        if (raw_on && !hid_on) {
            // 新按下：立即 ON，记录时间
            hid_air_bitmap |= (1 << i);
            key_press_time[i] = now;
        }
        else if (!raw_on && hid_on) {
            // 传感器已 OFF，检查是否达到最小保持时间
            uint32_t hold_time = now - key_press_time[i];
            if (hold_time >= min_hold_ms) {
                // 达到最小时间，释放
                hid_air_bitmap &= ~(1 << i);
                key_press_time[i] = 0;
            }
            // 否则继续保持
        }
        // 其他情况：
        // - raw_on && hid_on：继续按下
        // - !raw_on && !hid_on：继续释放
    }

    // 更新调试数据
    debug_data.max_distance = current_max_distance;
    debug_data.sensor_bitmap = sensor_bitmap;
    debug_data.hid_bitmap = hid_air_bitmap;
    debug_data.timestamp_us = time_us_32();
}

uint8_t air_get_bitmap() {
    return hid_air_bitmap;  // 返回 HID 输出状态 (6-bit)
}

uint16_t air_get_air_state() {
    return air_state;  // 返回完整的 AIR 状态 (12-bit)
}

bool air_is_triggered(uint8_t sensor) {
    if (sensor < 6) {
        return (hid_air_bitmap >> sensor) & 1;
    }
    return false;
}

uint16_t air_get_distance(uint8_t sensor) {
    // 兼容性接口：返回当前最大距离（仅用于调试）
    (void)sensor;
    return current_max_distance;
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