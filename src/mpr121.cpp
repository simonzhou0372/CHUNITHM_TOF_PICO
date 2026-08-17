/*
 * MPR121 Touch Controller Implementation
 * Uses I2C0 (SDA=16, SCL=17)
 */

#include "mpr121.h"
#include "board_defs.h"
#include "config.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include <stdio.h>
#include <string.h>

namespace Chuni245Tof {

// MPR121 Register addresses
#define MPR121_TOUCHSTATUS_L  0x00
#define MPR121_TOUCHSTATUS_H  0x01
#define MPR121_DEBOUNCE       0x5B
#define MPR121_CONFIG1        0x5C
#define MPR121_CONFIG2        0x5D
#define MPR121_ECR            0x5E
#define MPR121_SOFTRESET      0x80
#define MPR121_TOUCHTH_L      0x41
#define MPR121_RELEASETH_L    0x42  // 修正: ELE0 Release Threshold

// Auto-Configuration registers
#define MPR121_USL            0x7B  // Upper Signal Limit
#define MPR121_LSL            0x7C  // Lower Signal Limit
#define MPR121_TL             0x7D  // Target Level
#define MPR121_ACCR0          0x7E  // Auto-Configuration Control Register 0

// Filter configuration registers
#define MPR121_MHDR           0x2B
#define MPR121_NHDR           0x2C
#define MPR121_NCLR           0x2D
#define MPR121_FDLR           0x2E
#define MPR121_MHDF           0x2F
#define MPR121_NHDF           0x30
#define MPR121_NCLF           0x31
#define MPR121_FDLF           0x32
#define MPR121_NHDT           0x33
#define MPR121_NCLT           0x34
#define MPR121_FDLT           0x35

// Data registers for each electrode
// ELE0: Filtered=0x04-0x05, Baseline=0x06-0x07
// ELE1: Filtered=0x08-0x09, Baseline=0x0A-0x0B
// Each electrode: 4 bytes (FilteredL, FilteredH, BaselineL, BaselineH)
#define MPR121_ELE0_FILTERED  0x04
#define MPR121_ELE0_BASELINE  0x06

static uint8_t mpr_addr[3] = {MPR121_ADDR_1, MPR121_ADDR_2, MPR121_ADDR_3};
static uint32_t touch_state[3] = {0, 0, 0};
static bool mpr_ready[3] = {false, false, false};
static uint32_t i2c_error_count[3] = {0, 0, 0};  // I2C 错误计数

#define I2C_TIMEOUT_US 1000000

// Write byte to MPR121 with timeout
static bool mpr_write_byte(uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    int ret = i2c_write_blocking_until(I2C0_PORT, addr, buf, 2, false,
                                        time_us_64() + I2C_TIMEOUT_US);
    return ret >= 0;
}

// Read byte from MPR121 with timeout
static bool mpr_read_byte(uint8_t addr, uint8_t reg, uint8_t *value) {
    int ret = i2c_write_blocking_until(I2C0_PORT, addr, &reg, 1, true,
                                        time_us_64() + I2C_TIMEOUT_US);
    if (ret < 0) return false;

    ret = i2c_read_blocking_until(I2C0_PORT, addr, value, 1, false,
                                   time_us_64() + I2C_TIMEOUT_US);
    return ret >= 0;
}

// Read 16-bit value from MPR121 (little-endian)
static bool mpr_read_word(uint8_t addr, uint8_t reg, uint16_t *value) {
    uint8_t buf[2];
    int ret = i2c_write_blocking_until(I2C0_PORT, addr, &reg, 1, true,
                                        time_us_64() + I2C_TIMEOUT_US);
    if (ret < 0) return false;

    ret = i2c_read_blocking_until(I2C0_PORT, addr, buf, 2, false,
                                   time_us_64() + I2C_TIMEOUT_US);
    if (ret < 0) return false;

    *value = buf[0] | (buf[1] << 8);  // Little-endian
    return true;
}

// Initialize single MPR121
static bool mpr_init_single(uint8_t addr, int index) {
    printf("  MPR%d [0x%02X]: ", index + 1, addr);

    // Soft reset
    mpr_write_byte(addr, MPR121_SOFTRESET, 0x63);
    sleep_ms(2);

    // Check if device responds
    uint8_t check = 0;
    if (!mpr_read_byte(addr, 0x5C, &check)) {
        printf("NO RESPONSE\n");
        return false;
    }

    if (check == 0xFF || check == 0x00) {
        printf("INVALID (0x%02X)\n", check);
        return false;
    }

    // Set touch/release thresholds for all 12 channels
    // 高阈值 + 小迟滞区 = 稳定且灵敏
    uint8_t touch_thr = cfg ? cfg->touch_threshold : 20;
    uint8_t release_thr = cfg ? cfg->release_threshold : 18;

    for (int i = 0; i < 12; i++) {
        mpr_write_byte(addr, MPR121_TOUCHTH_L + i * 2, touch_thr);
        mpr_write_byte(addr, MPR121_RELEASETH_L + i * 2, release_thr);
    }

    // Filter configuration - 经过大量实际验证的参数
    // Rising filter (手指离开后 Baseline 恢复跟踪)
    mpr_write_byte(addr, MPR121_MHDR, 0x01);
    mpr_write_byte(addr, MPR121_NHDR, 0x01);
    mpr_write_byte(addr, MPR121_NCLR, 0x0E);
    mpr_write_byte(addr, MPR121_FDLR, 0x00);

    // Falling filter (手指按住时 Baseline 跟踪)
    mpr_write_byte(addr, MPR121_MHDF, 0x01);
    mpr_write_byte(addr, MPR121_NHDF, 0x05);
    mpr_write_byte(addr, MPR121_NCLF, 0x01);
    mpr_write_byte(addr, MPR121_FDLF, 0x00);

    // Touch filter (触摸检测滤波)
    mpr_write_byte(addr, MPR121_NHDT, 0x00);
    mpr_write_byte(addr, MPR121_NCLT, 0x00);
    mpr_write_byte(addr, MPR121_FDLT, 0x00);

    // Debounce = 0 (无去抖，最低延迟)
    mpr_write_byte(addr, MPR121_DEBOUNCE, 0x00);

    // CONFIG1 和 CONFIG2
    mpr_write_byte(addr, MPR121_CONFIG1, 0x35);
    mpr_write_byte(addr, MPR121_CONFIG2, 0x02);

    // 不使用 Auto-Configuration
    // 让 Baseline 自然稳定即可

    // Enable electrodes - 12 electrodes
    // ECR: 0x80 (enable) | 12 (electrodes)
    mpr_write_byte(addr, MPR121_ECR, 0x8C);

    // 等待 Baseline 稳定
    sleep_ms(100);

    printf("OK (THR=%d/%d)\n", touch_thr, release_thr);
    return true;
}

void mpr121_init() {
    printf("[MPR121] Initializing I2C0...\n");

    // Initialize I2C0 (MPR121 is on I2C0)
    i2c_init(I2C0_PORT, 400 * 1000);
    gpio_set_function(I2C0_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C0_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C0_SDA);
    gpio_pull_up(I2C0_SCL);

    sleep_ms(10);

    // Initialize all MPR121 chips
    printf("[MPR121] Initializing sensors:\n");
    for (int i = 0; i < 3; i++) {
        mpr_ready[i] = mpr_init_single(mpr_addr[i], i);
    }

    int count = 0;
    for (int i = 0; i < 3; i++) {
        if (mpr_ready[i]) count++;
    }
    printf("[MPR121] %d/3 sensors initialized\n", count);

    // 等待所有传感器 Baseline 稳定
    if (count > 0) {
        printf("[MPR121] Waiting for baseline stabilization...\n");
        sleep_ms(500);  // 额外等待 500ms
        printf("[MPR121] Ready.\n");
    }
}

void mpr121_set_thresholds(uint8_t touch_thr, uint8_t release_thr) {
    // 确保 Touch > Release，至少差 2，避免迟滞区为 0
    if (touch_thr <= release_thr) {
        touch_thr = release_thr + 2;
    }

    for (int dev = 0; dev < 3; dev++) {
        if (!mpr_ready[dev]) continue;
        for (int ch = 0; ch < 12; ch++) {
            mpr_write_byte(mpr_addr[dev], MPR121_TOUCHTH_L + ch * 2, touch_thr);
            mpr_write_byte(mpr_addr[dev], MPR121_RELEASETH_L + ch * 2, release_thr);
        }
    }

    printf("[MPR121] Thresholds set: Touch=%d, Release=%d (hysteresis=%d)\n",
           touch_thr, release_thr, touch_thr - release_thr);
}

void mpr121_update() {
    for (int i = 0; i < 3; i++) {
        if (!mpr_ready[i]) {
            touch_state[i] = 0;
            continue;
        }

        uint8_t status[2] = {0, 0};
        uint8_t reg = MPR121_TOUCHSTATUS_L;

        int ret = i2c_write_blocking_until(I2C0_PORT, mpr_addr[i], &reg, 1, true,
                                            time_us_64() + 10000);
        if (ret < 0) {
            // I2C 写入失败，记录错误但保持旧状态
            i2c_error_count[i]++;
            continue;
        }

        ret = i2c_read_blocking_until(I2C0_PORT, mpr_addr[i], status, 2, false,
                                       time_us_64() + 10000);
        if (ret < 0) {
            // I2C 读取失败，记录错误但保持旧状态
            i2c_error_count[i]++;
            continue;
        }

        // 成功读取，更新状态
        touch_state[i] = status[0] | ((uint32_t)status[1] << 8);
    }
}

uint32_t mpr121_get_touch_state(uint8_t device) {
    if (device < 3) {
        return touch_state[device];
    }
    return 0;
}

bool mpr121_is_touched(uint8_t device, uint8_t channel) {
    if (device < 3 && channel < 12) {
        return (touch_state[device] >> channel) & 1;
    }
    return false;
}

uint32_t mpr121_get_error_count(uint8_t device) {
    if (device < 3) {
        return i2c_error_count[device];
    }
    return 0;
}

void mpr121_debug_print() {
    printf("\n========== MPR121 Debug ==========\n");

    // 显示 I2C 错误计数
    printf("I2C Errors: MPR1=%lu, MPR2=%lu, MPR3=%lu\n",
           i2c_error_count[0], i2c_error_count[1], i2c_error_count[2]);

    for (int dev = 0; dev < 3; dev++) {
        if (!mpr_ready[dev]) {
            printf("MPR%d: NOT READY\n", dev + 1);
            continue;
        }

        uint8_t addr = mpr_addr[dev];
        printf("\n--- MPR%d [0x%02X] ---\n", dev + 1, addr);

        // 读取并显示当前阈值设置
        uint8_t touch_th = 0, release_th = 0;
        mpr_read_byte(addr, MPR121_TOUCHTH_L, &touch_th);
        mpr_read_byte(addr, MPR121_RELEASETH_L, &release_th);
        printf("Thresholds: Touch=%d, Release=%d\n", touch_th, release_th);
        printf("CH  Baseline Filtered  Delta  Touch\n");

        for (int ch = 0; ch < 12; ch++) {
            // Each electrode: 4 bytes starting at 0x04 + ch*4
            uint8_t filtered_reg = MPR121_ELE0_FILTERED + ch * 4;
            uint8_t baseline_reg = MPR121_ELE0_BASELINE + ch * 4;

            uint16_t filtered_raw = 0, baseline_raw = 0;
            bool ok1 = mpr_read_word(addr, filtered_reg, &filtered_raw);
            bool ok2 = mpr_read_word(addr, baseline_reg, &baseline_raw);

            if (!ok1 || !ok2) {
                printf("%2d: READ ERROR\n", ch);
                continue;
            }

            // 尝试不进行位移，直接使用原始值
            // Baseline 和 FilteredData 可能就是原始 16-bit 值的低 10/12 位
            uint16_t baseline = baseline_raw & 0x03FF;  // 低 10-bit
            uint16_t filtered = filtered_raw & 0x0FFF;  // 低 12-bit

            // Delta = FilteredData - Baseline
            int16_t delta = (int16_t)filtered - (int16_t)baseline;
            bool touched = (touch_state[dev] >> ch) & 1;

            // 标记异常情况
            const char* mark = "";
            if (touched && delta < touch_th) {
                mark = " <-- STUCK!";
            } else if (!touched && delta > touch_th) {
                mark = " <-- MISS?";
            }

            printf("%2d:  %5d    %5d   %+5d   %d%s\n",
                   ch, baseline, filtered, delta, touched ? 1 : 0, mark);
        }
    }

    printf("\n==================================\n");
}

// Reset baseline for all MPR121 chips (soft reset)
void mpr121_reset_baseline() {
    printf("[MPR121] Resetting baseline...\n");
    for (int i = 0; i < 3; i++) {
        if (!mpr_ready[i]) continue;

        // Soft reset
        mpr_write_byte(mpr_addr[i], MPR121_SOFTRESET, 0x63);
        sleep_ms(2);

        // Re-initialize
        mpr_ready[i] = mpr_init_single(mpr_addr[i], i);

        // Clear touch state
        touch_state[i] = 0;
    }
    printf("[MPR121] Baseline reset complete\n");
}

} // namespace Chuni245Tof