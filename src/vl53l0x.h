/*
 * VL53L0X TOF Sensor Header
 *
 * 可靠性架构:
 * - TimingBudget 统一配置源: VL53L0X_TIMING_BUDGET_US = 20000us (20ms)
 * - 完整的初始化错误检查（所有关键 I2C 写入都必须成功）
 * - 单传感器 XSHUT 恢复（与冷启动完全相同的初始化路径, 只操作目标传感器）
 * - I2C1 总线恢复（9-clock + STOP + 外设重新初始化, 不动 XSHUT/配置）
 * - 恢复分级: 瞬时错误容忍 → 单传感器恢复 → 总线恢复+存活甄别 → 全量重初始化
 *   (全量重初始化用于总线恢复失败或默认地址冲突)
 * - Sensor Health 状态机
 * - 所有总线操作只能在 Core1 执行（I2C1 由 Core1 独占）
 *
 * Recovery Access Boundary (恢复访问边界):
 * - 本文件 + tof_reader.cpp 是全工程唯一允许接触 I2C1 外设、
 *   I2C1 GPIO (GP6/GP7)、ToF XSHUT (GP1~GP5) 的翻译单元
 * - 恢复路径 (vl53l0x_start_recovery / start_bus_recovery / start_reinit_all)
 *   绝不触碰: I2C0 外设、GPIO16/17、MPR121、Slider、USB、任何板级/系统级初始化
 * - I2C0 (MPR121) 由 mpr121.cpp 独占且仅 Core0 访问, 与本文件无任何共享状态;
 *   AsyncI2c 按实例选择复位掩码，对 I2C1 的复位不操作 I2C0
 */

#ifndef VL53L0X_H
#define VL53L0X_H

#include <stdint.h>
#include <stdbool.h>

//==============================================================================
// ToF Recovery 总开关 (编译期)
//
//   1 = 启用分级恢复 (默认): 单传感器 XSHUT 复位重初始化 (Level 2) /
//       I2C1 总线恢复 + 存活甄别 (Level 3) / 全量重初始化 (Level 4)
//   0 = 禁用传感器恢复: 纯轮询模式 —— 完全不做掉线检测, 不存在下线/
//       快照失效/OFFLINE 事件/任何恢复动作。传感器无论通信是否正常都
//       持续轮询。底层 I2C 超时后清理控制器不受此开关影响。
//==============================================================================

// Master switch: 0 removes runtime fault detection/recovery and watchdogs.
// Normal initialization, bounded transaction errors and data validity remain.
#ifndef CHUNI_FAULT_RECOVERY_ENABLE
#define CHUNI_FAULT_RECOVERY_ENABLE 0
#endif
#ifndef CHUNI_TOF_RECOVERY_ENABLE
#define CHUNI_TOF_RECOVERY_ENABLE 1
#endif
#ifndef CHUNI_USB_AUTO_RECOVERY_ENABLE
#define CHUNI_USB_AUTO_RECOVERY_ENABLE 0
#endif
// Master OFF overrides all individual switches, including build definitions.
#if !CHUNI_FAULT_RECOVERY_ENABLE
#undef CHUNI_TOF_RECOVERY_ENABLE
#define CHUNI_TOF_RECOVERY_ENABLE 0
#undef CHUNI_USB_AUTO_RECOVERY_ENABLE
#define CHUNI_USB_AUTO_RECOVERY_ENABLE 0
#endif

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
    VL53L0X_READ_NOT_INITIALIZED,   // 传感器未初始化 / 已下线
    VL53L0X_READ_PENDING           // 非阻塞事务尚未完成，不算通信错误
} vl53l0x_read_result_t;

//==============================================================================
// 初始化与恢复（全部必须在 Core1 上调用 —— I2C1 由 Core1 独占）
//==============================================================================

// 安排完整初始化并立即返回: I2C1 外设 + 全部 5 个传感器
// 每个传感器: XSHUT 复位 → 地址分配 → 完整初始化 → TimingBudget=20ms → 验证 → 连续测距
void vl53l0x_init(void);

// 单传感器恢复: XSHUT 硬复位 + 完整重新初始化（与冷启动路径完全一致）
// 所有 start_* 返回 true 仅表示任务已接受；完成结果见 management_result。
bool vl53l0x_start_recovery(uint8_t index);

// I2C1 总线恢复: 停止外设 → GPIO 模拟 9 个 SCL 时钟 + STOP → 重新初始化 I2C1
bool vl53l0x_start_bus_recovery(void);

// 全传感器恢复: 所有 XSHUT 拉低 → 逐颗释放并完整初始化（避免地址冲突）
bool vl53l0x_start_reinit_all(void);

// 将传感器标记为离线（停止轮询该传感器, 数据失效）
// 仅供 Core1 恢复状态机使用
void vl53l0x_force_offline(uint8_t index);

// 存活探测: 读取 MODEL_ID 验证传感器在总线上仍可访问, 不改变任何配置。
// 供总线恢复后的分级甄别使用: 响应者原样保留继续测距,
// 无响应者才进入单传感器 XSHUT 恢复。
// 探测成功时刷新通信时间戳并清零连续错误计数。
bool vl53l0x_start_probe(uint8_t index);

// Core1 每轮推进一次，不等待 I2C、XSHUT、SPAD 或参考校准完成。
void vl53l0x_task_step();
bool vl53l0x_management_busy();
// 仅在 management_busy() == false 后读取（Core1）。
bool vl53l0x_management_result();
bool vl53l0x_needs_readdress(); // Core1: unexpected ACK at 0x29 during recovery
// 以下诊断值为发布后的标量，Core0 可读，不访问运行中的任务对象。
uint32_t vl53l0x_get_io_phase();
uint32_t vl53l0x_get_bus_resets();
uint32_t vl53l0x_get_management_kind();
uint32_t vl53l0x_get_frame_failures();

//==============================================================================
// 数据读取
//==============================================================================

// Core1 调用；PENDING 时保留 index，下轮重试，完成前不写 distance。
vl53l0x_read_result_t vl53l0x_read_distance_ex(uint8_t index, uint16_t *distance);

// 获取上一次有效的距离数据（不触发新测量）
uint16_t vl53l0x_get_last_distance(uint8_t index);

// 检查传感器是否已初始化就绪
bool vl53l0x_is_ready(uint8_t index);

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

// 全局停顿善后（Core1 调用）: 双核同时被冻结 (Core0 Flash 擦写 / multicore lockout)
// 后刷新所有传感器的时间基准并清零连续错误, 防止恢复状态机把停顿误判为掉线。
void vl53l0x_note_global_stall(uint32_t now_ms);

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* VL53L0X_H */
