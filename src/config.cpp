/*
 * Configuration Implementation
 * Default: Keyboard only mode
 */

#include "config.h"
#include "save.h"
#include <string.h>

namespace Chuni245Tof {

// Default configuration - Keyboard mode only
static const config_t default_config = {
    .hid_mode = HID_MODE_KEYBOARD_ONLY,  // Default: Keyboard only
    .touch_threshold = 20,   // 高阈值，稳定
    .release_threshold = 18,  // 高阈值，小迟滞区
    .tof_offset = 120,  // Default: 120mm 起始高度
    .tof_pitch = 40,  // Default: 40mm step height for air detection
    .air6_range = 150,  // Default: 150mm range for Air6 (更大的范围)
    .air_min_hold_ms = 50,  // Default: 50ms 最小按下持续时间 (uint16_t)
    .air_threshold = {1500, 1500, 1500, 1500, 1500},
};

config_t* cfg = nullptr;
static config_t config_data;

void config_init() {
    if (!save_load(&config_data, sizeof(config_data))) {
        memcpy(&config_data, &default_config, sizeof(config_data));
    }

    // Validate
    if (config_data.hid_mode > HID_MODE_GAMEPAD_KEYBOARD) {
        config_data.hid_mode = default_config.hid_mode;
    }
    // MPR121 阈值范围: touch 5-30, release 1-25
    if (config_data.touch_threshold < 5 || config_data.touch_threshold > 30) {
        config_data.touch_threshold = default_config.touch_threshold;
    }
    if (config_data.release_threshold < 1 || config_data.release_threshold > 25) {
        config_data.release_threshold = default_config.release_threshold;
    }
    // TOF 参数验证
    if (config_data.tof_offset < 40 || config_data.tof_offset > 200) {
        config_data.tof_offset = default_config.tof_offset;
    }
    if (config_data.tof_pitch < 4 || config_data.tof_pitch > 100) {
        config_data.tof_pitch = default_config.tof_pitch;
    }
    // Air6 范围验证：必须 >= pitch
    if (config_data.air6_range < config_data.tof_pitch || config_data.air6_range > 200) {
        config_data.air6_range = default_config.air6_range;
    }
    // AIR 最小按下时间验证：10-500ms
    if (config_data.air_min_hold_ms < 10 || config_data.air_min_hold_ms > 500) {
        config_data.air_min_hold_ms = default_config.air_min_hold_ms;
    }

    cfg = &config_data;
}

void config_save() {
    if (cfg) {
        save_write(cfg, sizeof(config_t));
    }
}

void config_reset() {
    if (cfg) {
        memcpy(cfg, &default_config, sizeof(config_t));
        config_save();
    }
}

} // namespace Chuni245Tof