/*
 * MPR121 Touch Controller Implementation
 * Uses I2C0 (SDA=16, SCL=17)
 *
 * ==============================================================================
 * MPR121 Touch Detection Architecture
 * ==============================================================================
 *
 * 【重要】Touch 判定由 MPR121 芯片内部硬件完成，不是 RP2040 根据 delta 软件判断。
 *
 * 完整链路：
 *   MPR121 Electrode E0~E11
 *       ↓ (电容变化)
 *   MPR121 Internal Hardware:
 *       - Filtered Data (12-bit, 0x04-0x1B)
 *       - Baseline (10-bit, 0x1E-0x35, E6+ 与滤波配置寄存器重叠，不可读)
 *       - Delta = Filtered - Baseline (有符号 16-bit)
 *       - Touch Threshold (寄存器 0x41-0x57, E0~E11)
 *       - Release Threshold (寄存器 0x42-0x58, E0~E11)
 *       ↓ (硬件比较)
 *   MPR121 Touch Status (寄存器 0x00-0x01, 12-bit bitmap)
 *       ↓ (I2C 读取)
 *   RP2040 touch_state[] (mpr121_update)
 *       ↓
 *   mpr121_is_touched() → slider/HID
 *
 * 【Touch/Release 阈值定义】
 *   - Touch Threshold: 当 delta < -touch_threshold 时触发 Touch
 *   - Release Threshold: 当 delta > -release_threshold 时触发 Release
 *   - 迟滞区：[-touch_threshold, -release_threshold]，避免频繁抖动
 *   - 要求：touch_threshold > release_threshold（至少差 2）
 *
 * 【关于 debug delta】
 *   delta = filtered - baseline，可以是正数或负数：
 *   - 负数：filtered < baseline，表示电容增加（手指靠近）
 *   - 正数：filtered > baseline，表示电容减少（手指离开）
 *   - 绝对值：变化幅度，不一定与 Touch 状态直接对应
 *
 * 【为什么 delta 绝对值可以很大但仍未触发 Touch？】
 *   1. Baseline 动态跟踪：MPR121 会根据环境变化调整 baseline
 *   2. Touch Threshold 是相对阈值：delta 必须低于 -touch_threshold 才触发
 *   3. 静态 delta 大不代表动态变化大：静态偏差会被 baseline 吸收
 *
 * 【不要根据 debug delta 软件判断 Touch】
 *   错误做法：if (delta < -touch_threshold) touched = true;
 *   正确做法：使用 MPR121_TOUCHSTATUS 寄存器（已由硬件完成判定）
 *
 * ==============================================================================
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

// Data registers for each electrode (PACKED layout, per MPR121 datasheet):
//   Filtered data: 0x04-0x1B  (12 electrodes × 2 bytes, packed)
//   Baseline data: 0x1E-0x35  (12 electrodes × 2 bytes, packed)
//
// IMPORTANT: Baseline addresses for ELE6+ (0x2A+) overlap with filter config
// registers (MHDR=0x2B, NHDR=0x2C, ...), so only ELE0-ELE5 baseline is
// reliably readable. ELE6-ELE11 baseline CANNOT be read back via I2C.
#define MPR121_ELE0_FILTERED  0x04
#define MPR121_ELE0_BASELINE  0x1E
#define MPR121_MAX_READABLE_BASELINE_CH  5  // E6+ baseline overlaps filter cfg

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
    // 使用 mpr121.h 中定义的默认阈值
    uint8_t touch_thr = cfg ? cfg->touch_threshold : MPR121_DEFAULT_TOUCH_THRESHOLD;
    uint8_t release_thr = cfg ? cfg->release_threshold : MPR121_DEFAULT_RELEASE_THRESHOLD;

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
    // MPR121 硬件要求：
    //   Touch:   delta < -touch_threshold 触发
    //   Release: delta > -release_threshold 释放
    // 因此 touch_threshold 必须大于 release_threshold，迟滞区大小 = touch - release
    if (touch_thr <= release_thr) {
        uint8_t old_touch = touch_thr;
        touch_thr = release_thr + 2;
        printf("[MPR121] WARNING: touch_threshold (%d) <= release_threshold (%d), auto-corrected to %d\r\n",
               old_touch, release_thr, touch_thr);

        // 同步修正 cfg，保证 Flash 中保存的值与实际写入的一致
        if (cfg) {
            cfg->touch_threshold = touch_thr;
            printf("[MPR121] cfg->touch_threshold updated to %d\r\n", touch_thr);
        }
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

        // ===== 读取并显示所有 E0~E11 的阈值寄存器 =====
        // 注意：阈值寄存器独立于 baseline/filtered 数据区域，
        // E0~E11 全部可读可写，不受 baseline 可读范围限制（仅 E0~E5 baseline 可读）
        printf("| CH | Touch TH | Release TH | Status |\n");
        printf("|----|----------|------------|--------|\n");
        for (int ch = 0; ch < 12; ch++) {
            uint8_t touch_th = 0, release_th = 0;
            mpr_read_byte(addr, MPR121_TOUCHTH_L + ch * 2, &touch_th);
            mpr_read_byte(addr, MPR121_RELEASETH_L + ch * 2, &release_th);
            bool touched = (touch_state[dev] >> ch) & 1;
            printf("| E%-2d| %-8d | %-10d | %-6s|\n",
                   ch, touch_th, release_th, touched ? "TOUCH" : "");
        }

        // ===== 显示 E0~E5 的 filtered/baseline/delta（仅 E0~E5 baseline 可读） =====
        printf("\nCH  Baseline Filtered  Delta  (E0~E5 only, E6+ baseline unreadable)\n");

        // E6+ baseline overlaps with filter config registers, unreadable.
        // Only show E0-E5 which have reliable filtered + baseline data.
        int max_ch = MPR121_MAX_READABLE_BASELINE_CH;
        for (int ch = 0; ch <= max_ch; ch++) {
            // Packed layout: filtered = 0x04 + ch*2, baseline = 0x1E + ch*2
            uint8_t filtered_reg = MPR121_ELE0_FILTERED + ch * 2;
            uint8_t baseline_reg = MPR121_ELE0_BASELINE + ch * 2;

            uint16_t filtered_raw = 0, baseline_raw = 0;
            bool ok1 = mpr_read_word(addr, filtered_reg, &filtered_raw);
            bool ok2 = mpr_read_word(addr, baseline_reg, &baseline_raw);

            if (!ok1 || !ok2) {
                printf("%2d: READ ERROR\n", ch);
                continue;
            }

            uint16_t baseline = baseline_raw & 0x03FF;  // 10-bit
            uint16_t filtered = filtered_raw & 0x0FFF;  // 12-bit

            // Delta = FilteredData - Baseline
            // 重要：MPR121 硬件内部 Touch 判定基于此 delta 值：
            //   Touch:   delta < -touch_threshold (filtered < baseline - touch_threshold)
            //   Release: delta > -release_threshold (filtered > baseline - release_threshold)
            // 注意：debug delta 只是观测值，实际 Touch 状态来自 MPR121_TOUCHSTATUS 寄存器。
            // 不要直接用 delta 与 touch_threshold 比较来判断 Touch 状态！
            int16_t delta = (int16_t)filtered - (int16_t)baseline;
            bool touched = (touch_state[dev] >> ch) & 1;

            printf("%2d:  %5d    %5d   %+5d   %d\n",
                   ch, baseline, filtered, delta, touched ? 1 : 0);
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

// ============================================================================
// MPR121 Debug System
// ============================================================================
//
// Design:
//   - Cycling I2C reads: one (dev, ch) pair per MPR121_DEBUG_SAMPLE_DIVIDER
//     loop iterations. This keeps per-loop overhead to ~2 I2C word reads
//     instead of reading all electrodes at once.
//   - Event detection uses touch_state (already read every loop for free).
//     On state transition, filtered/baseline is read on-demand for that ch.
//   - Stats table printed every MPR121_DEBUG_STAT_INTERVAL_MS.
//   - All code compiles to nothing when DEBUG_MPR121 == 0.
//
// Delta formula (matches existing mpr121_debug_print):
//   delta = (int16_t)(filtered & 0x0FFF) - (int16_t)(baseline & 0x03FF)
//   Negative = finger present (filtered < baseline).
// ============================================================================

#if DEBUG_MPR121

// Clamp electrode range to valid bounds.
// Max readable baseline channel is MPR121_MAX_READABLE_BASELINE_CH (5),
// because E6+ baseline addresses overlap with filter config registers.
#if MPR121_DEBUG_FIRST_ELECTRODE < 0
#undef MPR121_DEBUG_FIRST_ELECTRODE
#define MPR121_DEBUG_FIRST_ELECTRODE 0
#endif
#if MPR121_DEBUG_LAST_ELECTRODE > MPR121_MAX_READABLE_BASELINE_CH
#undef MPR121_DEBUG_LAST_ELECTRODE
#define MPR121_DEBUG_LAST_ELECTRODE MPR121_MAX_READABLE_BASELINE_CH
#endif
#if MPR121_DEBUG_SAMPLE_DIVIDER < 1
#undef MPR121_DEBUG_SAMPLE_DIVIDER
#define MPR121_DEBUG_SAMPLE_DIVIDER 1
#endif

struct mpr121_dbg_ch_t {
    uint16_t filtered;
    uint16_t baseline;
    int16_t  delta;
    // min/max accumulators (reset after each stats print)
    uint16_t filt_min;
    uint16_t filt_max;
    int16_t  delta_min;
    int16_t  delta_max;
    bool     has_sample;  // true if at least one sample taken since reset
};

static struct mpr121_dbg_ch_t dbg_ch[3][12];
static uint32_t dbg_prev_touch[3] = {0, 0, 0};
static uint64_t dbg_stat_last_us  = 0;
static uint32_t dbg_loop_ctr      = 0;
// Cycling read position
static uint8_t  dbg_read_dev = 0;
static uint8_t  dbg_read_ch  = MPR121_DEBUG_FIRST_ELECTRODE;

// Read filtered + baseline for one (dev, ch) pair and update accumulators.
// Uses the SAME register addresses and delta formula as mpr121_debug_print().
// Packed layout: filtered = 0x04 + ch*2, baseline = 0x1E + ch*2.
// ch must be <= MPR121_MAX_READABLE_BASELINE_CH (5), since E6+ baseline
// addresses overlap with filter config registers.
static void dbg_sample_channel(uint8_t dev, uint8_t ch) {
    if (!mpr_ready[dev]) return;
    if (ch > MPR121_MAX_READABLE_BASELINE_CH) return;
    uint8_t addr = mpr_addr[dev];
    uint8_t filt_reg = MPR121_ELE0_FILTERED + ch * 2;
    uint8_t base_reg = MPR121_ELE0_BASELINE + ch * 2;

    uint16_t filt_raw = 0, base_raw = 0;
    bool ok1 = mpr_read_word(addr, filt_reg, &filt_raw);
    bool ok2 = mpr_read_word(addr, base_reg, &base_raw);
    if (!ok1 || !ok2) return;

    uint16_t filtered = filt_raw & 0x0FFF;   // 12-bit
    uint16_t baseline = base_raw & 0x03FF;   // 10-bit
    int16_t  delta    = (int16_t)filtered - (int16_t)baseline;

    struct mpr121_dbg_ch_t *c = &dbg_ch[dev][ch];
    c->filtered = filtered;
    c->baseline = baseline;
    c->delta    = delta;

    if (!c->has_sample) {
        c->filt_min  = filtered;
        c->filt_max  = filtered;
        c->delta_min = delta;
        c->delta_max = delta;
        c->has_sample = true;
    } else {
        if (filtered < c->filt_min) c->filt_min = filtered;
        if (filtered > c->filt_max) c->filt_max = filtered;
        if (delta < c->delta_min)   c->delta_min = delta;
        if (delta > c->delta_max)   c->delta_max = delta;
    }
}

// Reset min/max accumulators for all monitored channels
static void dbg_reset_stats() {
    for (int dev = 0; dev < 3; dev++) {
        for (int ch = 0; ch < 12; ch++) {
            dbg_ch[dev][ch].filt_min   = UINT16_MAX;
            dbg_ch[dev][ch].filt_max   = 0;
            dbg_ch[dev][ch].delta_min  = INT16_MAX;
            dbg_ch[dev][ch].delta_max  = INT16_MIN;
            dbg_ch[dev][ch].has_sample = false;
        }
    }
}

// Advance cycling read position to next (dev, ch) in range
static void dbg_advance_pos() {
    dbg_read_ch++;
    if (dbg_read_ch > MPR121_DEBUG_LAST_ELECTRODE) {
        dbg_read_ch = MPR121_DEBUG_FIRST_ELECTRODE;
        dbg_read_dev++;
        if (dbg_read_dev >= 3) {
            dbg_read_dev = 0;
        }
    }
}

// Print the header + column labels for a device table
static void dbg_print_table_header(int dev) {
    int first = MPR121_DEBUG_FIRST_ELECTRODE;
    int last  = MPR121_DEBUG_LAST_ELECTRODE;

    printf("| Param      |");
    for (int ch = first; ch <= last; ch++) {
        printf(" E%-3d|", ch);
    }
    printf("\r\n|------------|");
    for (int ch = first; ch <= last; ch++) {
        (void)ch;
        printf("------|");
    }
    printf("\r\n");
}

// ---- Public API ----

void mpr121_debug_init() {
    printf("\r\n");
    printf("[MPR121 CONFIG] =================================================\r\n");

    for (int dev = 0; dev < 3; dev++) {
        if (!mpr_ready[dev]) {
            printf("\r\nMPR%d [0x%02X]: NOT READY\r\n", dev + 1, mpr_addr[dev]);
            continue;
        }
        uint8_t addr = mpr_addr[dev];

        // Read back global config registers (0x5B-0x5E, safely above electrode data)
        uint8_t config1 = 0, config2 = 0, ecr = 0, debounce = 0;
        mpr_read_byte(addr, MPR121_CONFIG1, &config1);
        mpr_read_byte(addr, MPR121_CONFIG2, &config2);
        mpr_read_byte(addr, MPR121_ECR,     &ecr);
        mpr_read_byte(addr, MPR121_DEBOUNCE,&debounce);

        printf("\r\n--- MPR%d [0x%02X] ---\r\n", dev + 1, addr);
        printf("CONFIG1=0x%02X  CONFIG2=0x%02X  ECR=0x%02X  DEBOUNCE=0x%02X\r\n",
               config1, config2, ecr, debounce);

        // Filter configuration values are the ones written during init.
        // We print known values because reading back 0x2B-0x35 would return
        // filter config, not electrode data (addresses overlap).
        printf("Filter (Rising) : MHDR=0x01 NHDR=0x01 NCLR=0x0E FDLR=0x00\r\n");
        printf("Filter (Falling): MHDF=0x01 NHDF=0x05 NCLF=0x01 FDLF=0x00\r\n");
        printf("Filter (Touched): NHDT=0x00 NCLT=0x00 FDLT=0x00\r\n");

        // ===== 显示所有 E0~E11 的阈值寄存器（不受 baseline 可读范围限制） =====
        // 阈值寄存器地址：Touch=0x41-0x57, Release=0x42-0x58（每电极 2 字节，交错）
        // 所有 E0~E11 的阈值寄存器都可读可写，与 baseline 数据区域不同
        printf("\r\nThreshold Registers (E0~E11, all readable):\r\n");
        printf("| CH | Touch | Release |\r\n");
        printf("|----|-------|---------|\r\n");
        for (int ch = 0; ch < 12; ch++) {
            uint8_t touch_th = 0, release_th = 0;
            mpr_read_byte(addr, MPR121_TOUCHTH_L + ch * 2, &touch_th);
            mpr_read_byte(addr, MPR121_RELEASETH_L + ch * 2, &release_th);
            printf("| E%-2d| %-5d | %-7d |\r\n", ch, touch_th, release_th);
        }
    }

    printf("\r\n");
    printf("[MPR121 CONFIG] Note: Baseline data only readable for E0~E5\r\n");
    printf("[MPR121 CONFIG]       (E6+ baseline addresses overlap with filter config)\r\n");
    printf("[MPR121 CONFIG] Threshold registers are readable for all E0~E11\r\n");
    printf("[MPR121 CONFIG] Debug monitoring range: E%d-E%d (filtered/baseline/delta)\r\n",
           MPR121_DEBUG_FIRST_ELECTRODE, MPR121_DEBUG_LAST_ELECTRODE);
    printf("[MPR121 CONFIG] Stat interval: %d ms, Sample divider: %d\r\n",
           MPR121_DEBUG_STAT_INTERVAL_MS, MPR121_DEBUG_SAMPLE_DIVIDER);
    printf("[MPR121 CONFIG] delta = filtered - baseline (negative = touched)\r\n");
    printf("[MPR121 CONFIG] =================================================\r\n\r\n");

    dbg_reset_stats();
    dbg_stat_last_us = time_us_64();

    // Initialize prev_touch to current state so we don't print spurious events
    for (int i = 0; i < 3; i++) {
        dbg_prev_touch[i] = touch_state[i];
    }
}

void mpr121_debug_tick() {
    // ---- 1. Sample divider: throttle I2C reads ----
    dbg_loop_ctr++;
    bool do_sample = (dbg_loop_ctr % MPR121_DEBUG_SAMPLE_DIVIDER) == 0;

    // ---- 2. Event detection (uses touch_state, already read by mpr121_update) ----
    for (int dev = 0; dev < 3; dev++) {
        if (!mpr_ready[dev]) continue;
        uint32_t changed = touch_state[dev] ^ dbg_prev_touch[dev];
        if (!changed) continue;

        for (int ch = MPR121_DEBUG_FIRST_ELECTRODE;
             ch <= MPR121_DEBUG_LAST_ELECTRODE; ch++) {
            if (!(changed & (1u << ch))) continue;

            bool now_touched = (touch_state[dev] >> ch) & 1u;

            // Read current filtered/baseline for this electrode
            dbg_sample_channel(dev, ch);

            struct mpr121_dbg_ch_t *c = &dbg_ch[dev][ch];
            printf("[MPR121 EVENT] MPR%d-E%-2d %-7s filtered=%-5u baseline=%-5u delta=%+6d\r\n",
                   dev + 1, ch,
                   now_touched ? "TOUCH" : "RELEASE",
                   c->filtered, c->baseline, c->delta);
        }
        dbg_prev_touch[dev] = touch_state[dev];
    }

    // ---- 3. Cycling read for stats accumulation ----
    if (do_sample) {
        // Skip devices not in range or not ready
        int attempts = 0;
        while (attempts < 36) {  // at most 36 channels to cycle through
            if (mpr_ready[dbg_read_dev] &&
                dbg_read_ch >= MPR121_DEBUG_FIRST_ELECTRODE &&
                dbg_read_ch <= MPR121_DEBUG_LAST_ELECTRODE) {
                dbg_sample_channel(dbg_read_dev, dbg_read_ch);
                dbg_advance_pos();
                break;
            }
            dbg_advance_pos();
            attempts++;
        }
    }

    // ---- 4. Periodic stats output ----
    uint64_t now_us = time_us_64();
    uint64_t elapsed_ms = (now_us - dbg_stat_last_us) / 1000u;
    if (elapsed_ms < MPR121_DEBUG_STAT_INTERVAL_MS) return;

    uint32_t t_ms = (uint32_t)(now_us / 1000u);

    printf("\r\n[MPR121 STAT t=%lums]\r\n", (unsigned long)t_ms);

    for (int dev = 0; dev < 3; dev++) {
        if (!mpr_ready[dev]) continue;

        int first = MPR121_DEBUG_FIRST_ELECTRODE;
        int last  = MPR121_DEBUG_LAST_ELECTRODE;

        printf("--- MPR%d [0x%02X] ---\r\n", dev + 1, mpr_addr[dev]);
        dbg_print_table_header(dev);

        // filtered row
        printf("| filtered  |");
        for (int ch = first; ch <= last; ch++) {
            printf(" %-5u|", dbg_ch[dev][ch].filtered);
        }
        printf("\r\n");

        // baseline row
        printf("| baseline  |");
        for (int ch = first; ch <= last; ch++) {
            printf(" %-5u|", dbg_ch[dev][ch].baseline);
        }
        printf("\r\n");

        // delta row
        printf("| delta     |");
        for (int ch = first; ch <= last; ch++) {
            printf(" %+5d|", dbg_ch[dev][ch].delta);
        }
        printf("\r\n");

        // filt_min row
        printf("| filt_min  |");
        for (int ch = first; ch <= last; ch++) {
            struct mpr121_dbg_ch_t *c = &dbg_ch[dev][ch];
            if (c->has_sample) printf(" %-5u|", c->filt_min);
            else               printf("     -|");
        }
        printf("\r\n");

        // filt_max row
        printf("| filt_max  |");
        for (int ch = first; ch <= last; ch++) {
            struct mpr121_dbg_ch_t *c = &dbg_ch[dev][ch];
            if (c->has_sample) printf(" %-5u|", c->filt_max);
            else               printf("     -|");
        }
        printf("\r\n");

        // delta_min row
        printf("| delta_min |");
        for (int ch = first; ch <= last; ch++) {
            struct mpr121_dbg_ch_t *c = &dbg_ch[dev][ch];
            if (c->has_sample) printf(" %+5d|", c->delta_min);
            else               printf("     -|");
        }
        printf("\r\n");

        // delta_max row
        printf("| delta_max |");
        for (int ch = first; ch <= last; ch++) {
            struct mpr121_dbg_ch_t *c = &dbg_ch[dev][ch];
            if (c->has_sample) printf(" %+5d|", c->delta_max);
            else               printf("     -|");
        }
        printf("\r\n");

        // touch_th row (read once from chip, doesn't change)
        printf("| touch_th  |");
        for (int ch = first; ch <= last; ch++) {
            uint8_t thr = 0;
            mpr_read_byte(mpr_addr[dev], MPR121_TOUCHTH_L + ch * 2, &thr);
            printf(" %-5u|", thr);
        }
        printf("\r\n");

        // release_th row
        printf("| release_th|");
        for (int ch = first; ch <= last; ch++) {
            uint8_t thr = 0;
            mpr_read_byte(mpr_addr[dev], MPR121_RELEASETH_L + ch * 2, &thr);
            printf(" %-5u|", thr);
        }
        printf("\r\n");

        // state row (from touch_state)
        printf("| state     |");
        for (int ch = first; ch <= last; ch++) {
            printf(" %-5d|", (int)((touch_state[dev] >> ch) & 1u));
        }
        printf("\r\n");
    }

    printf("\r\n");

    // Reset accumulators for next window
    dbg_reset_stats();
    dbg_stat_last_us = now_us;
}

#endif /* DEBUG_MPR121 */

} // namespace Chuni245Tof