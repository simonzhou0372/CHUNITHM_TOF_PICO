/*
 * Chuni245Tof Main Program
 * NKRO Keyboard with MPR121 Slider + VL53L0X Air
 *
 * 改进：
 * 1. 优化 HID 发送机制 - 使用 dirty flag，状态变化立即发送
 * 2. 修复 CONFIG 命令 - 支持 min_hold 参数
 * 3. 添加性能统计和 AIR 调试命令
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "hardware/timer.h"
#include "tusb.h"
#include "class/cdc/cdc_device.h"

#include "board_defs.h"
#include "config.h"
#include "save.h"
#include "mpr121.h"
#include "slider.h"
#include "vl53l0x.h"
#include "tof_reader.h"
#include "tof_events.h"
#include "air.h"
#include "button.h"
#include "log_output.h"   // printf 总开关 (默认禁用, 只发送 HID 报文)

using namespace Chuni245Tof;
extern "C" bool chuni_dcd_task(void);
extern "C" uint32_t chuni_dcd_timeout_count(void);
static bool usb_reinit_pending = false;
static uint64_t usb_reinit_at = 0;
static uint64_t bootloader_at = 0;

static void service_usb() {
#if CHUNI_FAULT_RECOVERY_ENABLE
    if (usb_reinit_pending) {
        if (time_us_64() >= usb_reinit_at) {
            tusb_init();
            usb_reinit_pending = false;
        }
        return;
    }
    if (!chuni_dcd_task()) {
        tud_deinit(0);
        usb_reinit_pending = true;
        usb_reinit_at = time_us_64() + 250000;
        return;
    }
#else
    (void)chuni_dcd_task(); // service hardware, never reinitialize USB
#endif
    tud_task_ext(0, false);
}

// Physical button pins
#define BUTTON_ENTER_PIN  18  // GP18 - Barrier mode 1 selection at startup
#define BUTTON_2_PIN      19  // GP19 - Barrier mode 0 selection at startup

// Onboard LED
#define LED_PIN 25

// Barrier mode selection buttons (startup only)
// 按照用户要求：
// - 不按按键上电 → 模式 2 (默认)
// - 按住 GP18 上电 → 模式 1
// - 按住 GP19 上电 → 模式 0 (优先级最高)

// Runtime barrier mode (determined at startup)
static uint8_t runtime_barrier_mode = 2;  // Default: mode 2

// NKRO Keyboard report
struct __attribute__((packed)) {
    uint8_t modifier;
    uint8_t keymap[15];
} hid_nkro, sent_hid_nkro;

// HID 状态管理
static volatile bool hid_dirty = false;  // 状态变化标志
static volatile uint64_t last_hid_send_time = 0;  // 上次发送时间
static volatile uint32_t hid_send_count = 0;  // HID 发送计数

// USB 活跃性统计（区分 "CPU 卡住" 与 "USB/电源层掉线" 的关键观测点）
static volatile uint64_t last_tud_task_us = 0;       // 上次 tud_task() 调用时刻
static volatile uint32_t max_tud_task_interval_us = 0;  // 两次 tud_task() 之间的最大间隔
static volatile uint32_t tud_task_count = 0;
static volatile uint32_t hid_ready_false_count = 0;  // HID endpoint busy 次数
static volatile uint64_t hid_busy_since_us = 0;      // 连续 not-ready 起点 (0 = endpoint ready)
static volatile uint32_t max_hid_not_ready_us = 0;   // 连续 not-ready 最长持续时间
static volatile uint32_t max_hid_send_gap_us = 0;    // 两次成功 HID 发送的最大间隔

//==============================================================================
// USB 健康状态 (部分关键指标可经 USBSTATUS 读取, 不依赖 printf)
//
// 用于区分四类故障 (发生 USB 无输入后逐项核对):
//   A. tud_mounted()==false        → 枚举/主机连接层问题 (unmount_count 增加)
//   B. mounted + suspended         → 主机 USB suspend (不是 HID 故障)
//   C. mounted + 非suspend + HID not-ready 持续很久 → endpoint 卡死
//      (max_hid_busy_us 增大, 超阈值触发受控重枚举 usb_recovery_count++)
//   D. mounted + HID ready 但发送失败 → 栈异常 (hid_send_fail_count 增加)
//==============================================================================
typedef struct {
    uint32_t mount_count;
    uint32_t unmount_count;
    uint32_t suspend_count;
    uint32_t resume_count;
    uint32_t hid_send_count;
    uint32_t hid_send_fail_count;
    uint32_t usb_recovery_count;    // 受控重枚举执行次数
    uint32_t max_tud_gap_us;        // 两次 tud_task() 的最大间隔
    uint32_t max_hid_busy_us;       // HID endpoint 连续 not-ready 的最长持续时间
    uint64_t last_mount_us;
    uint64_t last_unmount_us;
    uint64_t last_suspend_us;
    uint64_t last_resume_us;
    uint64_t last_hid_send_us;
} usb_health_t;

static usb_health_t usb_health;
static volatile bool usb_mounted = false;     // tud_mount_cb / tud_umount_cb 维护
static volatile bool usb_suspended = false;   // tud_suspend_cb / tud_resume_cb 维护
// IRQ only reads this byte, never a possibly torn 64-bit Core0 timestamp.
static volatile bool hid_stalled_for_led = false;

// ---- HID endpoint watchdog (仅 mounted + 非 suspended 时计时) ----
// ★ 自动恢复默认关闭 (CHUNI_USB_AUTO_RECOVERY_ENABLE=0) ★
// 原因: 恢复动作 (tud_disconnect/tud_connect) 本身就是一次 USB 断开重连,
// 主机侧表现与掉线完全相同 —— 自动恢复会把 "一次真死" 变成 "反复断连",
// 且误判条件 (如主机选择性挂起) 会造成周期性自断连。默认只统计不干预;
// 确认需要时把宏改为 1 再启用受控重枚举。
// Recovery switches are grouped in vl53l0x.h.

static const uint32_t HID_STALL_RECOVERY_MS = 5000;    // not-ready 持续阈值
static const uint32_t USB_RECOVERY_COOLDOWN_MS = 3000; // 两次受控重枚举最小间隔
static uint64_t hid_not_ready_since_us = 0;            // 本轮 not-ready 起点 (0 = ready)
static uint64_t last_usb_recovery_us = 0;
#if CHUNI_USB_AUTO_RECOVERY_ENABLE
static bool usb_disconnect_pending = false;            // disconnect 后等待延迟重连
static uint64_t usb_disconnect_at_us = 0;
#endif

// ---- 复位原因/启动计数 ----
// boot_count 放在 .uninitialized_data 段: SRAM 保留的复位可能保留此值。
// 断电/欠压后的 SRAM 不可靠, magic 不匹配时重新计数; 不能据此证明 brownout。
// USBSTATUS 的 wdt 才是 watchdog_caused_reboot() 读取的硬件复位证据。
#define BOOT_MAGIC 0x43485531u  // "CHU1"
static volatile uint32_t boot_magic __attribute__((section(".uninitialized_data")));
static volatile uint32_t boot_count __attribute__((section(".uninitialized_data")));
static bool reboot_by_watchdog = false;

// 性能统计
static volatile uint32_t main_loop_count = 0;
static volatile uint32_t main_loop_max_us = 0;
static volatile uint32_t main_loop_avg_us = 0;
static volatile uint64_t main_loop_last_time = 0;

// 监控模式
static volatile bool monitor_mode = false;
static volatile uint32_t monitor_interval_ms = 100;
static volatile uint64_t last_monitor_time = 0;
static volatile uint32_t monitor_truncated_count = 0; // CDC 背压导致被截断的 monitor 输出次数
static volatile uint32_t cdc_cmd_dropped_count = 0;   // CDC 背压导致被丢弃的命令条数

// Core0 各任务最大执行时间 (定位 "tud_task gap" 的真正来源; 低成本统计:
// 每任务一次 start/end + max 比较, 不产生任何输出)
static volatile uint32_t cdc_task_max_us = 0;
static volatile uint32_t slider_update_max_us = 0;
static volatile uint32_t air_update_max_us = 0;
static volatile uint32_t hid_task_max_us = 0;   // gen_nkro_report + report_usb_hid
static volatile uint32_t save_loop_max_us = 0;
static volatile uint32_t cmd_step_max_us = 0;    // 单条 CDC 命令执行耗时

// sent_hid_nkro 是否有效: USB 重枚举后置 false, 强制下一轮完整重发当前状态
// (用户无需重新按键 HID 就能恢复输出)
static bool sent_valid = false;

static char cdc_rx_buf[256];
static uint8_t cdc_rx_pos = 0;

// ---- CDC 命令队列 ----
// ★ cdc_task 只负责 "接收+解析+入队", 绝不在 CDC 路径执行命令。
//   执行由主循环 command_step() 完成, 每轮最多 1 条 —— 即使命令处理
//   路径出现未预期耗时, 也只会延后而不是累积阻塞主循环。
#define CDC_CMD_QUEUE_DEPTH 4
static char cdc_cmd_queue[CDC_CMD_QUEUE_DEPTH][256];
static volatile uint8_t cdc_cmd_q_head = 0;   // 生产者: cdc_task
static volatile uint8_t cdc_cmd_q_tail = 0;   // 消费者: command_step
static volatile uint32_t cdc_cmd_queue_full_count = 0;

// ---- 主循环 stage 面包屑 (硬锁现场留存) ----
// 写入 .uninitialized_data 段: 看门狗复位时保留, 断电后的值不可靠。
// 看门狗复位后从 prev_stage 即可读出卡死位置, 不再是 "黑盒重启"。
typedef enum {
    MAIN_STAGE_IDLE = 0,     // 上电未进入主循环
    MAIN_STAGE_TUD_TASK,     // TinyUSB 协议栈
    MAIN_STAGE_CDC_TASK,     // CDC 接收/解析/入队
    MAIN_STAGE_CMD_STEP,     // CDC 命令执行 (每轮最多 1 条)
    MAIN_STAGE_SLIDER,       // MPR121 轮转轮询 + 异步命令状态机
    MAIN_STAGE_AIR,          // ToF 快照消费
    MAIN_STAGE_BUTTON,       // 物理按钮
    MAIN_STAGE_HID,          // HID 报文生成/发送 + watchdog
    MAIN_STAGE_SAVE,         // 延迟 Flash 保存
    MAIN_STAGE_LOOP_END,     // 一轮完整结束 (喂狗点)
    MAIN_STAGE_SENSOR_INIT,  // 冷启动传感器初始化 (8s 看门狗保护)
} main_stage_t;

static volatile uint32_t last_main_stage __attribute__((section(".uninitialized_data")));
static volatile uint32_t last_stage_enter_us __attribute__((section(".uninitialized_data")));
static volatile uint32_t loop_completed_count __attribute__((section(".uninitialized_data")));

// 上一次生命周期 (看门狗复位前) 的现场快照, main() 开头捕获
static uint32_t crash_prev_stage = 0;
static uint32_t crash_prev_stage_us = 0;
static uint32_t crash_prev_loops = 0;

#define STAGE_ENTER(st) do { \
    last_main_stage = (uint32_t)(st); \
    last_stage_enter_us = time_us_32(); \
} while (0)

static const char* main_stage_name(uint32_t st) {
    switch (st) {
        case MAIN_STAGE_IDLE:     return "IDLE";
        case MAIN_STAGE_TUD_TASK: return "TUD_TASK";
        case MAIN_STAGE_CDC_TASK: return "CDC_TASK";
        case MAIN_STAGE_CMD_STEP: return "CMD_STEP";
        case MAIN_STAGE_SLIDER:   return "SLIDER";
        case MAIN_STAGE_AIR:      return "AIR";
        case MAIN_STAGE_BUTTON:   return "BUTTON";
        case MAIN_STAGE_HID:      return "HID";
        case MAIN_STAGE_SAVE:     return "SAVE";
        case MAIN_STAGE_LOOP_END: return "LOOP_END";
        case MAIN_STAGE_SENSOR_INIT: return "SENSOR_INIT";
        default:                  return "?";
    }
}

// HID Key codes (HID Usage Table)
#define HID_KEY_A       0x04
#define HID_KEY_B       0x05
#define HID_KEY_C       0x06
#define HID_KEY_D       0x07
#define HID_KEY_E       0x08
#define HID_KEY_F       0x09
#define HID_KEY_G       0x0A
#define HID_KEY_H       0x0B
#define HID_KEY_I       0x0C
#define HID_KEY_J       0x0D
#define HID_KEY_K       0x0E
#define HID_KEY_L       0x0F
#define HID_KEY_M       0x10
#define HID_KEY_N       0x11
#define HID_KEY_O       0x12
#define HID_KEY_P       0x13
#define HID_KEY_Q       0x14
#define HID_KEY_R       0x15
#define HID_KEY_S       0x16
#define HID_KEY_T       0x17
#define HID_KEY_U       0x18
#define HID_KEY_V       0x19
#define HID_KEY_W       0x1A
#define HID_KEY_X       0x1B
#define HID_KEY_Y       0x1C
#define HID_KEY_Z       0x1D
#define HID_KEY_1       0x1E
#define HID_KEY_2       0x1F
#define HID_KEY_3       0x20
#define HID_KEY_4       0x21
#define HID_KEY_5       0x22
#define HID_KEY_6       0x23
#define HID_KEY_7       0x24
#define HID_KEY_8       0x25
#define HID_KEY_9       0x26
#define HID_KEY_0       0x27
#define HID_KEY_ENTER   0x28
#define HID_KEY_LBRACKET  0x2F  // [
#define HID_KEY_RBRACKET  0x30  // ]
#define HID_KEY_BACKSLASH 0x31  // \
#define HID_KEY_SEMICOLON 0x33  // ;
#define HID_KEY_APOSTROPHE 0x34 // '
#define HID_KEY_COMMA   0x36
#define HID_KEY_PERIOD  0x37
#define HID_KEY_SLASH   0x38

// Slider keyboard mapping (Key 1-32)
// Hardware partition: 8/12/12 (matching MPR121 pin counts: 0x5C=8, 0x5B=12, 0x5A=12)
// Row 1 (cell 1-8):   Q W E R T Y U I
// Row 2 (cell 9-20):  O P [ ] a s d f g h j k
// Row 3 (cell 21-32): l ; ' \ z x c v b n m ,
static const uint8_t slider_keymap[32] = {
    HID_KEY_Q, HID_KEY_W, HID_KEY_E, HID_KEY_R,
    HID_KEY_T, HID_KEY_Y, HID_KEY_U, HID_KEY_I,

    HID_KEY_O, HID_KEY_P, HID_KEY_LBRACKET, HID_KEY_RBRACKET,
    HID_KEY_A, HID_KEY_S, HID_KEY_D, HID_KEY_F,
    HID_KEY_G, HID_KEY_H, HID_KEY_J, HID_KEY_K,

    HID_KEY_L, HID_KEY_SEMICOLON, HID_KEY_APOSTROPHE, HID_KEY_BACKSLASH,
    HID_KEY_Z, HID_KEY_X, HID_KEY_C, HID_KEY_V,
    HID_KEY_B, HID_KEY_N, HID_KEY_M, HID_KEY_COMMA,
};

// Air keyboard mapping (IR1-6: 4 5 6 7 8 9)
static const uint8_t air_keymap[6] = {
    HID_KEY_4,  // IR1
    HID_KEY_5,  // IR2
    HID_KEY_6,  // IR3
    HID_KEY_7,  // IR4
    HID_KEY_8,  // IR5
    HID_KEY_9,  // IR6
};

// Set key in NKRO bitmap
static void nkro_set_key(uint8_t keycode, bool pressed) {
    if (keycode >= 120) return;
    uint8_t byte = keycode / 8;
    uint8_t bit = keycode % 8;
    if (pressed) {
        hid_nkro.keymap[byte] |= (1 << bit);
    } else {
        hid_nkro.keymap[byte] &= ~(1 << bit);
    }
}

// Generate NKRO report
static void gen_nkro_report() {
    // 保存旧状态
    uint8_t old_keymap[15];
    uint8_t old_modifier = hid_nkro.modifier;
    memcpy(old_keymap, hid_nkro.keymap, sizeof(hid_nkro.keymap));

    // 清空新状态
    memset(&hid_nkro, 0, sizeof(hid_nkro));

    // Physical buttons (GP18=ENTER, GP19=2)
    if (!gpio_get(BUTTON_ENTER_PIN)) {
        nkro_set_key(HID_KEY_ENTER, true);
    }
    if (!gpio_get(BUTTON_2_PIN)) {
        nkro_set_key(HID_KEY_2, true);
    }

    // Slider keys (32 keys)
    uint32_t slider_state = slider_get_state();
    for (int i = 0; i < 32; i++) {
        if (slider_state & (1 << i)) {
            nkro_set_key(slider_keymap[i], true);
        }
    }

    // Air keys (6 height levels)
    uint8_t air_state = air_get_bitmap();
    for (int i = 0; i < 6; i++) {
        if (air_state & (1 << i)) {
            nkro_set_key(air_keymap[i], true);
        }
    }

    // 检查状态是否变化
    if (hid_nkro.modifier != old_modifier ||
        memcmp(hid_nkro.keymap, old_keymap, sizeof(hid_nkro.keymap)) != 0) {
        hid_dirty = true;  // 状态变化，需要发送
    }
}

// Send HID report
static void report_usb_hid() {
    if (usb_reinit_pending) return;
    uint64_t now = time_us_64();

    // 检查 USB 是否就绪（非阻塞 —— busy 时保持 dirty 标志, 状态继续更新,
    // endpoint 恢复后立即发送最新状态; 同时统计 busy 持续时间以识别
    // "endpoint 长期卡死" 而不是无限默默等待）
    if (!tud_hid_n_ready(0)) {
        hid_ready_false_count++;
        if (hid_busy_since_us == 0) {
            hid_busy_since_us = now;
        }
        uint32_t busy_us = (uint32_t)(now - hid_busy_since_us);
        if (busy_us > max_hid_not_ready_us) {
            max_hid_not_ready_us = busy_us;
        }
        return;
    }
    hid_busy_since_us = 0;   // 连续 busy 结束

    // 检查是否有状态变化
    // sent_valid=false (USB 重枚举后) 强制完整重发当前状态
    bool state_changed = !sent_valid ||
                         (memcmp(&hid_nkro, &sent_hid_nkro, sizeof(hid_nkro)) != 0);

    if (state_changed || hid_dirty) {
        // 状态变化立即发送; 或之前因 USB busy 延迟发送, 现在重试
        if (tud_hid_n_report(0, 0, &hid_nkro, sizeof(hid_nkro))) {
            uint32_t gap_us = (uint32_t)(now - last_hid_send_time);
            if (last_hid_send_time && gap_us > max_hid_send_gap_us) {
                max_hid_send_gap_us = gap_us;
            }
            sent_hid_nkro = hid_nkro;
            sent_valid = true;
            hid_dirty = false;
            last_hid_send_time = now;
            usb_health.last_hid_send_us = now;
            hid_send_count++;
            usb_health.hid_send_count++;
        } else {
            // ready 但 report 失败: HID 接口/设备栈异常 (故障分类 D)
            usb_health.hid_send_fail_count++;
            hid_dirty = true;
        }
    }
    // 如果状态未变化且没有 dirty，不发送（节省带宽）
}

//==============================================================================
// HID endpoint watchdog + 受控 USB 重枚举 (仅 Core0 主循环调用)
//
// mounted + 非 suspended + !tud_hid_n_ready(0) 持续超过 HID_STALL_RECOVERY_MS
// 才判定 transport 异常; 恢复手段是 TinyUSB 软断开/重连 (tud_disconnect 清除
// 上拉 → 主机看到物理拔出 → tud_connect 重新枚举), 不重启 MCU、不动
// tusb_init 生命周期、不碰任何 I2C/Flash。带冷却时间防循环。
//==============================================================================
#if CHUNI_FAULT_RECOVERY_ENABLE
static void usb_hid_watchdog() {
    if (usb_reinit_pending) {
        usb_mounted = false;
        usb_suspended = false;
        hid_stalled_for_led = false;
        hid_not_ready_since_us = 0;
        return;
    }
    uint64_t now = time_us_64();
    // Bus reset need not produce an umount callback; use stack state as truth.
    usb_mounted = tud_mounted();
    usb_suspended = tud_suspended();
    hid_stalled_for_led = false;

#if CHUNI_USB_AUTO_RECOVERY_ENABLE
    // 延迟重连: disconnect 后保持 250ms, 给主机足够的分离检测时间
    if (usb_disconnect_pending && now - usb_disconnect_at_us >= 250000) {
        usb_disconnect_pending = false;
        tud_connect();
    }
#endif

    if (!usb_mounted || usb_suspended) {
        hid_not_ready_since_us = 0;   // 非正常枚举状态: 不判 endpoint 卡死
        return;
    }

    if (!tud_hid_n_ready(0)) {
        if (hid_not_ready_since_us == 0) {
            hid_not_ready_since_us = now;
        }
        uint32_t busy_us = (uint32_t)(now - hid_not_ready_since_us);
        hid_stalled_for_led = busy_us > 100000;
        if (busy_us > usb_health.max_hid_busy_us) {
            usb_health.max_hid_busy_us = busy_us;
        }

#if CHUNI_USB_AUTO_RECOVERY_ENABLE
        if (now - hid_not_ready_since_us >= (uint64_t)HID_STALL_RECOVERY_MS * 1000) {
            if (now - last_usb_recovery_us >= (uint64_t)USB_RECOVERY_COOLDOWN_MS * 1000) {
                usb_health.usb_recovery_count++;
                last_usb_recovery_us = now;
                tud_disconnect();               // 受控软断开
                usb_disconnect_pending = true;
                usb_disconnect_at_us = now;
            }
            hid_not_ready_since_us = now;       // 重置窗口, 由重枚举后的状态决定
        }
#endif
    } else {
        hid_not_ready_since_us = 0;
    }
}
#endif


// ==============================================================================
// CDC 背压控制 (HID 是实时通道, CDC 是调试通道 —— CDC 绝不能阻塞 HID)
//
// stdio 的 USB CDC 后端在 "串口已连接但主机停止读取" 时, printf 会持续重试
// 写入直到 500ms 超时 —— 对实时 HID 是致命的。策略:
//   1. 长输出 (monitor JSON / ToF 事件) 先整体构建到缓冲区, 再按小块发送;
//   2. 每块发送前检查 tud_cdc_n_write_available —— 空间不足立即放弃本轮
//      剩余输出 (计数并跳过), 绝不等待;
//   3. 主机未连接 (DTR 断开) 时 stdio 本身不输出, 无阻塞。
// ==============================================================================

// 主机是否在读取: 已连接 且 TX 缓冲区有足够剩余空间
static bool cdc_output_ready(uint32_t need_bytes) {
    return !usb_reinit_pending && tud_cdc_connected() && tud_cdc_n_write_available(0) >= need_bytes;
}

// 受控输出一小块: 空间不足返回 false (调用者放弃剩余输出)
static bool cdc_write_gated(const char* s, uint32_t len) {
    if (!cdc_output_ready(len + 16)) return false;
    printf("%.*s", (int)len, s);
    return true;
}

// 输出监控数据（JSON格式，包含所有关键信息）
// 全部输出先构建到栈缓冲, 再分块经背压门控发送 —— CDC 拥塞时截断本轮,
// 绝不阻塞实时主循环
static void output_monitor_data() {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // 获取AIR调试数据
    air_debug_data_t air_debug = air_get_debug_data();

    // 获取Slider状态
    uint32_t slider_state = slider_get_state();

    // 一次性构建全部 JSON（有界缓冲, snprintf 保证不越界）
    static char buf[1024];
    int len = snprintf(buf, sizeof(buf),
        "{\r\n"
        "  \"t\": %lu,\r\n"
        "  \"slider\": %lu,\r\n"
        "  \"air\": {\"sensor\": %u, \"hid\": %u},\r\n"
        "  \"overlay\": %d,\r\n"
        "  \"tof\": [\r\n",
        (unsigned long)now,
        (unsigned long)slider_state,
        air_debug.sensor_bitmap,
        air_debug.hid_bitmap,
        cfg->air_overlay_enabled);
    for (int i = 0; i < 5 && len > 0 && len < (int)sizeof(buf); i++) {
        len += snprintf(buf + len, sizeof(buf) - len,
               "    {\"d\": %d, \"a\": %lu, \"v\": %d}%s\r\n",
               air_debug.sensor_distances[i],
               (unsigned long)air_debug.sensor_ages[i],
               air_debug.sensor_valid[i] ? 1 : 0,
               (i < 4) ? "," : "");
    }
    len += snprintf(buf + len, sizeof(buf) - len,
        "  ],\r\n"
        "  \"perf\": {\r\n"
        "    \"loop_avg\": %lu,\r\n"
        "    \"loop_max\": %lu,\r\n"
        "    \"tud_max_gap\": %lu,\r\n"
        "    \"hid_sends\": %lu,\r\n"
        "    \"hid_busy_max\": %lu,\r\n"
        "    \"hid_gap_max\": %lu,\r\n"
        "    \"tof_new\": %lu,\r\n"
        "    \"tof_poll_avg\": %lu,\r\n"
        "    \"tof_poll_max\": %lu,\r\n"
        "    \"core1_hb\": %lu,\r\n"
        "    \"stall_us\": %lu\r\n"
        "  }\r\n"
        "}\r\n---\r\n",
        (unsigned long)main_loop_avg_us,
        (unsigned long)main_loop_max_us,
        (unsigned long)max_tud_task_interval_us,
        (unsigned long)hid_send_count,
        (unsigned long)max_hid_not_ready_us,
        (unsigned long)max_hid_send_gap_us,
        (unsigned long)tof_reader_get_new_data_count(),
        (unsigned long)tof_reader_get_avg_poll_interval_us(),
        (unsigned long)tof_reader_get_max_poll_interval_us(),
        (unsigned long)tof_reader_get_heartbeat(),
        (unsigned long)tof_reader_get_last_stall_us());
    if (len < 0 || len >= (int)sizeof(buf)) len = sizeof(buf) - 1;

    // 分块发送（64B 小块: 每次 flush 恰好填满一个 EP 包, TX FIFO 不积压）
    uint32_t pos = 0;
    while (pos < (uint32_t)len) {
        uint32_t chunk = len - pos;
        if (chunk > 64) chunk = 64;
        if (!cdc_write_gated(buf + pos, chunk)) {
            monitor_truncated_count++;   // 背压: 放弃本轮剩余输出
            break;
        }
        pos += chunk;
    }
}

// Core1 ToF 事件输出: 每轮最多弹 4 条, 逐条经背压门控打印
static void tof_event_task() {
    static const char* const evt_names[] = {
        "NONE", "CORE1_STARTED", "INIT_FAIL", "READY",
        "BUDGET_VERIFY_FAIL", "OFFLINE", "RECOVERY_ATTEMPT",
        "RECOVERY_OK", "RECOVERY_FAIL", "BUS_RECOVERY_START",
        "BUS_RECOVERY_OK", "BUS_RECOVERY_FAIL", "FULL_REINIT_DONE",
        "STALL_NOTED",
    };

    tof_event_t ev;
    for (int i = 0; i < 4 && tof_event_pop(&ev); i++) {
        if (!cdc_output_ready(96)) break;   // CDC 拥塞: 事件留在队列, 下轮再取
        const char* name = (ev.type < sizeof(evt_names) / sizeof(evt_names[0]))
                               ? evt_names[ev.type] : "?";
        if (ev.sensor <= 4) {
            printf("[TOF %lu ms] TOF%u %s (arg=0x%08lX)\r\n",
                   (unsigned long)ev.timestamp_ms, ev.sensor + 1, name,
                   (unsigned long)ev.arg32);
        } else {
            printf("[TOF %lu ms] %s (lo=%u hi=%u arg=0x%08lX)\r\n",
                   (unsigned long)ev.timestamp_ms, name,
                   ev.arg16_lo, ev.arg16_hi, (unsigned long)ev.arg32);
        }
    }
}

// Always available, even with printf disabled. One bounded reply, no wait or
// retry if the host stops reading. All TinyUSB calls remain on Core0.
static void usb_status_reply() {
    static char reply[256];
    int len = snprintf(reply, sizeof(reply),
        "USB boot=%lu wdt=%u prev=%s loops=%lu core1=%lu gap_us=%lu "
        "mpr=%lu,%lu,%lu mount=%u suspend=%u sys_khz=%lu usb_abort=%lu rev=20260920\r\n",
        (unsigned long)boot_count, reboot_by_watchdog ? 1u : 0u,
        main_stage_name(crash_prev_stage), (unsigned long)crash_prev_loops,
        (unsigned long)tof_reader_get_heartbeat(),
        (unsigned long)max_tud_task_interval_us,
        (unsigned long)mpr121_get_error_count(0),
        (unsigned long)mpr121_get_error_count(1),
        (unsigned long)mpr121_get_error_count(2),
        tud_mounted() ? 1u : 0u, tud_suspended() ? 1u : 0u,
        (unsigned long)(clock_get_hz(clk_sys) / 1000),
        (unsigned long)chuni_dcd_timeout_count());
    if (len <= 0 || len >= (int)sizeof(reply) || !cdc_output_ready((uint32_t)len)) {
        return;
    }
    tud_cdc_n_write(0, reply, (uint32_t)len);
    tud_cdc_n_write_flush(0);
}

static void tof_status_reply() {
    static char reply[256];
    uint32_t ready = 0;
    for (int i = 0; i < 5; ++i) if (vl53l0x_is_ready(i)) ready |= 1u << i;
    int len = snprintf(reply, sizeof(reply),
        "TOF hb=%lu age_us=%lu stage=%lu job=%lu io=%lu ready=%02lx reset=%lu "
        "frames_fail=%lu err=%lu,%lu,%lu,%lu,%lu usb_abort=%lu data_ms=%lu,%lu,%lu,%lu,%lu\r\n",
        (unsigned long)tof_reader_get_heartbeat(), (unsigned long)tof_reader_get_progress_age_us(),
        (unsigned long)tof_reader_get_stage(), (unsigned long)vl53l0x_get_management_kind(),
        (unsigned long)vl53l0x_get_io_phase(), (unsigned long)ready,
        (unsigned long)vl53l0x_get_bus_resets(), (unsigned long)vl53l0x_get_frame_failures(),
        (unsigned long)vl53l0x_get_error_count(0), (unsigned long)vl53l0x_get_error_count(1),
        (unsigned long)vl53l0x_get_error_count(2), (unsigned long)vl53l0x_get_error_count(3),
        (unsigned long)vl53l0x_get_error_count(4), (unsigned long)chuni_dcd_timeout_count(),
        (unsigned long)tof_reader_get_age(0), (unsigned long)tof_reader_get_age(1),
        (unsigned long)tof_reader_get_age(2), (unsigned long)tof_reader_get_age(3),
        (unsigned long)tof_reader_get_age(4));
    if (len > 0 && len < (int)sizeof(reply) && cdc_output_ready((uint32_t)len)) {
        tud_cdc_n_write(0, reply, (uint32_t)len);
        tud_cdc_n_write_flush(0);
    }
}

// CDC command handler
static void cdc_process_command(const char* cmd) {
    if (strcmp(cmd, "TOFSTATUS") == 0) { tof_status_reply(); return; }
    if (strcmp(cmd, "USBSTATUS") == 0) {
        usb_status_reply();
        return;
    }
    // Parse CONFIG command: CONFIG touch release offset pitch [air12_range] [min_hold] [overlay]
    if (strncmp(cmd, "CONFIG ", 7) == 0) {
        int touch, release, offset, pitch, air12_range, min_hold, overlay;
        int num_args = sscanf(cmd + 7, "%d %d %d %d %d %d %d",
                             &touch, &release, &offset, &pitch, &air12_range, &min_hold, &overlay);

        if (num_args >= 4) {  // 至少 4 个参数
            // 阈值范围已解除限制，允许调试
            // 注意：touch_threshold 必须大于 release_threshold（迟滞区）
            // 如果违反，mpr121_set_thresholds() 会自动修正并更新 cfg
            if (touch >= 1 && touch <= 255) {
                cfg->touch_threshold = touch;
            }
            if (release >= 1 && release <= 255) {
                cfg->release_threshold = release;
            }
            // 立即应用阈值到 MPR121
            mpr121_set_thresholds(cfg->touch_threshold, cfg->release_threshold);
            if (offset >= 40 && offset <= 200) {
                cfg->tof_offset = offset;
            }
            if (pitch >= 4 && pitch <= 100) {
                cfg->tof_pitch = pitch;
            }
            // Air12 range (可选，默认 150)
            if (num_args >= 5 && air12_range >= pitch && air12_range <= 255) {
                cfg->air12_range = air12_range;
            }
            // Min hold time (可选，默认 100)
            if (num_args >= 6 && min_hold >= 10 && min_hold <= 500) {
                cfg->air_min_hold_ms = min_hold;
            }
            // Overlay mode (可选，默认 1)
            if (num_args >= 7 && (overlay == 0 || overlay == 1)) {
                cfg->air_overlay_enabled = overlay;
            }

            printf("OK touch=%d release=%d offset=%d pitch=%d air12=%d hold=%d overlay=%d\r\n",
                   cfg->touch_threshold, cfg->release_threshold,
                   cfg->tof_offset, cfg->tof_pitch, cfg->air12_range, cfg->air_min_hold_ms, cfg->air_overlay_enabled);
        } else {
            printf("ERROR Usage: CONFIG touch release offset pitch [air12_range] [min_hold] [overlay]\r\n");
        }
    }
    else if (strcmp(cmd, "CONFIG?") == 0) {
        printf("CONFIG touch=%d release=%d offset=%d pitch=%d air12=%d hold=%d overlay=%d\r\n",
               cfg->touch_threshold, cfg->release_threshold,
               cfg->tof_offset, cfg->tof_pitch, cfg->air12_range, cfg->air_min_hold_ms, cfg->air_overlay_enabled);
    }
    else if (strcmp(cmd, "SAVE") == 0) {
        if (config_save()) {
            printf("SAVE OK\r\n");
        } else {
            printf("SAVE ERROR\r\n");
        }
    }
    else if (strcmp(cmd, "DEFAULT") == 0) {
        // Reset to default values (does NOT save to Flash)
        // To persist defaults, user must call SAVE after DEFAULT
        // Use runtime barrier mode for default thresholds
        uint8_t current_mode = config_get_barrier_mode();
        uint8_t default_touch, default_release;

        // 根据 barrier mode 设置默认阈值（严格匹配 mpr121.h 定义）
        if (current_mode == 0) {
            default_touch = 20;
            default_release = 18;
        } else {
            // Mode 1 和 Mode 2 都使用 3/2
            default_touch = 3;
            default_release = 2;
        }

        cfg->touch_threshold = default_touch;
        cfg->release_threshold = default_release;
        cfg->tof_offset = 120;
        cfg->tof_pitch = 30;
        cfg->air12_range = 150;
        cfg->air_min_hold_ms = 100;
        cfg->air_overlay_enabled = 1;  // Default: Overlay ON
        mpr121_set_thresholds(default_touch, default_release);

        printf("DEFAULT OK barrier_mode=%d touch=%d release=%d offset=120 pitch=30 air12=150 hold=100 overlay=1\r\n",
               current_mode, default_touch, default_release);
        printf("NOTE: Defaults applied to RAM. Use SAVE to persist to Flash.\r\n");
    }
    else if (strcmp(cmd, "STATUS") == 0) {
        // 增强的 STATUS 输出
        air_debug_data_t air_debug = air_get_debug_data();

        printf("{\r\n");
        printf("  \"slider\": \"0x%08lX\",\r\n", slider_get_state());
        printf("  \"air\": \"0x%03X\",\r\n", air_get_air_state());  // 12-bit
        printf("  \"air_hid\": \"0x%02X\",\r\n", air_get_bitmap()); // 6-bit HID
        printf("  \"overlay\": %d,\r\n", cfg->air_overlay_enabled);
        printf("  \"barrier_mode\": %d,\r\n", config_get_barrier_mode());

        // 添加 TOF 传感器数据
        printf("  \"tof\": [\r\n");
        for (int i = 0; i < 5; i++) {
            printf("    {\"d\": %d, \"a\": %lu, \"v\": %d}%s\r\n",
                   air_debug.sensor_distances[i],
                   air_debug.sensor_ages[i],
                   air_debug.sensor_valid[i] ? 1 : 0,
                   (i < 4) ? "," : "");
        }
        printf("  ],\r\n");

        printf("  \"perf\": {\r\n");
        printf("    \"loop_avg_us\": %lu,\r\n", main_loop_avg_us);
        printf("    \"loop_max_us\": %lu,\r\n", main_loop_max_us);
        printf("    \"tud_max_gap_us\": %lu,\r\n", max_tud_task_interval_us);
        printf("    \"hid_sends\": %lu,\r\n", hid_send_count);
        printf("    \"hid_busy_max_us\": %lu,\r\n", max_hid_not_ready_us);
        printf("    \"hid_gap_max_us\": %lu,\r\n", max_hid_send_gap_us);
        printf("    \"tof_new_data\": %lu,\r\n", tof_reader_get_new_data_count());
        printf("    \"tof_poll_avg_us\": %lu,\r\n", tof_reader_get_avg_poll_interval_us());
        printf("    \"tof_poll_max_us\": %lu,\r\n", tof_reader_get_max_poll_interval_us());
        printf("    \"core1_hb\": %lu,\r\n", tof_reader_get_heartbeat());
        printf("    \"stall_us\": %lu\r\n", tof_reader_get_last_stall_us());
        printf("  },\r\n");
        printf("  \"i2c_err\": {\r\n");
        printf("    \"mpr\": [%lu, %lu, %lu],\r\n",
               mpr121_get_error_count(0), mpr121_get_error_count(1), mpr121_get_error_count(2));
        printf("    \"tof\": [%lu, %lu, %lu, %lu, %lu]\r\n",
               vl53l0x_get_error_count(0), vl53l0x_get_error_count(1), vl53l0x_get_error_count(2),
               vl53l0x_get_error_count(3), vl53l0x_get_error_count(4));
        printf("  },\r\n");
        printf("  \"usb\": {\r\n");
        printf("    \"mounted\": %d,\r\n", usb_mounted ? 1 : 0);
        printf("    \"suspended\": %d,\r\n", usb_suspended ? 1 : 0);
        printf("    \"mount\": %lu, \"unmount\": %lu,\r\n",
               (unsigned long)usb_health.mount_count, (unsigned long)usb_health.unmount_count);
        printf("    \"suspend\": %lu, \"resume\": %lu,\r\n",
               (unsigned long)usb_health.suspend_count, (unsigned long)usb_health.resume_count);
        printf("    \"hid_send\": %lu, \"hid_send_fail\": %lu,\r\n",
               (unsigned long)usb_health.hid_send_count, (unsigned long)usb_health.hid_send_fail_count);
        printf("    \"recovery\": %lu, \"max_hid_busy_us\": %lu,\r\n",
               (unsigned long)usb_health.usb_recovery_count, (unsigned long)usb_health.max_hid_busy_us);
        printf("    \"max_tud_gap_us\": %lu,\r\n", (unsigned long)usb_health.max_tud_gap_us);
        printf("    \"boot_count\": %lu, \"wdt_reboot\": %d, \"cmdq_full\": %lu\r\n",
               (unsigned long)boot_count, reboot_by_watchdog ? 1 : 0,
               (unsigned long)cdc_cmd_queue_full_count);
        printf("  },\r\n");
        printf("  \"crash\": {\r\n");
        printf("    \"prev_stage\": \"%s\" (%lu), \"prev_stage_us\": %lu,\r\n",
               main_stage_name(crash_prev_stage), (unsigned long)crash_prev_stage,
               (unsigned long)crash_prev_stage_us);
        printf("    \"prev_loops\": %lu, \"loops_total\": %lu\r\n",
               (unsigned long)crash_prev_loops, (unsigned long)loop_completed_count);
        printf("  },\r\n");
        printf("  \"task_max_us\": {\r\n");
        printf("    \"cdc\": %lu, \"cmd\": %lu, \"slider\": %lu, \"air\": %lu, \"hid\": %lu, \"save\": %lu\r\n",
               (unsigned long)cdc_task_max_us, (unsigned long)cmd_step_max_us,
               (unsigned long)slider_update_max_us,
               (unsigned long)air_update_max_us, (unsigned long)hid_task_max_us,
               (unsigned long)save_loop_max_us);
        printf("  },\r\n");
        printf("  \"save\": {\"count\": %lu, \"fail\": %lu, \"max_us\": %lu}\r\n",
               (unsigned long)save_get_count(), (unsigned long)save_get_fail_count(),
               (unsigned long)save_get_max_duration_us());
        printf("}\r\n");
    }
    else if (strcmp(cmd, "AIRDEBUG") == 0) {
        // 输出 AIR 调试数据
        air_debug_data_t debug = air_get_debug_data();
        printf("AIR Debug:\r\n");
        printf("  max_dist: %d mm (debug only)\r\n", debug.max_distance);
        printf("  sensor_bmp: 0x%03X (", debug.sensor_bitmap);
        for (int i = 0; i < 12; i++) {
            printf("%d", (debug.sensor_bitmap >> i) & 1);
            if (i == 5) printf(" ");  // Separate AIR1-6 and AIR7-12
        }
        printf(")\r\n");
        printf("  hid_bmp: 0x%02X (", debug.hid_bitmap);
        for (int i = 0; i < 6; i++) {
            printf("%d", (debug.hid_bitmap >> i) & 1);
        }
        printf(")\r\n");
        printf("  Per-TOF AIR layers:\r\n");
        for (int i = 0; i < 5; i++) {
            int layer = debug.sensor_air_layer[i];
            printf("    TOF%d: dist=%d mm, age=%lu ms, valid=%d, layer=%s\r\n",
                   i + 1, debug.sensor_distances[i], debug.sensor_ages[i],
                   debug.sensor_valid[i] ? 1 : 0,
                   (layer == 0xFF) ? "NONE" : (layer < 12 ? "AIR?" : "?"));
            if (layer < 12) {
                // 打印 AIR 层名称
                printf("         → AIR%d\r\n", layer + 1);
            }
        }
    }
    else if (strcmp(cmd, "PERF") == 0) {
        // 输出性能统计
        printf("Performance Statistics:\r\n");
        printf("  Main loop:\r\n");
        printf("    avg: %lu us\r\n", main_loop_avg_us);
        printf("    max: %lu us\r\n", main_loop_max_us);
        printf("    count: %lu\r\n", main_loop_count);
        printf("  USB servicing:\r\n");
        printf("    tud_task calls: %lu\r\n", tud_task_count);
        printf("    max tud_task gap: %lu us\r\n", max_tud_task_interval_us);
        printf("  HID:\r\n");
        printf("    sends: %lu\r\n", hid_send_count);
        printf("    ready-false count: %lu\r\n", hid_ready_false_count);
        printf("    max not-ready streak: %lu us\r\n", max_hid_not_ready_us);
        printf("    max send gap: %lu us\r\n", max_hid_send_gap_us);
        printf("  CDC backpressure:\r\n");
        printf("    monitor truncated: %lu\r\n", monitor_truncated_count);
        printf("    commands dropped: %lu\r\n", cdc_cmd_dropped_count);
        printf("    tof events dropped: %lu\r\n", tof_event_dropped_count());
        printf("  Recovery:\r\n");
        printf("    enabled: %d\r\n", CHUNI_TOF_RECOVERY_ENABLE);
        printf("  Core1 TOF:\r\n");
        printf("    new_data: %lu\r\n", tof_reader_get_new_data_count());
        printf("    poll_avg: %lu us\r\n", tof_reader_get_avg_poll_interval_us());
        printf("    poll_max: %lu us\r\n", tof_reader_get_max_poll_interval_us());
        printf("    core1_heartbeat: %lu\r\n", tof_reader_get_heartbeat());
        printf("    last_stall: %lu us\r\n", tof_reader_get_last_stall_us());
        printf("  USB lifecycle:\r\n");
        printf("    mounted/suspended: %d/%d\r\n", usb_mounted ? 1 : 0, usb_suspended ? 1 : 0);
        printf("    mount/unmount: %lu/%lu\r\n",
               (unsigned long)usb_health.mount_count, (unsigned long)usb_health.unmount_count);
        printf("    suspend/resume: %lu/%lu\r\n",
               (unsigned long)usb_health.suspend_count, (unsigned long)usb_health.resume_count);
        printf("    hid send/fail: %lu/%lu\r\n",
               (unsigned long)usb_health.hid_send_count, (unsigned long)usb_health.hid_send_fail_count);
        printf("    usb recoveries: %lu\r\n", (unsigned long)usb_health.usb_recovery_count);
        printf("    max hid busy: %lu us\r\n", (unsigned long)usb_health.max_hid_busy_us);
        printf("    boot count: %lu (wdt reboot: %d)\r\n",
               (unsigned long)boot_count, reboot_by_watchdog ? 1 : 0);
        printf("  Per-task max (us):\r\n");
        printf("    cdc=%lu slider=%lu air=%lu hid=%lu save=%lu\r\n",
               (unsigned long)cdc_task_max_us, (unsigned long)slider_update_max_us,
               (unsigned long)air_update_max_us, (unsigned long)hid_task_max_us,
               (unsigned long)save_loop_max_us);
        printf("  Flash save:\r\n");
        printf("    count/fail: %lu/%lu, max duration: %lu us\r\n",
               (unsigned long)save_get_count(), (unsigned long)save_get_fail_count(),
               (unsigned long)save_get_max_duration_us());
    }
    else if (strcmp(cmd, "SLIDER") == 0) {
        printf("Raw slider state: 0x%08lX\r\n", slider_get_state());
        printf("Cells pressed: ");
        uint32_t state = slider_get_state();
        for (int i = 0; i < 32; i++) {
            if (state & (1 << i)) {
                printf("%d ", i + 1);
            }
        }
        printf("\r\n");
    }
    else if (strcmp(cmd, "MPR") == 0) {
        printf("MPR121 raw data:\r\n");
        printf("  0x5A (cell 21-32): 0x%08lX\r\n", mpr121_get_touch_state(0));
        printf("  0x5B (cell 9-20):  0x%08lX\r\n", mpr121_get_touch_state(1));
        printf("  0x5C (cell 1-8):   0x%08lX\r\n", mpr121_get_touch_state(2));
    }
    else if (strcmp(cmd, "DIST") == 0) {
        printf("TOF distances (mm):");
        for (int i = 0; i < 5; i++) {
            printf(" %d", air_get_distance(i));
        }
        printf("\r\n");
    }
    else if (strcmp(cmd, "AIR") == 0) {
        printf("Air bitmap: 0x%02X (IR1-6: ", air_get_bitmap());
        for (int i = 0; i < 6; i++) {
            printf("%d", (air_get_bitmap() >> i) & 1);
        }
        printf(")\r\n");
    }
    else if (strcmp(cmd, "I2CSCAN") == 0) {
        printf("MPR121: ");
        for (int i = 0; i < 3; i++) {
            printf("0x%02X ", 0x5A + i);
        }
        printf("\r\nVL53L0X: ");
        for (int i = 0; i < 5; i++) {
            if (vl53l0x_is_ready(i)) {
                printf("OK ");
            } else {
                printf("X ");
            }
        }
        printf("\r\n");
    }
    else if (strcmp(cmd, "BOOTLOADER") == 0) {
        printf("OK\r\n");
        bootloader_at = time_us_64() + 100000;
    }
    else if (strcmp(cmd, "DEBUG") == 0) {
        // 打印 MPR121 调试信息: Baseline / FilteredData / Delta
        mpr121_debug_print();
    }
    else if (strcmp(cmd, "RESET") == 0) {
        // 重置 MPR121 Baseline
        mpr121_reset_baseline();
    }
    else if (strcmp(cmd, "HELP") == 0) {
        printf("Commands:\r\n");
        printf("  CONFIG touch release offset pitch [air6_range] [min_hold]\r\n");
        printf("    touch/release: 1-255 (no limit for debugging)\r\n");
        printf("    note: touch must be > release (auto-corrected if not)\r\n");
        printf("  CONFIG?\r\n");
        printf("  SAVE (deferred, executes on next loop)\r\n");
        printf("  DEFAULT\r\n");
        printf("  STATUS\r\n");
        printf("  AIRDEBUG\r\n");
        printf("  PERF (incl. USB/HID/CDC/Core1 stats)\r\n");
        printf("  START_MONITOR <interval_ms>\r\n");
        printf("  STOP_MONITOR\r\n");
        printf("  SLIDER, MPR, DEBUG, RESET\r\n");
        printf("  DIST, AIR, I2CSCAN\r\n");
        printf("  BOOTLOADER, HELP\r\n");
    }
    else if (strncmp(cmd, "START_MONITOR", 13) == 0) {
        // 解析参数: START_MONITOR <interval_ms>
        int interval = 100;
        int num_args = sscanf(cmd + 13, "%d", &interval);

        if (num_args >= 1) {
            // 限制范围: 50-5000ms
            if (interval < 50) interval = 50;
            if (interval > 5000) interval = 5000;

            monitor_interval_ms = interval;
            monitor_mode = true;
            last_monitor_time = time_us_64();

            printf("OK Monitor started (interval=%d ms)\r\n", monitor_interval_ms);
        } else {
            printf("ERROR Usage: START_MONITOR <interval_ms>\r\n");
        }
    }
    else if (strcmp(cmd, "STOP_MONITOR") == 0) {
        monitor_mode = false;
        printf("OK Monitor stopped\r\n");
    }
}

// CDC task - Non-blocking
// 每轮主循环最多执行 1 条 CDC 命令 (从队列取队头)
static void command_step() {
    if (cdc_cmd_q_head == cdc_cmd_q_tail) return;   // 队列空
    char cmd[256];
    strcpy(cmd, cdc_cmd_queue[cdc_cmd_q_tail]);
    cdc_cmd_q_tail = (uint8_t)((cdc_cmd_q_tail + 1) % CDC_CMD_QUEUE_DEPTH);
    cdc_process_command(cmd);
}

static void cdc_task() {
    if (usb_reinit_pending) return;
    // Limit the number of bytes processed per call to avoid blocking
    // 如果CDC有大量数据，每轮最多处理64字节
    const int MAX_CDC_READ_PER_LOOP = 64;
    // 单次 cdc_task 执行时间上限 (us): 即使主机突发大量数据也不挤压实时任务
    const uint64_t MAX_CDC_TASK_US = 1000;
    int bytes_read = 0;
    uint64_t deadline = time_us_64() + MAX_CDC_TASK_US;

    while (tud_cdc_available() && bytes_read < MAX_CDC_READ_PER_LOOP) {
        char c = tud_cdc_read_char();
        bytes_read++;

        if (c == '\r' || c == '\n') {
            if (cdc_rx_pos > 0) {
                cdc_rx_buf[cdc_rx_pos] = '\0';
                // ★ 只入队, 不执行 (旧实现: 在 CDC 解析路径同步执行
                //   CONFIG/DEFAULT/RESET/DEBUG 等长命令, 阻塞主循环
                //   数百 ms ~ 秒级)。执行交给主循环 command_step()。
                uint8_t next = (uint8_t)((cdc_cmd_q_head + 1) % CDC_CMD_QUEUE_DEPTH);
                if (next == cdc_cmd_q_tail) {
                    cdc_cmd_queue_full_count++;
                } else {
                    strcpy(cdc_cmd_queue[cdc_cmd_q_head], cdc_rx_buf);
                    cdc_cmd_q_head = next;
                }
                cdc_rx_pos = 0;
            }
        } else if (cdc_rx_pos < sizeof(cdc_rx_buf) - 1) {
            cdc_rx_buf[cdc_rx_pos++] = c;
        }

        if (time_us_64() > deadline) break;   // 执行时间上限, 保证循环节拍
    }
}

// NKRO report: 1 modifier + 15 keymap bytes = 16 (与 HID report descriptor
// 的 8+120 bit Input 定义、endpoint 64B 上限三者一致)
static_assert(sizeof(hid_nkro) == 16, "NKRO report must be 16 bytes");

// ===== LED 生命信号 (硬件定时器 IRQ, 与主循环解耦) =====
// 10ms tick. 优先级从高到低:
//   常亮         : 主循环卡死 (某 stage 停留 >50ms; 正常每 ~1ms 刷新)
//   100ms 快闪   : USB 未 mounted (枚举/连接层)
//   1000ms 慢闪  : USB suspended (主机挂起)
//   250ms 闪     : HID endpoint not-ready 持续 >100ms
//   500ms 心跳   : 一切正常
#define LED_TICK_US 10000
static repeating_timer_t led_timer;
static volatile uint32_t led_tick_count = 0;

static bool led_timer_cb(repeating_timer_t *rt) {
    (void)rt;
    led_tick_count++;

    // 卡死判定: 未到达 LOOP_END 且单 stage 停留 >50ms
    // (uint32 减法天然容忍 time_us_32 回绕)
#if CHUNI_FAULT_RECOVERY_ENABLE
    bool loop_stalled = (last_main_stage != (uint32_t)MAIN_STAGE_LOOP_END) &&
                        ((time_us_32() - last_stage_enter_us) > 50000);

    bool on;
    if (loop_stalled) {
        on = true;                          // 常亮: Core0 卡死证据
    } else if (!usb_mounted) {
        on = (led_tick_count % 10) < 5;     // 100ms 快闪
    } else if (usb_suspended) {
        on = (led_tick_count % 100) < 50;   // 1000ms 慢闪
    } else if (hid_stalled_for_led) {
        on = (led_tick_count % 25) < 13;    // 250ms 闪
    } else {
        on = (led_tick_count % 50) < 25;    // 500ms 心跳
    }


#else
    bool on = (led_tick_count % 50) < 25; // heartbeat only
#endif
    static bool last_on = false;
    if (on != last_on) {
        gpio_put(LED_PIN, on ? 1 : 0);
        last_on = on;
    }
    return true;   // 继续周期触发
}

int main(void) {
    // Use Pico's conservative 125 MHz clock during stability qualification.
    // I2C baud rates and sensor timing budgets are configured independently.
    set_sys_clock_khz(125000, true);
    // 注意: 不调用 stdio_init_all() —— 本工程 stdio 输出后端全部关闭
    // (CMakeLists: pico_enable_stdio_usb/uart = 0), USB device 栈的唯一
    // 所有者是 tusb_init() 的 TinyUSB 生命周期。

    // ===== 复位原因 / 启动计数 (SRAM 保留时有效, magic 校验) =====
    bool retained_state_valid = boot_magic == BOOT_MAGIC;
    if (retained_state_valid) {
        boot_count++;
    } else {
        boot_magic = BOOT_MAGIC;
        boot_count = 1;
    }
    reboot_by_watchdog = watchdog_caused_reboot();

    // 捕获上一次生命周期的硬锁现场 (看门狗复位时 last_main_stage 即
    // 卡死位置)。断电冷启动时 .uninitialized_data 为随机值, 仅在
    // wdt_reboot=1 时解读。
    if (retained_state_valid && reboot_by_watchdog) {
        crash_prev_stage = last_main_stage;
        crash_prev_stage_us = last_stage_enter_us;
        crash_prev_loops = loop_completed_count;
    }
    loop_completed_count = 0;
    // 为本次生命周期初始化面包屑 (冷启动时 .uninitialized_data 为随机值,
    // 若不初始化, LED stage-stall 判定会在进入主循环前误报常亮)
    last_main_stage = (uint32_t)MAIN_STAGE_IDLE;
    last_stage_enter_us = time_us_32();

    // Initialize LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);

    // ===== BARRIER MODE SELECTION (before any other initialization) =====
    // 初始化 GP18 和 GP19 用于启动时检测
    gpio_init(BUTTON_ENTER_PIN);  // GP18
    gpio_set_dir(BUTTON_ENTER_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_ENTER_PIN);

    gpio_init(BUTTON_2_PIN);      // GP19
    gpio_set_dir(BUTTON_2_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_2_PIN);

    // 等待 pull-up 稳定 (RP2040 内部 pull-up 约 50kΩ，需要时间充电线路电容)
    busy_wait_us(100);

    // 读取按键状态（active low）
    bool gp18_pressed = !gpio_get(BUTTON_ENTER_PIN);  // 按住 GP18 → 模式 1
    bool gp19_pressed = !gpio_get(BUTTON_2_PIN);      // 按住 GP19 → 模式 0

    // 确定 barrier mode（GP19 优先级高于 GP18）
    if (gp19_pressed) {
        runtime_barrier_mode = 0;  // Mode 0: 直接接触
    } else if (gp18_pressed) {
        runtime_barrier_mode = 1;  // Mode 1: 物体间隔
    } else {
        runtime_barrier_mode = 2;  // Mode 2: 物体间隔+手套 (默认)
    }

    printf("\n========================================\n");
    printf("   Chuni245Tof Controller\r\n");
    printf("   Optimized for High-Speed Air Detection\r\n");
    printf("========================================\n");

    // Print barrier mode selection
    printf("\n[STARTUP] Barrier Mode Selection:\n");
    printf("  GP18: %s, GP19: %s\n",
           gp18_pressed ? "PRESSED" : "released",
           gp19_pressed ? "PRESSED" : "released");
    printf("  Selected Mode: %d", runtime_barrier_mode);
    if (runtime_barrier_mode == 0) {
        printf(" (Direct contact - Touch=20 Release=18 CONFIG1=0x35 CONFIG2=0x02)\n");
    } else if (runtime_barrier_mode == 1) {
        printf(" (Object barrier - Touch=3 Release=2 CONFIG1=0x35 CONFIG2=0x22)\n");
    } else {
        printf(" (Object barrier + glove - Touch=3 Release=2 CONFIG1=0x25 CONFIG2=0x22)\n");
    }
    printf("\n");

    static mutex_t lock;
    mutex_init(&lock);

    // ===== Set barrier mode before config_init =====
    // This affects default touch/release thresholds and MPR121 CONFIG registers
    config_set_barrier_mode(runtime_barrier_mode);

    // ===== Initialize configuration =====
    // This will load from Flash or use defaults
    // The defaults will use runtime_barrier_mode for touch/release thresholds
    config_init();
    save_init(0xCA34CAFE, &lock);

    printf("Initializing sensors...\r\n");
    // Cover initialization too: otherwise a fault after watchdog reset can
    // leave the LED lit forever before the runtime watchdog is enabled.
#if CHUNI_FAULT_RECOVERY_ENABLE
    watchdog_enable(8000, true);
#endif
    STAGE_ENTER(MAIN_STAGE_SENSOR_INIT);
    button_init();
    slider_init();  // This will call mpr121_init() with barrier mode settings
#if DEBUG_MPR121
    mpr121_debug_init();  // one-shot config dump after MPR121 init
#endif
    // I2C1 由 Core1 独占: VL53L0X 初始化与轮询全部在 Core1 上执行
    // (air_init → tof_reader_init → Core1 启动 vl53l0x_init)
    air_init();      // 启动 Core 1 读取任务（含 I2C1 + VL53L0X 初始化）
    // Both sensor initializations are scheduled, not waited on. Core0 can now
    // service enumeration while their reset/settling deadlines elapse.
    tusb_init();
    printf("Ready.\r\n\n");

    printf("Commands: STATUS, AIRDEBUG, PERF, DIST, AIR, HELP\r\n\n");

    main_loop_last_time = time_us_64();

    /*
     * ==============================================================================
     * Core 0 主循环架构
     * ==============================================================================
     *
     * 【实时任务优先级】
     * ⭐⭐⭐⭐⭐  TinyUSB tud_task()    - USB协议栈，必须持续运行
     * ⭐⭐⭐⭐⭐  USB HID              - 必须持续发送，不能长时间阻塞
     * ⭐⭐⭐⭐⭐  MPR121 Slider        - 更高优先级实时输入，但故障不能拖死Core 0
     * ⭐⭐⭐⭐    AIR                  - 高实时性输入
     * ⭐        CDC/Monitor          - 低优先级调试接口
     * ⭐        Flash Save           - 仅在SAVE命令时执行
     *
     * 【关键原则】
     * 1. MPR121正常时：必须优先保证其扫描频率和实时性
     * 2. MPR121异常时：快速失败，不阻塞AIR和USB HID
     * 3. AIR必须完全独立于MPR121，即使MPR121全部失败，AIR仍正常运行
     * 4. 任何单个实时任务的最坏执行时间必须有界（<5ms）
     * 5. USB HID不能被任何输入设备长时间阻塞
     *
     * 【架构改进】
     * - MPR121 I2C使用极短timeout（500us），失败立即返回
     * - CDC任务限制每次处理的字节数，避免大量数据阻塞
     * - Monitor输出按间隔调用，不会频繁阻塞
     * - Flash操作只在SAVE命令时执行，不在主循环中
     *
     * ==============================================================================
     */
    // ===== Core0 硬锁看门狗 =====
    // 任何主循环 stage 永久卡死 (未知的 while/mutex/sleep 路径, 状态机
    // 卡住等) 最多 2s 后强制复位; 复位后 crash_prev_stage 给出卡死位置。
    // 冷启动的 8s 看门狗在此收紧为 2s; 主循环每轮末尾喂狗一次。
    // pause_on_debug=1: gdb 断点暂停时不误触发。
#if CHUNI_FAULT_RECOVERY_ENABLE
    watchdog_enable(2000, 1);
#endif

    // ===== LED 生命信号: 硬件定时器 IRQ (与主循环完全解耦) =====
    // 旧实现 LED 在主循环内更新 —— 主循环一死 LED 一起冻结, 失去
    // 观测价值 (用户实测 "USB 无输入时 LED 也停止变化" 即此缺陷)。
    // 新实现: 10ms 硬件定时器中断更新 LED, 主循环卡死时由 stage-stall
    // 判定点亮 (常亮 = 卡死证据)。IRQ 处理器仅做比较 + gpio_put, 微秒级。
    add_repeating_timer_us(LED_TICK_US, led_timer_cb, NULL, &led_timer);

    while (1) {
        if (bootloader_at && time_us_64() >= bootloader_at) reset_usb_boot(0, 0);
        // 性能统计：记录循环开始时间
        uint64_t loop_start = time_us_64();

        // ===== USB 服务间隔统计 =====
        // 真正决定 USB 稳定性的是两次 tud_task() 之间的最大空洞:
        // 主循环任何阻塞路径都会直接体现在该统计上
        uint32_t tud_interval_us = (uint32_t)(loop_start - last_tud_task_us);
        if (last_tud_task_us && tud_interval_us > max_tud_task_interval_us) {
            max_tud_task_interval_us = tud_interval_us;
            usb_health.max_tud_gap_us = tud_interval_us;
        }
        last_tud_task_us = loop_start;

        // ===== TinyUSB Task（最高优先级，必须持续运行）=====
        STAGE_ENTER(MAIN_STAGE_TUD_TASK);
        service_usb();
        tud_task_count++;

        // ===== Slider更新（更高优先级实时输入，非阻塞）=====
        STAGE_ENTER(MAIN_STAGE_SLIDER);
        // 推进 MPR121 的一个 I2C 步骤；FIFO/ACK/STOP 等待跨主循环完成。
        { uint64_t t0 = time_us_64();
        slider_update();
        uint32_t d = (uint32_t)(time_us_64() - t0); if (d > slider_update_max_us) slider_update_max_us = d; }
#if DEBUG_MPR121
        mpr121_debug_tick();
#endif

        // ===== AIR更新（高实时性输入，完全独立）=====
        STAGE_ENTER(MAIN_STAGE_AIR);
        // 仅依赖Core 1 TOF snapshot，不依赖MPR121
        { uint64_t t0 = time_us_64();
        air_update();
        uint32_t d = (uint32_t)(time_us_64() - t0); if (d > air_update_max_us) air_update_max_us = d; }

        // ===== 物理按钮更新（非阻塞GPIO读取）=====
        STAGE_ENTER(MAIN_STAGE_BUTTON);
        button_update();

        // ===== HID Report生成和发送（必须持续运行）=====
        STAGE_ENTER(MAIN_STAGE_HID);
        { uint64_t t0 = time_us_64();
        gen_nkro_report();
        report_usb_hid();
#if CHUNI_FAULT_RECOVERY_ENABLE
        usb_hid_watchdog();
#endif
        uint32_t d = (uint32_t)(time_us_64() - t0); if (d > hid_task_max_us) hid_task_max_us = d; }

        // ===== CDC任务（低优先级，只接收/解析/入队）=====
        { uint64_t t0 = time_us_64();
        STAGE_ENTER(MAIN_STAGE_CDC_TASK);
        cdc_task();
        uint32_t d = (uint32_t)(time_us_64() - t0); if (d > cdc_task_max_us) cdc_task_max_us = d; }

        // ===== CDC 命令执行（每轮最多 1 条, 分步推进）=====
        { uint64_t t0 = time_us_64();
        STAGE_ENTER(MAIN_STAGE_CMD_STEP);
        command_step();
        uint32_t d = (uint32_t)(time_us_64() - t0); if (d > cmd_step_max_us) cmd_step_max_us = d; }


        // ===== 低优先级后台任务 =====
        // SAVE 只在空闲窗口 (无按键活动) 执行, 避免游戏进行中随机 USB 短暂停
        bool keys_idle = (slider_get_state() == 0) && (air_get_bitmap() == 0) &&
                         gpio_get(BUTTON_ENTER_PIN) && gpio_get(BUTTON_2_PIN);
        save_set_idle_hint(keys_idle);
        { uint64_t t0 = time_us_64();
        STAGE_ENTER(MAIN_STAGE_SAVE);
        save_loop();   // 执行延迟的 Flash 保存（无请求时零开销）
        uint32_t d = (uint32_t)(time_us_64() - t0); if (d > save_loop_max_us) save_loop_max_us = d; }

        // ===== Core1 ToF 事件输出 =====
        // (printf 已全局禁用: 只发送 HID 报文。事件队列仍由 Core1 写入,
        //  满后丢弃并计数, 不阻塞 Core1)
        // tof_event_task();

        // 性能统计：计算循环耗时
        uint64_t loop_end = time_us_64();
        uint32_t loop_duration = (uint32_t)(loop_end - loop_start);

        main_loop_count++;
        if (loop_duration > main_loop_max_us) {
            main_loop_max_us = loop_duration;
        }
        // 滑动平均
        main_loop_avg_us = (main_loop_avg_us * 9 + loop_duration) / 10;

        // 监控模式：定期输出数据（低优先级）
        // (printf 已全局禁用: 只发送 HID 报文, monitor 输出关闭)
        // if (monitor_mode) {
        //     uint64_t now = time_us_64();
        //     uint32_t elapsed_ms = (now - last_monitor_time) / 1000;
        //
        //     if (elapsed_ms >= monitor_interval_ms) {
        //         output_monitor_data();
        //         last_monitor_time = now;
        //     }
        // }

        // ===== 主循环节拍完成标记 + 看门狗喂狗 =====
        // watchdog_update() 只在完整走完一遍主循环后调用: 任何 stage
        // 永久卡死都会在 2s 内触发复位, 复位后可读出卡死 stage。
        // (旧实现的 main_loop_max_us>5000 LED 常亮锁存已移除 —— 那是
        //  一次性锁存, 第一次 SAVE Flash 擦写 ~400ms 就会永久点亮)
        loop_completed_count++;
        last_main_stage = (uint32_t)MAIN_STAGE_LOOP_END;
        last_stage_enter_us = time_us_32();
#if CHUNI_FAULT_RECOVERY_ENABLE
        watchdog_update();
#endif
    }

    return 0;
}

// HID callbacks + TinyUSB 生命周期回调
extern "C" {
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t* buffer, uint16_t reqlen) {
    // 必须立即返回 —— 不进入 I2C/Flash/printf/等待
    (void)itf; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const* buffer, uint16_t bufsize) {
    (void)itf; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

// ---- TinyUSB 生命周期回调 (由 Core0 的 tud_task() 调用) ----
// 只更新状态变量/时间戳/计数, 禁止 printf/阻塞/I2C/Flash

void tud_mount_cb(void) {
    usb_mounted = true;
    usb_suspended = false;
    usb_health.mount_count++;
    usb_health.last_mount_us = time_us_64();
    // USB 重枚举后强制重发当前 HID 状态: sent 状态无效 + dirty,
    // 第一次 HID ready 即发送完整当前状态, 用户无需重新按键
    sent_valid = false;
    hid_dirty = true;
    hid_not_ready_since_us = 0;
}

void tud_umount_cb(void) {
    usb_mounted = false;
    usb_suspended = false;
    usb_health.unmount_count++;
    usb_health.last_unmount_us = time_us_64();
    hid_not_ready_since_us = 0;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    usb_suspended = true;
    usb_health.suspend_count++;
    usb_health.last_suspend_us = time_us_64();
    hid_not_ready_since_us = 0;
}

void tud_resume_cb(void) {
    usb_suspended = false;
    usb_health.resume_count++;
    usb_health.last_resume_us = time_us_64();
    hid_not_ready_since_us = 0;
    sent_valid = false;
    hid_dirty = true;
}
}
