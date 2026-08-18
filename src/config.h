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
    uint8_t tof_offset;  // 起始高度, 默认 60mm
    uint8_t tof_pitch;   // 每段高度 (Air1-5), 默认 30mm
    uint8_t air6_range;  // Air6 检测范围, 默认 150mm (>= pitch)
    uint16_t air_min_hold_ms;  // AIR 最小按下持续时间, 默认 50ms (修正为 uint16_t)

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
void config_save();
void config_reset();

} // namespace Chuni245Tof

// Import into global namespace for compatibility
using namespace Chuni245Tof;

#endif /* CONFIG_H */