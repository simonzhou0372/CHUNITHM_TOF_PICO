/*
 * TOF Reader - Core 1 Implementation
 * 专用核心读取 VL53L0X，实现最高刷新率
 */

#include "tof_reader.h"
#include "vl53l0x.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/irq.h"
#include <stdio.h>
#include <stdatomic.h>

namespace Chuni245Tof {

// ===== 双缓冲数据结构 =====
// Core 1 写入，Core 0 读取，使用原子操作避免锁

// 距离数据 (5 个传感器)
static volatile uint16_t distance_buffer[5] = {0, 0, 0, 0, 0};

// 数据更新时间戳 (ms)
static volatile uint32_t update_time[5] = {0, 0, 0, 0, 0};

// 统计数据
static volatile uint32_t read_count = 0;
static volatile uint32_t error_count = 0;
static volatile bool core1_running = false;

// ===== Core 1 主循环 =====
static void core1_main() {
    core1_running = true;

    // VL53L0X 已在 main.cpp 中初始化
    // 等待数据稳定
    sleep_ms(100);

    printf("[TOF_READER] Core 1 reading loop started\n");

    while (core1_running) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // 快速轮询所有 5 个传感器
        for (int i = 0; i < 5; i++) {
            if (!vl53l0x_is_ready(i)) {
                continue;
            }

            // 非阻塞读取
            uint16_t dist = vl53l0x_read_distance(i);

            // 原子更新数据
            distance_buffer[i] = dist;
            update_time[i] = now;
            read_count++;
        }

        // 短暂休眠，避免过度占用 I2C 总线
        // 理论：VL53L0X 20ms 测量周期，这里 1ms 轮询足够
        sleep_ms(1);
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

uint16_t tof_reader_get_distance(uint8_t index) {
    if (index < 5) {
        return distance_buffer[index];
    }
    return 8190;
}

uint32_t tof_reader_get_age(uint8_t index) {
    if (index < 5) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        return now - update_time[index];
    }
    return 0xFFFFFFFF;
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

} // namespace Chuni245Tof