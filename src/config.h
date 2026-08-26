/*
 * Configuration Header
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

namespace Chuni245Tof {

// HID modes
#define HID_MODE_GAMEPAD_ONLY       0
#define HID_MODE_KEYBOARD_ONLY      1
#define HID_MODE_GAMEPAD_KEYBOARD   2

// NKRO keymap
#define NKRO_KEYMAP "1234567890qwertyuiopasdfghjklzxcvbnm"
#define NKRO_KEYMAP_LEN 32

// Configuration structure
typedef struct {
    uint8_t hid_mode;
    uint8_t touch_threshold;
    uint8_t release_threshold;

    // TOF Air 检测参数 (单位: mm)
    uint8_t tof_offset;  // 起始高度, 默认 120mm
    uint8_t tof_pitch;   // 每段高度 (AIR1~AIR11), 默认 30mm
    uint8_t air12_range; // AIR12 特殊检测范围, 默认 150mm (>= pitch)
    uint16_t air_min_hold_ms;  // AIR 最小按下持续时间, 默认 100ms

    // AIR Overlay Mode - 扩展 AIR 检测到 12 层
    // AIR7~AIR12 是 AIR6 上方的六个新距离层
    // AIR7 -> HID key1, AIR8 -> HID key2, ... AIR12 -> HID key6
    uint8_t air_overlay_enabled;  // 默认 1 (开启)

    // 保留原有阈值配置 (兼容性)
    uint16_t air_threshold[5];
} config_t;

// Report IDs
#define REPORT_ID_JOYSTICK  1
#define REPORT_ID_NKRO      2

// Global config pointer
extern config_t* cfg;

// Configuration functions
void config_init();
bool config_save();  // 返回 true 表示保存成功, false 表示失败
void config_reset();

// Barrier mode functions
void config_set_barrier_mode(uint8_t mode);  // 设置 barrier mode (影响默认阈值)
uint8_t config_get_barrier_mode();           // 获取当前 barrier mode

} // namespace Chuni245Tof

// Import into global namespace for compatibility
using namespace Chuni245Tof;

#endif /* CONFIG_H */