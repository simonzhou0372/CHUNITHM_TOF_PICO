/*
 * ToF Event Queue 实现 — SPSC 环形缓冲 (Core1 生产 / Core0 消费)
 * 详见 tof_events.h 头注释
 */

#include "tof_events.h"

#include "pico/time.h"

#include <string.h>

namespace Chuni245Tof {

namespace {

// 固定容量, 2 的幂 (掩码索引)。32 条 × 12B = 384B 静态内存, 无 malloc。
// Core1 最坏事件速率远低于 Core0 每轮弹出的数量, 32 条足够吸收突发;
// 即使爆满也只丢调试事件, 不影响任何实时路径。
constexpr uint32_t TOF_EVENT_QUEUE_SIZE = 32u;
constexpr uint32_t TOF_EVENT_QUEUE_MASK = TOF_EVENT_QUEUE_SIZE - 1u;

tof_event_t evt_buf[TOF_EVENT_QUEUE_SIZE];
volatile uint32_t evt_head = 0;   // 仅 Core0 读写 (消费位置)
volatile uint32_t evt_tail = 0;   // 仅 Core1 读写 (生产位置)
volatile uint32_t evt_dropped = 0;

} // anonymous namespace

void tof_event_push(const tof_event_t *ev)
{
    uint32_t tail = evt_tail;
    if (((tail + 1u) & TOF_EVENT_QUEUE_MASK) == evt_head) {
        evt_dropped++;   // 满: 丢弃低优先级调试事件, 绝不等待消费者
        return;
    }
    evt_buf[tail] = *ev;
    __compiler_memory_barrier();   // 数据写入先于索引发布
    evt_tail = (tail + 1u) & TOF_EVENT_QUEUE_MASK;
}

void tof_event_post(uint8_t type, uint8_t sensor, uint32_t arg32)
{
    tof_event_t ev;
    ev.timestamp_ms = to_ms_since_boot(get_absolute_time());
    ev.arg32 = arg32;
    ev.sensor = sensor;
    ev.type = type;
    ev.arg16_lo = 0;
    ev.arg16_hi = 0;
    tof_event_push(&ev);
}

bool tof_event_pop(tof_event_t *out)
{
    uint32_t head = evt_head;
    if (head == evt_tail) {
        return false;
    }
    *out = evt_buf[head];
    __compiler_memory_barrier();   // 数据读取完成后才发布消费位置
    evt_head = (head + 1u) & TOF_EVENT_QUEUE_MASK;
    return true;
}

uint32_t tof_event_dropped_count(void)
{
    return evt_dropped;
}

} // namespace Chuni245Tof
