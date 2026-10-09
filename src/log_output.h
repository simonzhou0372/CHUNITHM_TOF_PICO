/*
 * printf 输出总开关 (全工程统一)
 *
 * ★ 默认 0 = 只发送 HID 报文, 所有 printf 编译为空操作 ★
 *
 * 背景: SDK stdio 的 USB CDC 后端在 "DTR 已连接但主机停止读取" 时,
 * 每次 printf 调用最多阻塞 500ms (PICO_STDIO_USB_STDOUT_TIMEOUT_US,
 * SDK 2.3.0 默认 500000us), 期间 tud_task() 得不到调用, HID endpoint
 * 失去服务 —— 这是 "传输固定数据量后 USB 完全停止" 的根因。
 *
 * USBSTATUS 不依赖此开关: 它通过 TinyUSB CDC 直接发送一条有界诊断回复,
 * TX 空间不足即跳过, 无等待。默认构建即可读取复位原因和上次停留阶段。
 * 单独把此开关改为 1 不会恢复 STATUS/PERF 等旧 printf 输出, 因为 CMake
 * 仍禁用了 stdio USB/UART 后端; 不要为了诊断重新启用阻塞的 stdio_usb。
 */

#ifndef LOG_OUTPUT_H
#define LOG_OUTPUT_H

#ifndef CHUNI_PRINTF_ENABLE
#define CHUNI_PRINTF_ENABLE 0
#endif

#if !CHUNI_PRINTF_ENABLE
#ifdef printf
#undef printf
#endif
#define printf(...) ((void)0)
#endif

#endif /* LOG_OUTPUT_H */
