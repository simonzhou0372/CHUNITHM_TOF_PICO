/*
 * Flash Save Header
 */

#ifndef SAVE_H
#define SAVE_H

#include <stdint.h>
#include "pico/mutex.h"

namespace Chuni245Tof {

void save_init(uint32_t magic, mutex_t* mutex);
bool save_load(void* data, uint32_t len);
bool save_write(const void* data, uint32_t len);

// 请求保存（延迟执行）: 立即快照数据并置位, 实际 Flash 擦写由 save_loop() 执行。
// 返回 false = 未初始化/数据非法/已有待执行的保存。
bool save_request_write(const void* data, uint32_t len);

// 是否有待执行的保存
bool save_pending();

// 在 Core0 主循环低优先级调用: 执行待保存的 Flash 擦写。
// Flash 操作经 SDK flash_safe_execute 执行 (双核安全);
// 只在 "无按键活动" (save_set_idle_hint(true)) 或等待超过上限时执行,
// 保证这个受控的 USB 短暂停 (XIP 停摆 ~50-400ms) 绝不随机落在游戏进行中。
void save_loop();

// 主循环每轮更新: 当前无任何按键活动 = true (空闲窗口提示)
void save_set_idle_hint(bool idle);

// 执行统计 (RAM 常驻, 可经 STATUS 读取, 用于核对 USB dropout 与 SAVE 是否同步)
uint32_t save_get_count(void);
uint32_t save_get_fail_count(void);
uint32_t save_get_max_duration_us(void);
uint64_t save_get_last_start_us(void);
uint64_t save_get_last_end_us(void);

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* SAVE_H */