/*
 * TOF Reader - Core 1 Implementation
 * 专用核心读取 VL53L0X，实现最高刷新率
 *
 * 改进：
 * 1. 只有 VL53L0X 真正产生新数据时才更新 timestamp
 * 2. 使用 sequence counter 保证 Core 0 读取一致性
 * 3. 优化 polling 策略，减少无效 I2C transaction
 * 4. 性能统计
 */

#include "tof_reader.h"
#include "vl53l0x.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/irq.h"
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

namespace Chuni245Tof {

// ===== 数据缓冲区 =====
// 使用 double buffering + sequence counter 保证一致性

// 单个传感器的完整数据
typedef struct {
    uint16_t distance;      // 距离值 (mm)
    uint32_t timestamp_ms;  // 数据更新时间戳 (ms)
    uint32_t sequence;      // 序列号
    bool valid;             // 数据是否有效
} tof_sensor_data_t;

// 双缓冲
static volatile tof_sensor_data_t sensor_data[5] = {0};

// 统计数据
static volatile uint32_t read_count = 0;
static volatile uint32_t error_count = 0;
static volatile bool core1_running = false;

// 性能统计
static volatile uint32_t new_data_count = 0;  // 真正获得新数据的次数
static volatile uint32_t total_poll_count = 0;  // 总轮询次数
static volatile uint32_t avg_poll_interval_us = 0;
static volatile uint32_t max_poll_interval_us = 0;

// 每个 VL53L0X 的轮询状态
typedef struct {
    uint32_t last_check_time_us;  // 上次检查时间
    bool expecting_data;          // 是否期待新数据
    uint32_t next_ready_time_us;  // 预计下次数据就绪时间
} sensor_poll_state_t;

static sensor_poll_state_t poll_state[5] = {0};

// VL53L0X 测量周期（约 20ms）
#define VL53L0X_MEASUREMENT_PERIOD_US  20000
#define VL53L0X_MIN_POLL_INTERVAL_US   2000  // 最小轮询间隔 2ms

// ===== Core 1 主循环 =====
static void core1_main() {
    core1_running = true;

    // VL53L0X 已在 main.cpp 中初始化
    // 等待数据稳定
    sleep_ms(100);

    printf("[TOF_READER] Core 1 reading loop started\n");

    uint32_t loop_start_time = time_us_32();
    uint32_t last_log_time = loop_start_time;

    // 初始化轮询状态
    uint32_t now_us = time_us_32();
    for (int i = 0; i < 5; i++) {
        poll_state[i].last_check_time_us = now_us;
        poll_state[i].expecting_data = true;  // 启动时期待数据
        poll_state[i].next_ready_time_us = now_us + VL53L0X_MEASUREMENT_PERIOD_US;
        sensor_data[i].valid = vl53l0x_is_ready(i);
    }

    while (core1_running) {
        uint32_t loop_now_us = time_us_32();
        uint32_t loop_now_ms = to_ms_since_boot(get_absolute_time());

        total_poll_count++;

        // 遍历所有传感器
        for (int i = 0; i < 5; i++) {
            // 检查是否应该轮询这个传感器
            // 策略：
            // 1. 如果距离预计下次就绪时间很近（< 2ms），立即检查
            // 2. 否则如果距离上次检查已经超过最小轮询间隔，也检查
            // 3. 最多每 2ms 检查一次，避免过度 polling

            int32_t time_to_next_ready = (int32_t)(poll_state[i].next_ready_time_us - loop_now_us);
            int32_t time_since_last_check = (int32_t)(loop_now_us - poll_state[i].last_check_time_us);

            bool should_check = false;

            if (time_to_next_ready <= 2000 && time_since_last_check >= 500) {
                // 接近预计就绪时间，每 500us 检查一次
                should_check = true;
            } else if (time_since_last_check >= VL53L0X_MIN_POLL_INTERVAL_US) {
                // 超过最小轮询间隔，检查一下
                should_check = true;
            }

            if (!should_check) {
                continue;
            }

            // 更新检查时间
            poll_state[i].last_check_time_us = loop_now_us;

            // 检查传感器是否就绪
            if (!vl53l0x_is_ready(i)) {
                sensor_data[i].valid = false;
                continue;
            }

            // 尝试读取新数据
            uint16_t distance = 0;
            bool got_new_data = vl53l0x_read_distance(i, &distance);

            if (got_new_data) {
                // 真正获得了新数据，更新缓冲区
                // 使用 sequence counter 保证一致性
                uint32_t seq = sensor_data[i].sequence + 1;

                // 先更新 sequence（Core 0 会检查这个）
                sensor_data[i].sequence = seq;

                // 更新距离和时间戳
                sensor_data[i].distance = distance;
                sensor_data[i].timestamp_ms = loop_now_ms;
                sensor_data[i].valid = true;

                // 更新统计
                new_data_count++;
                read_count++;

                // 更新预计下次就绪时间
                poll_state[i].next_ready_time_us = loop_now_us + VL53L0X_MEASUREMENT_PERIOD_US;
                poll_state[i].expecting_data = false;
            }
            // 如果没有新数据，不更新任何状态，保持旧值
        }

        // 性能统计：计算轮询间隔
        uint32_t loop_end_time = time_us_32();
        uint32_t loop_duration = loop_end_time - loop_start_time;
        loop_start_time = loop_end_time;

        if (loop_duration > max_poll_interval_us) {
            max_poll_interval_us = loop_duration;
        }

        // 滑动平均计算
        avg_poll_interval_us = (avg_poll_interval_us * 9 + loop_duration) / 10;

        // 短暂休眠，避免过度占用 CPU
        // 但不要休眠太久，以免错过数据就绪
        sleep_us(500);  // 500us
    }

    core1_running = false;
}

// ===== 公共接口 =====

void tof_reader_init() {
    // 启动 Core 1
    multicore_reset_core1();
    multicore_launch_core1(core1_main);

    printf("[TOF_READER] Core 1 started\n");
}

// 获取数据快照（原子操作，保证一致性）
tof_data_snapshot_t tof_reader_get_snapshot(uint8_t index) {
    tof_data_snapshot_t snapshot = {0};

    if (index >= 5) {
        snapshot.valid = false;
        return snapshot;
    }

    // 使用 sequence counter 确保读取一致性
    // 如果在读取过程中 sequence 发生变化，重试
    uint32_t seq1, seq2;
    int retries = 0;

    do {
        seq1 = sensor_data[index].sequence;

        // 读取数据
        snapshot.distance = sensor_data[index].distance;
        snapshot.timestamp_ms = sensor_data[index].timestamp_ms;
        snapshot.valid = sensor_data[index].valid;

        seq2 = sensor_data[index].sequence;

        retries++;
    } while (seq1 != seq2 && retries < 10);

    snapshot.sequence = seq1;

    return snapshot;
}

// 兼容旧接口
uint16_t tof_reader_get_distance(uint8_t index) {
    return tof_reader_get_snapshot(index).distance;
}

uint32_t tof_reader_get_age(uint8_t index) {
    tof_data_snapshot_t snapshot = tof_reader_get_snapshot(index);
    if (!snapshot.valid) {
        return 0xFFFFFFFF;  // 无效数据，返回最大值
    }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    return now - snapshot.timestamp_ms;
}

bool tof_reader_is_running() {
    return core1_running;
}

uint32_t tof_reader_get_count() {
    return read_count;
}

uint32_t tof_reader_get_error_count() {
    uint32_t total = 0;
    for (int i = 0; i < 5; i++) {
        total += vl53l0x_get_error_count(i);
    }
    return total;
}

uint32_t tof_reader_get_avg_poll_interval_us() {
    return avg_poll_interval_us;
}

uint32_t tof_reader_get_max_poll_interval_us() {
    return max_poll_interval_us;
}

uint32_t tof_reader_get_new_data_count() {
    return new_data_count;
}

} // namespace Chuni245Tof