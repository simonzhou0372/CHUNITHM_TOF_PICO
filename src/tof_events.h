/*
 * ToF Event Queue — Core1 → Core0 非阻塞事件通道
 *
 * 架构约束（USB 与 ToF Recovery 完全隔离）:
 *   - Core1 (I2C1/VL53L0X/Recovery) 绝对禁止调用 printf / TinyUSB API。
 *     pico-sdk 的 USB CDC stdio 后端内部持有跨核互斥锁 stdio_usb_mutex 并直接
 *     调用 tud_cdc_write —— Core1 的 printf 会与 Core0 的 tud_task() 并发改写
 *     TinyUSB device stack 内部状态（竞态可导致 HID endpoint 卡死, 只能重新
 *     枚举恢复）, 且 Core1 持锁期间 Core0 的 printf 会阻塞 → tud_task 饥饿。
 *   - Core1 只把事件推入本队列（固定容量 / 无 malloc / 无阻塞 / 满则丢弃）,
 *     Core0 在主循环低优先级弹出并通过 CDC 打印。
 *
 * 同步模型: SPSC（单生产者 Core1 / 单消费者 Core0）环形缓冲。
 *   - head 仅由 Core0 写, tail 仅由 Core1 写
 *   - 数据写入与索引更新之间加编译器屏障（RP2040 双核缓存一致性由 SIO
 *     总线保证, barrier 保证编译器不重排）
 */

#ifndef TOF_EVENTS_H
#define TOF_EVENTS_H

#include <stdint.h>
#include <stdbool.h>

namespace Chuni245Tof {

// 事件类型（状态变化事件, 非逐错误日志 —— 逐错误日志不进队列, 只进计数器）
typedef enum : uint8_t {
    TOF_EVT_NONE = 0,
    TOF_EVT_CORE1_STARTED,          // Core1 轮询任务启动
    TOF_EVT_INIT_FAIL,              // 冷启动初始化失败 (arg32=0)
    TOF_EVT_READY,                  // 传感器就绪 (arg32 = actual budget us)
    TOF_EVT_BUDGET_VERIFY_FAIL,     // TimingBudget 读回验证失败 (arg32 = actual budget us)
    TOF_EVT_OFFLINE,                // 掉线下线 (arg32 = {consec<<24|comm_silent_ms<<8|no_data_ms>>8} 简化: consec)
    TOF_EVT_RECOVERY_ATTEMPT,       // 开始恢复尝试 (arg32 = fail_count)
    TOF_EVT_RECOVERY_OK,            // 恢复成功 (arg32 = recovery 序号)
    TOF_EVT_RECOVERY_FAIL,          // 恢复失败 (arg32 = fail_count)
    TOF_EVT_BUS_RECOVERY_START,     // 总线恢复开始
    TOF_EVT_BUS_RECOVERY_OK,        // 总线恢复成功 (arg16 低 16 = survivors)
    TOF_EVT_BUS_RECOVERY_FAIL,      // 总线恢复失败 → 全量重初始化
    TOF_EVT_FULL_REINIT_DONE,       // 全量重初始化完成 (arg32 = 成功的传感器位图)
    TOF_EVT_STALL_NOTED,            // 检测到全局停顿, 时间基准已刷新 (arg32 = stall us)
} tof_event_type_t;

typedef struct {
    uint32_t timestamp_ms;   // 事件产生时刻 (Core1 视角, ms since boot)
    uint32_t arg32;          // 类型相关载荷
    uint8_t  sensor;         // 传感器索引 0-4, 0xFF = 不针对特定传感器
    uint8_t  type;           // tof_event_type_t
    uint8_t  arg16_lo;       // 备用小载荷（如 survivors）
    uint8_t  arg16_hi;
} tof_event_t;

// Core1 调用: 非阻塞入队。队列满 → 丢弃并计数, 绝不等待 Core0。
void tof_event_push(const tof_event_t *ev);

// 便捷封装（Core1 调用）
void tof_event_post(uint8_t type, uint8_t sensor, uint32_t arg32);

// Core0 调用: 非阻塞出队。队列空 → 返回 false。
bool tof_event_pop(tof_event_t *out);

// Core0 调用: 因队满而丢弃的事件累计数
uint32_t tof_event_dropped_count(void);

} // namespace Chuni245Tof

#endif /* TOF_EVENTS_H */
