/*
 * VL53L0X TOF Sensor Header
 *
 * 可靠性架构:
 * - TimingBudget 统一配置源: VL53L0X_TIMING_BUDGET_US = 20000us (20ms)
 * - 完整的初始化错误检查（所有关键 I2C 写入都必须成功）
 * - 单传感器 XSHUT 恢复（与冷启动完全相同的初始化路径, 只操作目标传感器）
 * - I2C1 总线恢复（9-clock + STOP + 外设重新初始化, 不动 XSHUT/配置）
 * - 恢复分级: 瞬时错误容忍 → 单传感器恢复 → 总线恢复+存活甄别 → 全量重初始化
 *   (全量重初始化仅在总线恢复失败时作为最后手段)
 * - Sensor Health 状态机
 * - 所有总线操作只能在 Core1 执行（I2C1 由 Core1 独占）
 */

#ifndef VL53L0X_H
#define VL53L0X_H

#include <stdint.h>
#include <stdbool.h>

namespace Chuni245Tof {

//==============================================================================
// 传感器健康状态
//==============================================================================

typedef enum {
    VL53L0X_HEALTH_OK = 0,      // 正常: 有新数据, 无通信错误
    VL53L0X_HEALTH_DEGRADED,    // 降级: 有通信错误但仍有数据 / 尚未达到恢复阈值
    VL53L0X_HEALTH_RECOVERING,  // 恢复中: XSHUT 复位 + 重新初始化进行中
    VL53L0X_HEALTH_OFFLINE      // 离线: 初始化失败或恢复失败, 数据无效
} vl53l0x_health_t;

//==============================================================================
// 读取结果（区分"测量未完成"与"I2C 通信失败"）
//==============================================================================

typedef enum {
    VL53L0X_READ_NEW_DATA = 0,      // 真正读取到一次新的测量结果
    VL53L0X_READ_NOT_READY,         // 测量尚未完成（I2C 通信正常）
    VL53L0X_READ_COMM_ERROR,        // I2C 通信失败（不产生数据）
    VL53L0X_READ_NOT_INITIALIZED    // 传感器未初始化 / 已下线
} vl53l0x_read_result_t;

//==============================================================================
// 初始化与恢复（全部必须在 Core1 上调用 —— I2C1 由 Core1 独占）
//==============================================================================

// 完整初始化: I2C1 外设 + 全部 5 个传感器
// 每个传感器: XSHUT 复位 → 地址分配 → 完整初始化 → TimingBudget=20ms → 验证 → 连续测距
void vl53l0x_init(void);

// 单传感器恢复: XSHUT 硬复位 + 完整重新初始化（与冷启动路径完全一致）
// 返回 true = 恢复成功
bool vl53l0x_recover_sensor(uint8_t index);

// I2C1 总线恢复: 停止外设 → GPIO 模拟 9 个 SCL 时钟 + STOP → 重新初始化 I2C1
// 返回 true = 总线已恢复可用
bool vl53l0x_bus_recover(void);

// 全传感器恢复: 所有 XSHUT 拉低 → 逐颗释放并完整初始化（避免地址冲突）
// 返回 true = 全部 5 个传感器恢复成功
bool vl53l0x_reinit_all(void);

// 将传感器标记为离线（停止轮询该传感器, 数据失效）
// 仅供 Core1 恢复状态机使用
void vl53l0x_force_offline(uint8_t index);

// 存活探测: 读取 MODEL_ID 验证传感器在总线上仍可访问, 不改变任何配置。
// 供总线恢复后的分级甄别使用: 响应者原样保留继续测距,
// 无响应者才进入单传感器 XSHUT 恢复。
// 探测成功时刷新通信时间戳并清零连续错误计数。
bool vl53l0x_probe_sensor(uint8_t index);

// 注册让出回调: 初始化/恢复的长等待期间 (XSHUT 复位 / SPAD 查询 / 参考校准)
// 驱动会周期性调用该回调, 由 tof_reader 继续轮询健康传感器,
// 保证单传感器恢复期间其他 4 个传感器的数据不超龄 (50ms)
// 必须在 Core1 上调用 (回调也在 Core1 上执行)
void vl53l0x_set_yield_callback(void (*cb)(void));

//==============================================================================
// 数据读取
//==============================================================================

// 读取距离（扩展版, 区分错误类型）—— 供 Core1 轮询使用
vl53l0x_read_result_t vl53l0x_read_distance_ex(uint8_t index, uint16_t *distance);

// 兼容旧接口: true = 成功读取新数据
bool vl53l0x_read_distance(uint8_t index, uint16_t *distance);

// 获取上一次有效的距离数据（不触发新测量）
uint16_t vl53l0x_get_last_distance(uint8_t index);

// 检查传感器是否已初始化就绪
bool vl53l0x_is_ready(uint8_t index);

// 检查传感器是否有新数据就绪（基于中断状态寄存器）
bool vl53l0x_has_new_data(uint8_t index);

//==============================================================================
// 健康状态与统计
//==============================================================================

vl53l0x_health_t vl53l0x_get_health(uint8_t index);

// 累计 I2C 错误计数（自启动/恢复以来）
uint32_t vl53l0x_get_error_count(uint8_t index);

// 连续通信错误计数（任何成功的通信 —— 含 NOT_READY 轮询 —— 都会清零）
uint32_t vl53l0x_get_consecutive_errors(uint8_t index);

// 最后一次成功读取新数据的时间戳 (ms)
uint32_t vl53l0x_get_last_new_data_ms(uint8_t index);

// 最后一次成功通信的时间戳 (ms) —— 任何寄存器读取成功都算。
// 区分 "通信层失联"（comm 停滞）与 "测距卡死"（comm 正常但 data 停滞）
uint32_t vl53l0x_get_last_comm_ok_ms(uint8_t index);

// 恢复次数统计
uint32_t vl53l0x_get_recovery_count(uint8_t index);

// 配置的 TimingBudget (us) —— 统一配置源
uint32_t vl53l0x_get_timing_budget_us(void);

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* VL53L0X_H */
