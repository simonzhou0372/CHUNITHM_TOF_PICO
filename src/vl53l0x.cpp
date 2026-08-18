/*
 * VL53L0X TOF Sensor Implementation
 * Based on Reference implementation with full initialization
 */

#include "vl53l0x.h"
#include "board_defs.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>

namespace Chuni245Tof {

// VL53L0X I2C address
#define VL53L0X_DEFAULT_ADDR 0x29

// GPIO pins for XSHUT
static const uint8_t xshut_pins[5] = {TOF1_XSHUT, TOF2_XSHUT, TOF3_XSHUT, TOF4_XSHUT, TOF5_XSHUT};

// Sensor state
static uint8_t sensor_addr[5] = {0x30, 0x31, 0x32, 0x33, 0x34};
static bool sensor_ready[5] = {false, false, false, false, false};
static uint16_t distance_mm[5] = {0, 0, 0, 0, 0};
static uint8_t stop_variable[5] = {0};
static uint32_t i2c_error_count[5] = {0, 0, 0, 0, 0};  // I2C 错误计数

// I2C timeout - 稳定性优先
// 稳定性 > 延迟 >> 精度
#define I2C_TIMEOUT_US 20000  // 20ms，确保稳定通信

// Write register
static bool write_reg(uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_write_blocking_until(I2C1_PORT, addr, buf, 2, false, time_us_64() + I2C_TIMEOUT_US) >= 0;
}

// Read register
static bool read_reg(uint8_t addr, uint8_t reg, uint8_t *value) {
    if (i2c_write_blocking_until(I2C1_PORT, addr, &reg, 1, true, time_us_64() + I2C_TIMEOUT_US) < 0) return false;
    return i2c_read_blocking_until(I2C1_PORT, addr, value, 1, false, time_us_64() + I2C_TIMEOUT_US) >= 0;
}

// Read 16-bit register
static bool read_reg16(uint8_t addr, uint8_t reg, uint16_t *value) {
    if (i2c_write_blocking_until(I2C1_PORT, addr, &reg, 1, true, time_us_64() + I2C_TIMEOUT_US) < 0) return false;
    uint8_t buf[2];
    if (i2c_read_blocking_until(I2C1_PORT, addr, buf, 2, false, time_us_64() + I2C_TIMEOUT_US) < 0) return false;
    *value = (buf[0] << 8) | buf[1];
    return true;
}

// Write 16-bit register
static bool write_reg16(uint8_t addr, uint8_t reg, uint16_t value) {
    uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)value};
    return i2c_write_blocking_until(I2C1_PORT, addr, buf, 3, false, time_us_64() + I2C_TIMEOUT_US) >= 0;
}

// Write multiple registers from list
static void write_reg_list(uint8_t addr, const uint16_t *list) {
    const uint16_t *regs = list + 1;
    for (int i = 0; i < *list; i++) {
        write_reg(addr, regs[i] >> 8, regs[i] & 0xff);
    }
}

// Register sequences for initialization (from Pololu library)
static const uint16_t reg_mode1[] = {4, 0x8800, 0x8001, 0xff01, 0x0000};
static const uint16_t reg_mode2[] = {3, 0x0001, 0xff00, 0x8000};

// Check if sensor is present
static bool is_present(uint8_t addr) {
    uint8_t model_id = 0;
    if (!read_reg(addr, 0xC0, &model_id)) return false;
    return model_id == 0xEE;
}

// Initialize sensor - 稳定性优先配置
// MeasurementTimingBudget = 20ms
static bool init_sensor(uint8_t index) {
    uint8_t new_addr = sensor_addr[index];

    // Enable sensor
    gpio_put(xshut_pins[index], 1);
    sleep_ms(10);  // 增加等待时间，确保稳定

    // Check presence at default address
    if (!is_present(VL53L0X_DEFAULT_ADDR)) {
        printf("  TOF%d: NOT FOUND\n", index + 1);
        return false;
    }

    // Change I2C address
    write_reg(VL53L0X_DEFAULT_ADDR, 0x8A, new_addr);
    sleep_ms(10);  // 增加等待时间

    // Verify new address
    if (!is_present(new_addr)) {
        printf("  TOF%d: ADDR FAIL\n", index + 1);
        return false;
    }

    // Basic initialization sequence (from Pololu library)
    write_reg_list(new_addr, reg_mode1);

    uint8_t sv = 0;
    read_reg(new_addr, 0x91, &sv);
    stop_variable[index] = sv;

    write_reg_list(new_addr, reg_mode2);

    // ===== 稳定性优先配置 =====
    // MeasurementTimingBudget = 20ms
    // 稳定性 > 延迟 >> 精度

    // VCSEL pulse periods - 适中的值
    write_reg(new_addr, 0x27, 0x14);  // PRE_RANGE_VCSEL_PERIOD = 20
    write_reg(new_addr, 0x29, 0x14);  // FINAL_RANGE_VCSEL_PERIOD = 20

    // 超时设置 - 约 20ms 测量周期
    // 公式：timeout = value * 16 * (2^1.5) μs
    // 0x0100 ≈ 20ms
    write_reg16(new_addr, 0x28, 0x0100);  // PRE_RANGE_TIMEOUT
    write_reg16(new_addr, 0x2A, 0x0100);  // FINAL_RANGE_TIMEOUT

    // Signal rate limit - 适中的值（0.25 MCPS）
    write_reg16(new_addr, 0x44, 0x0020);

    // GPIO 配置 - data ready 中断
    write_reg(new_addr, 0x0A, 0x04);  // 新数据就绪时触发中断

    // System sequence - 仅距离测量
    write_reg(new_addr, 0x21, 0x06);

    // 启动连续测量模式 (back-to-back)
    // 0x03 = 连续 back-to-back 模式
    write_reg(new_addr, 0x00, 0x03);

    // 等待第一次测量完成
    sleep_ms(50);

    printf("  TOF%d: OK (addr=0x%02X)\n", index + 1, new_addr);
    return true;
}

void vl53l0x_init() {
    printf("[VL53L0X] Initializing I2C1...\n");

    // Initialize I2C1
    i2c_init(I2C1_PORT, 400000);
    gpio_set_function(I2C1_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C1_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C1_SDA);
    gpio_pull_up(I2C1_SCL);

    // Reset all sensors - 确保完全复位
    printf("[VL53L0X] Resetting sensors...\n");
    for (int i = 0; i < 5; i++) {
        gpio_init(xshut_pins[i]);
        gpio_set_dir(xshut_pins[i], GPIO_OUT);
        gpio_put(xshut_pins[i], 0);  // LOW = reset
    }
    sleep_ms(100);  // 等待所有传感器完全复位

    // Initialize one by one
    printf("[VL53L0X] Initializing:\n");
    for (int i = 0; i < 5; i++) {
        sensor_ready[i] = init_sensor(i);
        sleep_ms(50);  // 每个传感器之间等待，避免 I2C 冲突
    }

    int count = 0;
    for (int i = 0; i < 5; i++) {
        if (sensor_ready[i]) count++;
    }
    printf("[VL53L0X] %d/5 ready\n", count);
}

void vl53l0x_start_ranging(uint8_t index) {
    if (index >= 5 || !sensor_ready[index]) return;
    write_reg(sensor_addr[index], 0x00, 0x02);
}

void vl53l0x_stop_ranging(uint8_t index) {
    if (index >= 5 || !sensor_ready[index]) return;
    write_reg(sensor_addr[index], 0x00, 0x01);
}

// 读取距离数据（非阻塞）
// 返回值: true = 成功读取新数据, false = 数据未就绪或读取失败
// distance: 输出参数，返回距离值(mm)
// 注意: 只有返回 true 时，distance 才是新测量值，应更新timestamp
bool vl53l0x_read_distance(uint8_t index, uint16_t *distance) {
    if (index >= 5 || !sensor_ready[index] || !distance) {
        return false;
    }

    uint8_t addr = sensor_addr[index];

    // 检查数据是否就绪
    uint8_t interrupt_status = 0;
    if (!read_reg(addr, 0x13, &interrupt_status)) {
        i2c_error_count[index]++;
        return false;  // I2C 错误，不更新数据
    }

    if ((interrupt_status & 0x07) == 0) {
        // 数据未就绪，不更新数据
        return false;
    }

    // 数据已就绪，读取距离值
    uint16_t range = 0;
    if (!read_reg16(addr, 0x1E, &range)) {
        i2c_error_count[index]++;
        return false;  // I2C 错误，不更新数据
    }

    // 清除中断标志
    if (!write_reg(addr, 0x0B, 0x01)) {
        i2c_error_count[index]++;
        // 即使清除中断失败，数据仍然有效
    }

    // 验证并更新距离值
    if (range > 0 && range < 8190) {
        distance_mm[index] = range;
        *distance = range;
        return true;  // 成功获取新数据
    } else {
        distance_mm[index] = 8190;
        *distance = 8190;
        return true;  // 虽然是错误值，但确实是新数据
    }
}

// 获取上一次有效的距离数据（不触发新测量）
uint16_t vl53l0x_get_last_distance(uint8_t index) {
    if (index < 5) return distance_mm[index];
    return 8190;
}

// 检查传感器是否有新数据就绪（非阻塞检查）
bool vl53l0x_has_new_data(uint8_t index) {
    if (index >= 5 || !sensor_ready[index]) {
        return false;
    }

    uint8_t interrupt_status = 0;
    if (!read_reg(sensor_addr[index], 0x13, &interrupt_status)) {
        return false;
    }

    return (interrupt_status & 0x07) != 0;
}

bool vl53l0x_is_ready(uint8_t index) {
    return (index < 5) && sensor_ready[index];
}

uint32_t vl53l0x_get_error_count(uint8_t index) {
    if (index < 5) {
        return i2c_error_count[index];
    }
    return 0;
}

} // namespace Chuni245Tof