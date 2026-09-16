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
// Flash 操作期间: 双核 XIP 停摆 (multicore lockout 保护 Core1),
// USB 约 50-400ms 无输出 —— 因此绝不从命令解析路径直接同步擦写。
void save_loop();

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* SAVE_H */