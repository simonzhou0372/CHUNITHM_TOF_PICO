/*
 * VL53L0X TOF Sensor Driver
 *
 * 基于官方 Pololu 参考实现 (.ref/VL53L0X.cpp) 的完整移植:
 *   - 完整初始化序列（含 tuning settings / SPAD 管理 / 参考校准）
 *   - TimingBudget 真实生效: 统一常量 VL53L0X_TIMING_BUDGET_US = 20000us,
 *     通过 setMeasurementTimingBudget 写入 PRE/FINAL RANGE timeout 寄存器 (0x51/0x71),
 *     并在初始化末尾读回重算验证 (±200us 容差)
 *   - I2C、初始化与恢复均为协作式状态机，所有寄存器写检查返回值
 *   - 中断清除失败视为通信错误 (防止"旧数据标新")
 *
 * I2C1 总线由 Core1 独占 —— 本文件所有函数只能在 Core1 上调用。
 */

#include "vl53l0x.h"
#include "board_defs.h"
#include "tof_events.h"

#include "pico/time.h"
#include "hardware/i2c.h"
#include "i2c_async.h"
#include "cooperative_task.h"
#include "hardware/gpio.h"

#include <string.h>

namespace Chuni245Tof {

//==============================================================================
// 统一配置常量（唯一配置源）
//==============================================================================

// TimingBudget: 20ms = ST 官方最小值, 兼顾响应速度与测距质量
// 所有路径（冷启动 / 单传感器恢复 / 全量恢复）使用同一常量
constexpr uint32_t VL53L0X_TIMING_BUDGET_US = 20000u;

// TimingBudget 读回验证容差 (us):
// 预算按 MCLK 粒度量化（每 MCLK ≈ 38us）, 加上舍入误差, 200us 足够覆盖
constexpr int32_t TIMING_BUDGET_VERIFY_TOLERANCE_US = 200;

// 信号率下限: 0.25 MCPS, Q9.7 定点 (0.25 * 128 = 32)
constexpr uint16_t VL53L0X_SIGNAL_RATE_LIMIT_Q97 = 32;

// I2C
constexpr uint32_t I2C1_FREQ_HZ = 400000u;
constexpr uint32_t I2C_TRANSACTION_TIMEOUT_US = 3000u;

// XSHUT 时序
constexpr uint32_t XSHUT_RESET_DELAY_MS = 10u;    // XSHUT 拉低复位时间
constexpr uint32_t SENSOR_BOOT_DELAY_MS = 10u;    // 释放 XSHUT 后传感器启动时间

// VL53L0X 默认 I2C 地址 (上电/XSHUT 复位后)
constexpr uint8_t VL53L0X_DEFAULT_ADDR = 0x29u;

//==============================================================================
// 寄存器地址 (ST vl53l0x_device.h, 与 .ref/VL53L0X.h 一致)
//==============================================================================

constexpr uint8_t REG_SYSRANGE_START                        = 0x00;
constexpr uint8_t REG_SYSTEM_SEQUENCE_CONFIG                = 0x01;
constexpr uint8_t REG_SYSTEM_INTERRUPT_CONFIG_GPIO          = 0x0A;
constexpr uint8_t REG_SYSTEM_INTERRUPT_CLEAR                = 0x0B;
constexpr uint8_t REG_RESULT_INTERRUPT_STATUS               = 0x13;
constexpr uint8_t REG_RESULT_RANGE_STATUS                   = 0x14;  // 距离在 0x14+10 = 0x1E
constexpr uint8_t REG_GLOBAL_CONFIG_SPAD_ENABLES_REF_0      = 0xB0;
constexpr uint8_t REG_GLOBAL_CONFIG_REF_EN_START_SELECT     = 0xB6;
constexpr uint8_t REG_DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD   = 0x4E;
constexpr uint8_t REG_DYNAMIC_SPAD_REF_EN_START_OFFSET      = 0x4F;
constexpr uint8_t REG_POWER_MANAGEMENT_GO1_POWER_FORCE      = 0x80;
constexpr uint8_t REG_VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HV      = 0x89;
constexpr uint8_t REG_I2C_SLAVE_DEVICE_ADDRESS              = 0x8A;
constexpr uint8_t REG_MSRC_CONFIG_CONTROL                   = 0x60;
constexpr uint8_t REG_FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT = 0x44;
constexpr uint8_t REG_PRE_RANGE_CONFIG_VCSEL_PERIOD         = 0x50;
constexpr uint8_t REG_PRE_RANGE_CONFIG_TIMEOUT_MACROP       = 0x51;  // 16bit (0x51-0x52)
constexpr uint8_t REG_FINAL_RANGE_CONFIG_VCSEL_PERIOD       = 0x70;
constexpr uint8_t REG_FINAL_RANGE_CONFIG_TIMEOUT_MACROP     = 0x71;  // 16bit (0x71-0x72)
constexpr uint8_t REG_MSRC_CONFIG_TIMEOUT_MACROP            = 0x46;
constexpr uint8_t REG_IDENTIFICATION_MODEL_ID               = 0xC0;
constexpr uint8_t REG_GPIO_HV_MUX_ACTIVE_HIGH               = 0x84;

//==============================================================================
// Sensor 状态
//==============================================================================

// XSHUT 引脚 (TOF1..TOF5)
static const uint8_t xshut_pins[5] = {
    TOF1_XSHUT, TOF2_XSHUT, TOF3_XSHUT, TOF4_XSHUT, TOF5_XSHUT
};

// 分配后的传感器地址
static uint8_t sensor_addr[5] = { 0x30, 0x31, 0x32, 0x33, 0x34 };

// 初始化完成标志（false = 不轮询, 不产生任何 I2C 流量）
static volatile bool sensor_ready[5] = { false };

// 健康覆盖标志（恢复状态机设置）
enum health_override_t {
    HO_NONE = 0,
    HO_RECOVERING,
    HO_OFFLINE,
};
static volatile health_override_t health_override[5] = { HO_NONE };

// startContinuous 前导码所需的 stop variable（DSS 模式必需）
static uint8_t stop_variable[5] = { 0 };

// 统计
static volatile uint32_t i2c_error_count[5] = { 0 };
static volatile uint32_t consecutive_errors[5] = { 0 };
static volatile uint32_t last_new_data_ms[5] = { 0 };
static volatile uint32_t last_comm_ok_ms[5] = { 0 };
static volatile uint32_t recovery_count[5] = { 0 };

// 最后一次有效距离（按传感器缓存）
static uint16_t last_distance[5] = { 0 };

//==============================================================================
// 固定内存的协作式任务（每次 task_step 只推进一个叶任务）
//==============================================================================

// 初始化/恢复与采样交错执行；等待 I2C 或时限时立即返回 Core1 调度器。
static AsyncI2c tof_bus(I2C1_PORT);
static bool io_claimed = false;
static bool address_conflict = false; // Core1 only; cleared by full readdressing

static Task<bool> delay_us(uint64_t duration) {
    const uint64_t end = time_us_64() + duration;
    while (time_us_64() < end) co_await std::suspend_always{};
    co_return true;
}

static Task<bool> transfer(uint8_t addr, const uint8_t* tx, size_t txlen,
                           uint8_t* rx, size_t rxlen) {
    while (io_claimed || tof_bus.busy()) co_await std::suspend_always{};
    io_claimed = true;
    bool started = tof_bus.start(addr, tx, txlen, rxlen, I2C_TRANSACTION_TIMEOUT_US);
    while (tof_bus.busy()) co_await std::suspend_always{};
    bool ok = started && tof_bus.result() == (int)(txlen + rxlen);
    if (ok && rxlen) memcpy(rx, tof_bus.data(), rxlen);
    io_claimed = false;
    co_return ok;
}
static Task<bool> write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    co_return co_await transfer(addr, buf, 2, nullptr, 0);
}
static Task<bool> read_reg(uint8_t addr, uint8_t reg, uint8_t* val) {
    co_return co_await transfer(addr, &reg, 1, val, 1);
}
static Task<bool> read_reg16(uint8_t addr, uint8_t reg, uint16_t* val) {
    uint8_t buf[2]{};
    if (!(co_await transfer(addr, &reg, 1, buf, 2))) co_return false;
    *val = ((uint16_t)buf[0] << 8) | buf[1];
    co_return true;
}
static Task<bool> write_reg16(uint8_t addr, uint8_t reg, uint16_t val) {
    uint8_t buf[3] = {reg, (uint8_t)(val >> 8), (uint8_t)val};
    co_return co_await transfer(addr, buf, 3, nullptr, 0);
}
static Task<bool> write_multi(uint8_t addr, uint8_t reg, const uint8_t* src, uint8_t len) {
    uint8_t buf[8];
    if (len > sizeof(buf) - 1) co_return false;
    buf[0] = reg;
    memcpy(buf + 1, src, len);
    co_return co_await transfer(addr, buf, len + 1, nullptr, 0);
}
static Task<bool> read_multi(uint8_t addr, uint8_t reg, uint8_t* dst, uint8_t len) {
    co_return co_await transfer(addr, &reg, 1, dst, len);
}

// 记录通信错误
static void record_comm_error(uint8_t index)
{
    i2c_error_count[index] = i2c_error_count[index] + 1;
    consecutive_errors[index] = consecutive_errors[index] + 1;
}

// 记录一次成功的通信（任何寄存器读成功都证明 总线+传感器 仍可访问）
// 与 last_new_data_ms 分开: 前者度量 "通信层存活", 后者度量 "测距流水线产出"
static inline void stamp_comm_ok(uint8_t index)
{
    last_comm_ok_ms[index] = to_ms_since_boot(get_absolute_time());
}

// 检查传感器在指定地址响应且型号正确 (MODEL_ID = 0xEE)
static Task<bool> check_model_id(uint8_t addr)
{
    uint8_t model = 0;
    if (!(co_await read_reg(addr, REG_IDENTIFICATION_MODEL_ID, &model))) co_return false;
    co_return model == 0xEE;
}

//==============================================================================
// Timing 计算 —— 逐行移植自 Pololu VL53L0X.cpp（公式不可改动）
//==============================================================================

// decodeVcselPeriod: VCSEL 周期寄存器 → PCLK 周期数
static inline uint16_t decode_vcsel_period(uint8_t reg)
{
    return (uint16_t)((reg + 1) << 1);
}

// calcMacroPeriod: PCLK 数 → 一个 macro period 的纳秒数
// (2304 * pclks * 1655 + 500) / 1000 —— Pololu 原始公式
static inline uint32_t calc_macro_period_ns(uint16_t vcsel_period_pclks)
{
    return ((uint64_t)2304 * vcsel_period_pclks * 1655 + 500) / 1000;
}

// decodeTimeout: 16bit timeout 寄存器值 → MCLK 数
static inline uint32_t decode_timeout(uint16_t reg_val)
{
    const uint32_t exponent = reg_val >> 8;
    if (exponent >= 24) return 0;
    return (((uint32_t)reg_val & 0xFF) << exponent) + 1;
}

// encodeTimeout: MCLK 数 → 16bit timeout 寄存器值
static inline uint16_t encode_timeout(uint32_t timeout_mclks)
{
    uint32_t ls_byte = 0;
    uint16_t ms_byte = 0;

    ls_byte = timeout_mclks - 1;
    while (ls_byte & 0xFFFFFF00) {
        ls_byte >>= 1;
        ms_byte++;
    }
    return (ms_byte << 8) | (ls_byte & 0xFF);
}

// MCLK 数 → 微秒
static inline uint32_t timeout_mclks_to_us(uint32_t timeout_mclks, uint16_t vcsel_period_pclks)
{
    uint32_t macro_period_ns = calc_macro_period_ns(vcsel_period_pclks);
    return (((uint64_t)timeout_mclks * macro_period_ns) + 500) / 1000;
}

// 微秒 → MCLK 数
static inline uint32_t timeout_us_to_mclks(uint32_t timeout_us, uint16_t vcsel_period_pclks)
{
    uint32_t macro_period_ns = calc_macro_period_ns(vcsel_period_pclks);
    return (((uint64_t)timeout_us * 1000) + (macro_period_ns / 2)) / macro_period_ns;
}

//==============================================================================
// Sequence step enables / timeouts（移植自 Pololu）
//==============================================================================

struct seq_step_enables_t {
    bool tcc, msrc, dss, pre_range, final_range;
};

struct seq_step_timeouts_t {
    uint16_t pre_range_vcsel_period_pclks;
    uint16_t final_range_vcsel_period_pclks;

    uint16_t msrc_dss_tcc_mclks;
    uint32_t pre_range_mclks;
    uint32_t final_range_mclks;

    uint32_t msrc_dss_tcc_us;
    uint32_t pre_range_us;
    uint32_t final_range_us;
};

static Task<bool> get_sequence_step_enables(uint8_t addr, seq_step_enables_t *enables)
{
    uint8_t seq_config;
    if (!(co_await read_reg(addr, REG_SYSTEM_SEQUENCE_CONFIG, &seq_config))) co_return false;

    enables->tcc         = (seq_config >> 4) & 0x1;
    enables->msrc        = (seq_config >> 2) & 0x1;
    enables->dss         = (seq_config >> 3) & 0x1;
    enables->pre_range   = (seq_config >> 6) & 0x1;
    enables->final_range = (seq_config >> 7) & 0x1;
    co_return true;
}

static Task<bool> get_sequence_step_timeouts(uint8_t addr,
                                       const seq_step_enables_t *enables,
                                       seq_step_timeouts_t *timeouts)
{
    uint16_t reg16;
    uint8_t reg8;

    // pre_range VCSEL 周期 (寄存器 0x50)
    if (!(co_await read_reg(addr, REG_PRE_RANGE_CONFIG_VCSEL_PERIOD, &reg8))) co_return false;
    timeouts->pre_range_vcsel_period_pclks = decode_vcsel_period(reg8);

    // MSRC/DSS/TCC timeout (8bit 寄存器 0x46: 值+1 = MCLK 数)
    if (!(co_await read_reg(addr, REG_MSRC_CONFIG_TIMEOUT_MACROP, &reg8))) co_return false;
    timeouts->msrc_dss_tcc_mclks = (uint16_t)reg8 + 1;
    timeouts->msrc_dss_tcc_us = timeout_mclks_to_us(timeouts->msrc_dss_tcc_mclks,
                                                    timeouts->pre_range_vcsel_period_pclks);

    // pre_range timeout (16bit 寄存器 0x51)
    if (!(co_await read_reg16(addr, REG_PRE_RANGE_CONFIG_TIMEOUT_MACROP, &reg16))) co_return false;
    timeouts->pre_range_mclks = decode_timeout(reg16);
    timeouts->pre_range_us = timeout_mclks_to_us(timeouts->pre_range_mclks,
                                                 timeouts->pre_range_vcsel_period_pclks);

    // final_range VCSEL 周期 (寄存器 0x70)
    if (!(co_await read_reg(addr, REG_FINAL_RANGE_CONFIG_VCSEL_PERIOD, &reg8))) co_return false;
    timeouts->final_range_vcsel_period_pclks = decode_vcsel_period(reg8);

    // final_range timeout (16bit 寄存器 0x71):
    // 寄存器统计的是 pre+final 的总 MCLK 数, 读回时必须扣除 pre 部分
    if (!(co_await read_reg16(addr, REG_FINAL_RANGE_CONFIG_TIMEOUT_MACROP, &reg16))) co_return false;
    timeouts->final_range_mclks = decode_timeout(reg16);
    if (enables->pre_range) {
        timeouts->final_range_mclks -= timeouts->pre_range_mclks;
    }
    timeouts->final_range_us = timeout_mclks_to_us(timeouts->final_range_mclks,
                                                   timeouts->final_range_vcsel_period_pclks);

    co_return true;
}

//==============================================================================
// TimingBudget 设置（移植自 Pololu setMeasurementTimingBudget）
// 返回 false = budget 无法在此配置下实现（例如过小）
//==============================================================================

// 各序列步骤开销 (us) —— ST API 官方值
constexpr uint32_t OVERHEAD_START_OVERRHEAD_US      = 1910;
constexpr uint32_t OVERHEAD_END_OVERRHEAD_US        = 960;
constexpr uint32_t OVERHEAD_MSRC_OVERRHEAD_US       = 660;
constexpr uint32_t OVERHEAD_TCC_OVERRHEAD_US        = 590;
constexpr uint32_t OVERHEAD_DSS_OVERRHEAD_US        = 690;
constexpr uint32_t OVERHEAD_PRE_RANGE_OVERRHEAD_US  = 660;
constexpr uint32_t OVERHEAD_FINAL_RANGE_OVERRHEAD_US = 550;

static Task<bool> set_measurement_timing_budget(uint8_t addr, uint32_t budget_us)
{
    seq_step_enables_t enables;
    seq_step_timeouts_t timeouts;

    if (!(co_await get_sequence_step_enables(addr, &enables))) co_return false;
    if (!(co_await get_sequence_step_timeouts(addr, &enables, &timeouts))) co_return false;

    const uint32_t min_budget = OVERHEAD_START_OVERRHEAD_US + OVERHEAD_END_OVERRHEAD_US;
    if (budget_us < min_budget) co_return false;

    uint32_t used_budget_us = min_budget;

    if (enables.tcc) {
        used_budget_us += (OVERHEAD_TCC_OVERRHEAD_US + timeouts.msrc_dss_tcc_us);
    }
    if (enables.dss) {
        used_budget_us += 2 * (OVERHEAD_DSS_OVERRHEAD_US + timeouts.msrc_dss_tcc_us); // DSS 重复两次
    } else if (enables.msrc) {
        used_budget_us += (OVERHEAD_MSRC_OVERRHEAD_US + timeouts.msrc_dss_tcc_us);
    }
    if (enables.pre_range) {
        used_budget_us += (OVERHEAD_PRE_RANGE_OVERRHEAD_US + timeouts.pre_range_us);
    }
    if (enables.final_range) {
        used_budget_us += OVERHEAD_FINAL_RANGE_OVERRHEAD_US;

        if (used_budget_us > budget_us) co_return false;

        uint32_t final_range_timeout_us = budget_us - used_budget_us;

        // 剩余预算直接转为 final_range MCLK, 再加上 pre_range MCLK:
        // (FINAL_RANGE timeout 寄存器统计的是 pre+final 的总 MCLK 数,
        //  两者 VCSEL 周期不同, 所以只能在 MCLK 域相加 —— Pololu 原始逻辑)
        uint32_t final_range_timeout_mclks =
            timeout_us_to_mclks(final_range_timeout_us, timeouts.final_range_vcsel_period_pclks);

        if (enables.pre_range) {
            final_range_timeout_mclks += timeouts.pre_range_mclks;
        }

        if (!(co_await write_reg16(addr, REG_FINAL_RANGE_CONFIG_TIMEOUT_MACROP,
                         encode_timeout(final_range_timeout_mclks)))) {
            co_return false;
        }
    }

    co_return true;
}

//==============================================================================
// TimingBudget 读回验证: 从寄存器重算实际 budget, 与目标值比对
//==============================================================================

static Task<bool> get_measurement_timing_budget(uint8_t addr, uint32_t *budget_us)
{
    seq_step_enables_t enables;
    seq_step_timeouts_t timeouts;

    if (!(co_await get_sequence_step_enables(addr, &enables))) co_return false;
    if (!(co_await get_sequence_step_timeouts(addr, &enables, &timeouts))) co_return false;

    uint32_t used_budget_us = OVERHEAD_START_OVERRHEAD_US + OVERHEAD_END_OVERRHEAD_US;

    if (enables.tcc) {
        used_budget_us += (OVERHEAD_TCC_OVERRHEAD_US + timeouts.msrc_dss_tcc_us);
    }
    if (enables.dss) {
        used_budget_us += 2 * (OVERHEAD_DSS_OVERRHEAD_US + timeouts.msrc_dss_tcc_us);
    } else if (enables.msrc) {
        used_budget_us += (OVERHEAD_MSRC_OVERRHEAD_US + timeouts.msrc_dss_tcc_us);
    }
    if (enables.pre_range) {
        used_budget_us += (OVERHEAD_PRE_RANGE_OVERRHEAD_US + timeouts.pre_range_us);
    }
    if (enables.final_range) {
        used_budget_us += OVERHEAD_FINAL_RANGE_OVERRHEAD_US;
        used_budget_us += timeouts.final_range_us;
    }

    *budget_us = used_budget_us;
    co_return true;
}

//==============================================================================
// SPAD 管理（移植自 Pololu getSpadInfo / init SPAD 部分）
//==============================================================================

static Task<bool> get_spad_info(uint8_t addr, uint8_t *count, bool *type_is_aperture)
{
    uint8_t tmp;

    if (!(co_await write_reg(addr, 0x80, 0x01))) co_return false;
    if (!(co_await write_reg(addr, 0xFF, 0x01))) co_return false;
    if (!(co_await write_reg(addr, 0x00, 0x00))) co_return false;

    if (!(co_await write_reg(addr, 0xFF, 0x06))) co_return false;
    uint8_t r83;
    if (!(co_await read_reg(addr, 0x83, &r83))) co_return false;
    if (!(co_await write_reg(addr, 0x83, r83 | 0x04))) co_return false;

    if (!(co_await write_reg(addr, 0xFF, 0x07))) co_return false;
    if (!(co_await write_reg(addr, 0x81, 0x01))) co_return false;
    if (!(co_await write_reg(addr, 0x80, 0x01))) co_return false;
    if (!(co_await write_reg(addr, 0x94, 0x6b))) co_return false;
    if (!(co_await write_reg(addr, 0x83, 0x00))) co_return false;

    // 等待 SPAD map 准备好（带超时, 每 200us 轮询一次, 期间让出给健康传感器）
    uint64_t deadline = time_us_64() + 50000;   // 50ms 超时
    do {
        (co_await delay_us(200));

        if (!(co_await read_reg(addr, 0x83, &tmp))) co_return false;
        if (time_us_64() > deadline) co_return false;
    } while (tmp == 0x00);

    if (!(co_await write_reg(addr, 0x83, 0x01))) co_return false;
    if (!(co_await read_reg(addr, 0x92, &tmp))) co_return false;

    *count = tmp & 0x7f;
    *type_is_aperture = (tmp >> 7) & 0x01;

    if (!(co_await write_reg(addr, 0x81, 0x00))) co_return false;
    if (!(co_await write_reg(addr, 0xFF, 0x06))) co_return false;
    if (!(co_await read_reg(addr, 0x83, &r83))) co_return false;
    if (!(co_await write_reg(addr, 0x83, r83 & ~0x04))) co_return false;

    if (!(co_await write_reg(addr, 0xFF, 0x01))) co_return false;
    if (!(co_await write_reg(addr, 0x00, 0x01))) co_return false;
    if (!(co_await write_reg(addr, 0xFF, 0x00))) co_return false;
    if (!(co_await write_reg(addr, 0x80, 0x00))) co_return false;

    co_return true;
}

//==============================================================================
// 参考校准（移植自 Pololu performSingleRefCalibration）
//==============================================================================

static Task<bool> perform_single_ref_calibration(uint8_t addr, uint8_t vhv_init_byte)
{
    if (!(co_await write_reg(addr, REG_SYSRANGE_START, 0x01 | vhv_init_byte))) co_return false;

    // 等待中断状态位变化（带超时, 期间让出给健康传感器）
    uint64_t deadline = time_us_64() + 200000;   // 200ms 超时
    uint8_t status;
    do {
        (co_await delay_us(200));

        if (!(co_await read_reg(addr, REG_RESULT_INTERRUPT_STATUS, &status))) co_return false;
        if (time_us_64() > deadline) co_return false;
    } while ((status & 0x07) == 0);

    if (!(co_await write_reg(addr, REG_SYSTEM_INTERRUPT_CLEAR, 0x01))) co_return false;
    if (!(co_await write_reg(addr, REG_SYSRANGE_START, 0x00))) co_return false;

    co_return true;
}

//==============================================================================
// Tuning settings（逐项移植自 Pololu init() 中的 tuning settings 列表）
// 每项 (reg << 8) | val, 按 PairofRegisters 编码
//==============================================================================

static const uint16_t tuning_settings[] = {
    /* {reg, val} 逐项对应 Pololu init() 步骤 14 */
    0xFF01, 0x0000,   // setReg(0xFF, 0x01); setReg(0x00, 0x00)
    0xFF00, 0x0900, 0x1000, 0x1100,
    0x2401, 0x25FF, 0x7500,
    0xFF01, 0x4E2C, 0x4800, 0x3020,
    0xFF00, 0x3009, 0x5400, 0x3104, 0x3203, 0x4083, 0x4625, 0x6000, 0x2700,
    0x5006, 0x5100, 0x5296, 0x5608, 0x5730, 0x6100, 0x6200, 0x6400, 0x6500, 0x66A0,
    0xFF01, 0x2232, 0x4714, 0x49FF, 0x4A00,
    0xFF00, 0x7A0A, 0x7B00, 0x7821,
    0xFF01, 0x2334, 0x4200, 0x44FF, 0x4526, 0x4605, 0x4040, 0x0E06, 0x201A, 0x4340,
    0xFF00, 0x3403, 0x3544,
    0xFF01, 0x3104, 0x4B09, 0x4C05, 0x4D04,
    0xFF00, 0x4400, 0x4520, 0x4708, 0x4828, 0x6700, 0x7004, 0x7101, 0x72FE, 0x7600, 0x7700,
    0xFF01, 0x0D01,
    0xFF00, 0x8001, 0x01F8,
    0xFF01, 0x8E01, 0x0001, 0xFF00, 0x8000,
};

constexpr size_t TUNING_SETTINGS_COUNT = sizeof(tuning_settings) / sizeof(tuning_settings[0]);

static Task<bool> apply_tuning_settings(uint8_t addr)
{
    for (size_t i = 0; i < TUNING_SETTINGS_COUNT; i++) {
        uint8_t reg = (uint8_t)(tuning_settings[i] >> 8);
        uint8_t val = (uint8_t)(tuning_settings[i] & 0xFF);
        if (!(co_await write_reg(addr, reg, val))) co_return false;
    }
    co_return true;
}

//==============================================================================
// 连续测距启动（移植自 Pololu startContinuous, back-to-back 模式）
//==============================================================================

static Task<bool> start_continuous_backto_back(uint8_t addr, uint8_t stop_var)
{
    // back-to-back 模式前导码（DSS 必需的 stop variable 写回）
    if (!(co_await write_reg(addr, 0x80, 0x01))) co_return false;
    if (!(co_await write_reg(addr, 0xFF, 0x01))) co_return false;
    if (!(co_await write_reg(addr, 0x00, 0x00))) co_return false;
    if (!(co_await write_reg(addr, 0x91, stop_var))) co_return false;
    if (!(co_await write_reg(addr, 0x00, 0x01))) co_return false;
    if (!(co_await write_reg(addr, 0xFF, 0x00))) co_return false;
    if (!(co_await write_reg(addr, 0x80, 0x00))) co_return false;

    // SYSRANGE_START = 0x02 → MODE_BACKTOBACK 连续测距
    co_return (co_await write_reg(addr, REG_SYSRANGE_START, 0x02));
}

//==============================================================================
// 单传感器完整初始化（冷启动与恢复共用同一路径）
// 前置条件: 传感器刚经历 XSHUT 复位（地址回到 0x29）
//==============================================================================

static Task<bool> init_sensor(uint8_t index)
{
    const uint8_t new_addr = sensor_addr[index];
    uint16_t reg16;
    uint8_t reg8;

    // ---- XSHUT 释放 + 等待启动（让出式等待, 不阻塞健康传感器轮询）----
    gpio_put(xshut_pins[index], 1);
    (co_await delay_us((uint64_t)SENSOR_BOOT_DELAY_MS * 1000u));

    // ---- 1. 在默认地址检查存在性 ----
    if (!(co_await check_model_id(VL53L0X_DEFAULT_ADDR))) {
        co_return false;
    }

    // ---- 2. 修改 I2C 地址并验证 ----
    if (!(co_await write_reg(VL53L0X_DEFAULT_ADDR, REG_I2C_SLAVE_DEVICE_ADDRESS, new_addr))) {
        co_return false;
    }
    (co_await delay_us(2000));   // 地址切换稳定等待
    if (!(co_await check_model_id(new_addr))) {
        co_return false;
    }

    // ---- 3. 2V8 IO 模式（模块为 3.3V 供电）----
    if (!(co_await read_reg(new_addr, REG_VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HV, &reg8))) co_return false;
    if (!(co_await write_reg(new_addr, REG_VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HV, reg8 | 0x01))) co_return false;

    // ---- 4. Set I2C standard mode (官方 init 序列) ----
    if (!(co_await write_reg(new_addr, 0x88, 0x00))) co_return false;

    // ---- 5. 读取 stop variable (DSS 模式启动测距需要) ----
    if (!(co_await write_reg(new_addr, 0x80, 0x01))) co_return false;
    if (!(co_await write_reg(new_addr, 0xFF, 0x01))) co_return false;
    if (!(co_await write_reg(new_addr, 0x00, 0x00))) co_return false;
    if (!(co_await read_reg(new_addr, 0x91, &stop_variable[index]))) co_return false;
    if (!(co_await write_reg(new_addr, 0x00, 0x01))) co_return false;
    if (!(co_await write_reg(new_addr, 0xFF, 0x00))) co_return false;
    if (!(co_await write_reg(new_addr, 0x80, 0x00))) co_return false;

    // ---- 6. 禁用 MSRC 默认阈值控制, 启用 signal rate limit ----
    if (!(co_await read_reg(new_addr, REG_MSRC_CONFIG_CONTROL, &reg8))) co_return false;
    if (!(co_await write_reg(new_addr, REG_MSRC_CONFIG_CONTROL, reg8 | 0x12))) co_return false;

    if (!(co_await write_reg16(new_addr, REG_FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT,
                     VL53L0X_SIGNAL_RATE_LIMIT_Q97))) co_return false;

    // ---- 7. SPAD 管理 ----
    if (!(co_await write_reg(new_addr, REG_SYSTEM_SEQUENCE_CONFIG, 0xFF))) co_return false;

    uint8_t spad_count;
    bool spad_type_is_aperture;
    if (!(co_await get_spad_info(new_addr, &spad_count, &spad_type_is_aperture))) co_return false;

    uint8_t ref_spad_map[6];
    if (!(co_await read_multi(new_addr, REG_GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6))) co_return false;

    // Pololu: 根据 spad info 重写参考 SPAD 使能表
    if (!(co_await write_reg(new_addr, 0xFF, 0x01))) co_return false;
    if (!(co_await write_reg(new_addr, REG_DYNAMIC_SPAD_REF_EN_START_OFFSET, 0x00))) co_return false;
    if (!(co_await write_reg(new_addr, REG_DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD, 0x2C))) co_return false;
    if (!(co_await write_reg(new_addr, 0xFF, 0x00))) co_return false;
    if (!(co_await write_reg(new_addr, REG_GLOBAL_CONFIG_REF_EN_START_SELECT, 0xB4))) co_return false;

    uint8_t first_spad_to_enable = spad_type_is_aperture ? 12 : 0;
    uint8_t spads_enabled = 0;

    for (uint8_t i = 0; i < 48; i++) {
        if (i < first_spad_to_enable || spads_enabled == spad_count) {
            ref_spad_map[i / 8] &= ~(1 << (i % 8));
        } else if ((ref_spad_map[i / 8] >> (i % 8)) & 0x1) {
            spads_enabled++;
        }
    }

    if (!(co_await write_multi(new_addr, REG_GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6))) co_return false;

    // ---- 8. Tuning settings (官方校准数据) ----
    if (!(co_await apply_tuning_settings(new_addr))) co_return false;

    // ---- 9. 中断配置: 样本就绪中断, GPIO 低有效, 清除中断 ----
    if (!(co_await write_reg(new_addr, REG_SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04))) co_return false;
    if (!(co_await read_reg(new_addr, REG_GPIO_HV_MUX_ACTIVE_HIGH, &reg8))) co_return false;
    if (!(co_await write_reg(new_addr, REG_GPIO_HV_MUX_ACTIVE_HIGH, reg8 & ~0x10))) co_return false;
    if (!(co_await write_reg(new_addr, REG_SYSTEM_INTERRUPT_CLEAR, 0x01))) co_return false;

    // ---- 10. 序列配置: pre_range + final_range + dss (0xE8) ----
    if (!(co_await write_reg(new_addr, REG_SYSTEM_SEQUENCE_CONFIG, 0xE8))) co_return false;

    // ---- 11. TimingBudget = 20ms (唯一配置点) ----
    if (!(co_await set_measurement_timing_budget(new_addr, VL53L0X_TIMING_BUDGET_US))) co_return false;

    // ---- 12. 读回验证: 从寄存器重算实际 budget ----
    uint32_t actual_budget = 0;
    if (!(co_await get_measurement_timing_budget(new_addr, &actual_budget))) co_return false;
    int32_t diff = (int32_t)actual_budget - (int32_t)VL53L0X_TIMING_BUDGET_US;
    if (diff < -TIMING_BUDGET_VERIFY_TOLERANCE_US || diff > TIMING_BUDGET_VERIFY_TOLERANCE_US) {
        // 事件进队列（Core0 负责 CDC 输出）—— Core1 禁止 printf
        tof_event_post(TOF_EVT_BUDGET_VERIFY_FAIL, index, actual_budget);
        co_return false;
    }

    uint16_t final_timeout_reg = 0;
    uint16_t pre_timeout_reg = 0;
    (co_await read_reg16(new_addr, REG_FINAL_RANGE_CONFIG_TIMEOUT_MACROP, &final_timeout_reg));
    (co_await read_reg16(new_addr, REG_PRE_RANGE_CONFIG_TIMEOUT_MACROP, &pre_timeout_reg));

    // ---- 13. 参考校准: VHV + phase ----
    if (!(co_await write_reg(new_addr, REG_SYSTEM_SEQUENCE_CONFIG, 0x01))) co_return false;
    if (!(co_await perform_single_ref_calibration(new_addr, 0x40))) co_return false;

    if (!(co_await write_reg(new_addr, REG_SYSTEM_SEQUENCE_CONFIG, 0x02))) co_return false;
    if (!(co_await perform_single_ref_calibration(new_addr, 0x00))) co_return false;

    if (!(co_await write_reg(new_addr, REG_SYSTEM_SEQUENCE_CONFIG, 0xE8))) co_return false;

    // ---- 14. 启动连续测距 (back-to-back) ----
    if (!(co_await start_continuous_backto_back(new_addr, stop_variable[index]))) co_return false;

    // ---- 15. 最终状态确认 ----
    uint16_t range_status = 0;
    if (!(co_await read_reg16(new_addr, REG_RESULT_RANGE_STATUS, &range_status))) co_return false;

    // 就绪事件进队列（Core0 负责 CDC 输出）—— Core1 禁止 printf
    tof_event_post(TOF_EVT_READY, index, actual_budget);

    co_return true;
}

//==============================================================================
// 公共 API —— 初始化与恢复
//==============================================================================

static Task<bool> init_all(void)
{
    // ---- I2C1 外设初始化 ----
    tof_bus.init(I2C1_FREQ_HZ);
    gpio_set_function(I2C1_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C1_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C1_SDA);
    gpio_pull_up(I2C1_SCL);

    // ---- XSHUT 引脚初始化, 全部拉低 ----
    for (int i = 0; i < 5; i++) {
        gpio_init(xshut_pins[i]);
        gpio_set_dir(xshut_pins[i], GPIO_OUT);
        gpio_put(xshut_pins[i], 0);
    }

    (co_await delay_us((uint64_t)(XSHUT_RESET_DELAY_MS) * 1000u));

    // ---- 逐颗释放并初始化（避免地址冲突: 同一时刻只有一颗处于 0x29）----
    for (int i = 0; i < 5; i++) {
        memset((void *)&i2c_error_count[i], 0, sizeof(uint32_t));
        consecutive_errors[i] = 0;
        recovery_count[i] = 0;
        health_override[i] = HO_NONE;
        last_distance[i] = 0;

        if ((co_await init_sensor(i))) {
            sensor_ready[i] = true;
            // 数据/通信时间基准从初始化成功时刻开始计时
            // (防止 5 颗总初始化时间超过 no-data 阈值时误触发恢复)
            uint32_t now = to_ms_since_boot(get_absolute_time());
            last_new_data_ms[i] = now;
            last_comm_ok_ms[i] = now;
        } else {
            sensor_ready[i] = false;
            health_override[i] = HO_OFFLINE;
            // 传感器初始化失败: 拉低 XSHUT 保持复位, 后续由恢复状态机重试
            gpio_put(xshut_pins[i], 0);
            tof_event_post(TOF_EVT_INIT_FAIL, i, 0);
        }
    }

    co_return true;
}

// 单传感器恢复: XSHUT 硬复位 + 完整重初始化（与冷启动完全同一路径）
#if CHUNI_TOF_RECOVERY_ENABLE
Task<bool> recover_sensor_job(uint8_t index)
{
    if (index >= 5) co_return false;

    recovery_count[index] = recovery_count[index] + 1;

    // 复位期间标记离线, 快照数据由调用者失效
    sensor_ready[index] = false;
    health_override[index] = HO_RECOVERING;

    // XSHUT 拉低硬复位 → 地址回到 0x29（让出式等待）
    gpio_put(xshut_pins[index], 0);
    (co_await delay_us((uint64_t)XSHUT_RESET_DELAY_MS * 1000u));

    // The target is held in reset. Any ACK at 0x29 belongs to ANOTHER sensor
    // that has lost its assigned address (e.g. a shared power/reset transient).
    // Do not broadcast an address write to two sensors. Let the scheduler
    // hold all XSHUT low and readdress them one at a time.
    uint8_t unexpected_model = 0;
    if (co_await read_reg(VL53L0X_DEFAULT_ADDR, REG_IDENTIFICATION_MODEL_ID, &unexpected_model)) {
        address_conflict = true;
        health_override[index] = HO_OFFLINE;
        co_return false;
    }

    if ((co_await init_sensor(index))) {
        sensor_ready[index] = true;
        health_override[index] = HO_NONE;
        consecutive_errors[index] = 0;
        uint32_t now = to_ms_since_boot(get_absolute_time());
        last_new_data_ms[index] = now;
        last_comm_ok_ms[index] = now;
        co_return true;
    }

    // 恢复失败: 保持复位, 等待下次退避重试
    gpio_put(xshut_pins[index], 0);
    sensor_ready[index] = false;
    health_override[index] = HO_OFFLINE;
    co_return false;
}

// I2C1 总线恢复: 停止外设 → SDA/SCL 转 GPIO → 检测 SDA 卡低 → 9 时钟 + STOP → 恢复外设
Task<bool> bus_recover_job(void)
{
    // 严格有界: 最多 9 个时钟脉冲 (每个 ~10us) + STOP (~15us) + 两次外设重初始化,
    // 无任何无限等待路径。START 事件由调用者 (tof_reader 状态机) 发布。

    // ---- 1. 停止 I2C 外设, 释放引脚 ----
    while (io_claimed || tof_bus.busy()) co_await std::suspend_always{};
    io_claimed = true;
    reset_block_mask(1u << I2C_RESET_NUM(I2C1_PORT));
    gpio_set_function(I2C1_SDA, GPIO_FUNC_SIO);
    gpio_set_function(I2C1_SCL, GPIO_FUNC_SIO);

    // ---- 2. 作为开漏 GPIO 检测总线状态 ----
    gpio_init(I2C1_SDA);
    gpio_init(I2C1_SCL);
    gpio_set_dir(I2C1_SDA, GPIO_IN);
    gpio_set_dir(I2C1_SCL, GPIO_IN);
    gpio_pull_up(I2C1_SDA);
    gpio_pull_up(I2C1_SCL);

    (co_await delay_us(10));

    bool sda_low = !gpio_get(I2C1_SDA);

    // ---- 3. SDA 被从机拉低: 发送最多 9 个 SCL 时钟脉冲, 每个脉冲后检查 SDA ----
    // 全程开漏驱动: 拉低 = 预置输出0 + 方向OUT; 释放 = 方向IN (由上拉电阻拉高)
    // 严禁推挽输出高 —— VL53L0X 支持时钟拉伸, 从机可能正将 SCL/SDA 驱动为低,
    // 推挽输出高会与从机输出级形成电源对地短路
    if (sda_low) {
        for (int i = 0; i < 9; i++) {
            if (gpio_get(I2C1_SDA)) break;   // SDA 已释放, 提前结束

            // SCL 低
            gpio_put(I2C1_SCL, 0);
            gpio_set_dir(I2C1_SCL, GPIO_OUT);
            (co_await delay_us(5));
            // SCL 释放 (上拉拉高; 若从机时钟拉伸则保持低, 下一脉冲重试)
            gpio_set_dir(I2C1_SCL, GPIO_IN);
            (co_await delay_us(5));
        }

        // 4. STOP 条件: SCL 高电平期间 SDA 由低变高
        gpio_set_dir(I2C1_SCL, GPIO_IN);          // 确保 SCL 释放为高
        gpio_put(I2C1_SDA, 0);
        gpio_set_dir(I2C1_SDA, GPIO_OUT);         // SDA 低
        (co_await delay_us(5));
        gpio_set_dir(I2C1_SDA, GPIO_IN);          // SDA 释放 → 低变高 = STOP
        (co_await delay_us(10));
    }

    // ---- 5. 验证总线释放 ----
    bool bus_ok = gpio_get(I2C1_SDA) && gpio_get(I2C1_SCL);

    // ---- 6. 重新初始化 I2C1 外设 ----
    tof_bus.init(I2C1_FREQ_HZ);
    while (tof_bus.busy()) co_await std::suspend_always{};
    io_claimed = false;
    gpio_set_function(I2C1_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C1_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C1_SDA);
    gpio_pull_up(I2C1_SCL);

    // 结果事件由调用者 (tof_reader 状态机) 统一发布

    co_return bus_ok;
}

// 全传感器恢复: 所有 XSHUT 拉低 → 逐颗释放并完整初始化
Task<bool> reinit_all_job(void)
{
    // 全部下线 + 快照语义由调用者负责
    for (int i = 0; i < 5; i++) {
        sensor_ready[i] = false;
        health_override[i] = HO_OFFLINE;
        consecutive_errors[i] = 0;
        gpio_put(xshut_pins[i], 0);
    }

    (co_await delay_us((uint64_t)(XSHUT_RESET_DELAY_MS) * 1000u));   // 全部复位期间无传感器可轮询, 直接延时即可

    uint8_t ok_bitmap = 0;
    bool all_ok = true;
    for (int i = 0; i < 5; i++) {
        recovery_count[i] = recovery_count[i] + 1;

        if ((co_await init_sensor(i))) {
            sensor_ready[i] = true;
            health_override[i] = HO_NONE;
            uint32_t now = to_ms_since_boot(get_absolute_time());
            last_new_data_ms[i] = now;
            last_comm_ok_ms[i] = now;
            ok_bitmap |= (1u << i);
        } else {
            sensor_ready[i] = false;
            health_override[i] = HO_OFFLINE;
            gpio_put(xshut_pins[i], 0);
            all_ok = false;
            tof_event_post(TOF_EVT_INIT_FAIL, i, 0);
        }
    }

    // 汇总事件进队列（Core0 负责 CDC 输出）—— Core1 禁止 printf
    tof_event_post(TOF_EVT_FULL_REINIT_DONE, 0xFF, ok_bitmap);

    co_return all_ok;
}

#endif
void vl53l0x_force_offline(uint8_t index)
{
#if CHUNI_TOF_RECOVERY_ENABLE

    if (index >= 5) return;
    sensor_ready[index] = false;
    health_override[index] = HO_RECOVERING;

#else
    return;
#endif
}

// 存活探测（不改变任何配置, 不产生测距动作）:
// 总线级恢复后用于甄别 —— 响应的传感器原样保留继续测距,
// 无响应的传感器才进入各自的 XSHUT 恢复路径。
// 探测成功时刷新通信时间戳并清零连续错误 (供掉线判定使用)。
Task<bool> probe_sensor_job(uint8_t index)
{
    if (index >= 5 || !sensor_ready[index]) co_return false;

    if (!(co_await check_model_id(sensor_addr[index]))) {
        record_comm_error(index);
        co_return false;
    }

    stamp_comm_ok(index);
    consecutive_errors[index] = 0;
    co_return true;
}

//==============================================================================
// 公共 API —— 数据读取
//==============================================================================

static Task<vl53l0x_read_result_t> read_distance_job(uint8_t index, uint16_t *distance)
{
    if (index >= 5) co_return VL53L0X_READ_NOT_INITIALIZED;
    if (!sensor_ready[index]) co_return VL53L0X_READ_NOT_INITIALIZED;

    const uint8_t addr = sensor_addr[index];

    // ---- 检查中断状态: bit0-2 非零 = 新样本就绪 ----
    uint8_t status;
    if (!(co_await read_reg(addr, REG_RESULT_INTERRUPT_STATUS, &status))) {
        record_comm_error(index);
        co_return VL53L0X_READ_COMM_ERROR;    // 通信失败: 不返回任何数据
    }

    // 状态寄存器读取成功 = 通信层存活（与是否产出数据无关）
    stamp_comm_ok(index);

    if ((status & 0x07) == 0) {
        // 测量进行中 —— 这次成功的通信同时证明传感器没有失联,
        // 连续通信错误计数在此清零（"连续失败"必须是连续的通信失败,
        // 不能把夹杂着成功通信的错误序列累加成掉线依据）
        consecutive_errors[index] = 0;
        co_return VL53L0X_READ_NOT_READY;     // 测量进行中（通信正常）
    }

    // ---- 读取距离结果 (0x14 + 10 = 0x1E) ----
    uint16_t range;
    if (!(co_await read_reg16(addr, 0x1E, &range))) {
        record_comm_error(index);
        co_return VL53L0X_READ_COMM_ERROR;
    }

    // ---- 清除中断 ----
    // 清除失败绝对不能静默忽略: 否则下一次轮询会读到同一次旧测量结果,
    // 并把它当作"新数据"上报（stale-as-fresh bug）
    if (!(co_await write_reg(addr, REG_SYSTEM_INTERRUPT_CLEAR, 0x01))) {
        record_comm_error(index);
        co_return VL53L0X_READ_COMM_ERROR;
    }

    // ---- 有效性: 0 / 8190+ 为无效测距（保持原语义）----
    if (range == 0 || range >= 8190) {
        range = 8190;    // 无目标/超出量程, 仍是真实新数据
    }

    last_distance[index] = range;
    consecutive_errors[index] = 0;
    last_new_data_ms[index] = to_ms_since_boot(get_absolute_time());

    if (distance) *distance = range;
    co_return VL53L0X_READ_NEW_DATA;
}

uint16_t vl53l0x_get_last_distance(uint8_t index)
{
    if (index >= 5) return 0;
    return last_distance[index];
}

bool vl53l0x_is_ready(uint8_t index)
{
    if (index >= 5) return false;
    return sensor_ready[index];
}

//==============================================================================
// 公共 API —— 健康状态与统计
//==============================================================================

vl53l0x_health_t vl53l0x_get_health(uint8_t index)
{
    if (index >= 5) return VL53L0X_HEALTH_OFFLINE;

    switch (health_override[index]) {
    case HO_RECOVERING: return VL53L0X_HEALTH_RECOVERING;
    case HO_OFFLINE:    return VL53L0X_HEALTH_OFFLINE;
    default:            break;
    }

    if (!sensor_ready[index]) return VL53L0X_HEALTH_OFFLINE;
    if (consecutive_errors[index] > 0) return VL53L0X_HEALTH_DEGRADED;
    return VL53L0X_HEALTH_OK;
}

uint32_t vl53l0x_get_error_count(uint8_t index)
{
    if (index >= 5) return 0;
    return i2c_error_count[index];
}

uint32_t vl53l0x_get_consecutive_errors(uint8_t index)
{
    if (index >= 5) return 0;
    return consecutive_errors[index];
}

uint32_t vl53l0x_get_last_new_data_ms(uint8_t index)
{
    if (index >= 5) return 0;
    return last_new_data_ms[index];
}

// 最后一次成功通信（任何寄存器读取成功）的时间戳 (ms)
// 与 last_new_data 的区别: 传感器 "应答但不产出数据" 时, comm 刷新而 data 停滞
uint32_t vl53l0x_get_last_comm_ok_ms(uint8_t index)
{
    if (index >= 5) return 0;
    return last_comm_ok_ms[index];
}

uint32_t vl53l0x_get_recovery_count(uint8_t index)
{
    if (index >= 5) return 0;
    return recovery_count[index];
}

uint32_t vl53l0x_get_timing_budget_us(void)
{
    return VL53L0X_TIMING_BUDGET_US;
}

// 全局停顿善后: Core0 侧 Flash 擦写 (multicore lockout + XIP 停摆) 等操作会把
// 双核同时冻结数十至数百毫秒。停顿期间 Core1 无法轮询, 恢复后的第一轮循环里
// "无数据时长" 会被一次性放大, 超过 NO_DATA_RECOVERY_MS 即误触发恢复风暴。
// 由 Core1 在检测到停顿后调用: 刷新全部时间基准并清零连续错误计数,
// 传感器真实状态由停顿后的实际轮询结果重新给出。
void vl53l0x_note_global_stall(uint32_t now_ms)
{
    for (int i = 0; i < 5; i++) {
        consecutive_errors[i] = 0;
        if (sensor_ready[i]) {
            last_comm_ok_ms[i] = now_ms;
            last_new_data_ms[i] = now_ms;
        }
    }
}


// Exactly one management job (address 0x29 may have only one owner) plus one
// sampling job. They cooperate via io_claimed, never wait on a mutex.
static Task<bool> management_job;
static Task<vl53l0x_read_result_t> sample_job;
static uint8_t sampling_index = 0xff;
static uint16_t sampled_distance = 0;
static volatile uint32_t management_kind = 0;
static volatile uint32_t published_management = 0;

void vl53l0x_init() {
    management_kind = 1;
    management_job = init_all();
}
void vl53l0x_task_step() {
    tof_bus.step();
    management_job.step();
    sample_job.step();
    published_management = management_job.done() ? 0 : management_kind;
}
bool vl53l0x_management_busy() { return !management_job.done(); }
bool vl53l0x_management_result() { return management_job.valid() && management_job.result(); }
bool vl53l0x_start_recovery(uint8_t index) {
#if CHUNI_TOF_RECOVERY_ENABLE

    if (!management_job.done() || sample_job.valid() || index >= 5) return false;
    sensor_ready[index] = false;
    management_kind = 2;
    management_job = recover_sensor_job(index);
    return true;

#else
    return false;
#endif
}
bool vl53l0x_start_bus_recovery() {
#if CHUNI_TOF_RECOVERY_ENABLE

    if (!management_job.done() || sample_job.valid()) return false;
    management_kind = 3;
    management_job = bus_recover_job();
    return true;

#else
    return false;
#endif
}
bool vl53l0x_start_reinit_all() {
#if CHUNI_TOF_RECOVERY_ENABLE

    if (!management_job.done() || sample_job.valid()) return false;
    address_conflict = false;
    management_kind = 4;
    management_job = reinit_all_job();
    return true;

#else
    return false;
#endif
}
bool vl53l0x_start_probe(uint8_t index) {
#if CHUNI_TOF_RECOVERY_ENABLE

    if (!management_job.done() || sample_job.valid() || index >= 5) return false;
    management_kind = 5;
    management_job = probe_sensor_job(index);
    return true;

#else
    return false;
#endif
}
vl53l0x_read_result_t vl53l0x_read_distance_ex(uint8_t index, uint16_t* distance) {
    if (index >= 5) return VL53L0X_READ_NOT_INITIALIZED;
    if (sample_job.valid()) {
        if (sampling_index != index || !sample_job.done()) return VL53L0X_READ_PENDING;
        const auto result = sample_job.result();
        if (result == VL53L0X_READ_NEW_DATA && distance) *distance = sampled_distance;
        sample_job.clear();
        sampling_index = 0xff;
        return result;
    }
    if (!sensor_ready[index]) return VL53L0X_READ_NOT_INITIALIZED;
    // Bus clear/full reset must have exclusive access, while per-device init
    // may interleave reads of all other ready sensors.
    if (!management_job.done() && management_kind >= 3) return VL53L0X_READ_PENDING;
    sampling_index = index;
    sample_job = read_distance_job(index, &sampled_distance);
    return sample_job.valid() ? VL53L0X_READ_PENDING : VL53L0X_READ_COMM_ERROR;
}
uint32_t vl53l0x_get_io_phase() { return tof_bus.phase(); }
uint32_t vl53l0x_get_bus_resets() { return tof_bus.reset_count(); }
uint32_t vl53l0x_get_management_kind() { return published_management; }
uint32_t vl53l0x_get_frame_failures() { return TaskFrames::failures; }
bool vl53l0x_needs_readdress() { return address_conflict; }

} // namespace Chuni245Tof
