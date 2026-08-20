/*
 * Configuration Implementation
 *
 * 修复说明:
 * 1. 增加 config_init() 启动时的详细日志
 * 2. 增加 config_save() 返回 bool 表示成功/失败
 * 3. 增加 Flash 加载后的参数验证日志
 * 4. 保持与现有代码的兼容性
 */

#include "config.h"
#include "save.h"
#include "mpr121.h"  // 引入 MPR121 默认阈值定义
#include <string.h>
#include <stdio.h>

namespace Chuni245Tof {

//==============================================================================
// 默认配置
//==============================================================================

// Default configuration - Keyboard mode only
static const config_t default_config = {
    .hid_mode = HID_MODE_KEYBOARD_ONLY,  // Default: Keyboard only
    .touch_threshold = MPR121_DEFAULT_TOUCH_THRESHOLD,      // 由 mpr121.h 定义
    .release_threshold = MPR121_DEFAULT_RELEASE_THRESHOLD,  // 由 mpr121.h 定义
    .tof_offset = 120,                    // Default: 120mm 起始高度
    .tof_pitch = 30,                      // Default: 30mm step height for air detection
    .air6_range = 150,                    // Default: 150mm range for Air6 (更大的范围)
    .air_min_hold_ms = 100,               // Default: 100ms 最小按下持续时间
    .air_threshold = {1500, 1500, 1500, 1500, 1500},
};

//==============================================================================
// 全局配置数据
//==============================================================================

config_t* cfg = nullptr;
static config_t config_data;

//==============================================================================
// 配置验证
//==============================================================================

/**
 * 验证配置参数范围
 * @param cfg 配置指针
 * @return 是否需要修正
 */
static bool validate_config(config_t* cfg) {
    bool modified = false;

    // HID 模式验证
    if (cfg->hid_mode > HID_MODE_GAMEPAD_KEYBOARD) {
        printf("CONFIG VALIDATE: hid_mode %d -> %d (invalid)\r\n",
               cfg->hid_mode, default_config.hid_mode);
        cfg->hid_mode = default_config.hid_mode;
        modified = true;
    }

    // MPR121 阈值范围: 已解除限制，允许调试
    // touch_threshold 和 release_threshold 的合理范围需要用户自行把握
    // 注意：touch_threshold 必须大于 release_threshold（迟滞区）
    if (cfg->touch_threshold < 1 || cfg->touch_threshold > 255) {
        printf("CONFIG VALIDATE: touch_threshold %d -> %d (out of range 1-255)\r\n",
               cfg->touch_threshold, default_config.touch_threshold);
        cfg->touch_threshold = default_config.touch_threshold;
        modified = true;
    }
    if (cfg->release_threshold < 1 || cfg->release_threshold > 255) {
        printf("CONFIG VALIDATE: release_threshold %d -> %d (out of range 1-255)\r\n",
               cfg->release_threshold, default_config.release_threshold);
        cfg->release_threshold = default_config.release_threshold;
        modified = true;
    }

    // TOF 参数验证
    if (cfg->tof_offset < 40 || cfg->tof_offset > 200) {
        printf("CONFIG VALIDATE: tof_offset %d -> %d (out of range 40-200)\r\n",
               cfg->tof_offset, default_config.tof_offset);
        cfg->tof_offset = default_config.tof_offset;
        modified = true;
    }
    if (cfg->tof_pitch < 4 || cfg->tof_pitch > 100) {
        printf("CONFIG VALIDATE: tof_pitch %d -> %d (out of range 4-100)\r\n",
               cfg->tof_pitch, default_config.tof_pitch);
        cfg->tof_pitch = default_config.tof_pitch;
        modified = true;
    }

    // Air6 范围验证：必须 >= pitch
    if (cfg->air6_range < cfg->tof_pitch || cfg->air6_range > 200) {
        printf("CONFIG VALIDATE: air6_range %d -> %d (must be >= pitch %d and <= 200)\r\n",
               cfg->air6_range, default_config.air6_range, cfg->tof_pitch);
        cfg->air6_range = default_config.air6_range;
        modified = true;
    }

    // AIR 最小按下时间验证：10-500ms
    if (cfg->air_min_hold_ms < 10 || cfg->air_min_hold_ms > 500) {
        printf("CONFIG VALIDATE: air_min_hold_ms %d -> %d (out of range 10-500)\r\n",
               cfg->air_min_hold_ms, default_config.air_min_hold_ms);
        cfg->air_min_hold_ms = default_config.air_min_hold_ms;
        modified = true;
    }

    return modified;
}

//==============================================================================
// API 实现
//==============================================================================

/**
 * 初始化配置
 *
 * 流程:
 * 1. 尝试从 Flash 加载
 * 2. 如果加载失败，使用默认配置
 * 3. 验证参数范围
 * 4. 设置全局指针
 */
void config_init() {
    printf("\r\n========================================\r\n");
    printf("CONFIG INIT: Starting configuration load\r\n");
    printf("========================================\r\n");

    // 尝试从 Flash 加载
    bool loaded = save_load(&config_data, sizeof(config_data));

    if (loaded) {
        printf("CONFIG INIT: Loaded from Flash, validating...\r\n");

        // 验证参数范围
        bool modified = validate_config(&config_data);

        if (modified) {
            printf("CONFIG INIT: Parameters corrected after validation\r\n");
        }

        printf("CONFIG LOAD FLASH: touch=%d release=%d offset=%d pitch=%d air6=%d hold=%d\r\n",
               config_data.touch_threshold, config_data.release_threshold,
               config_data.tof_offset, config_data.tof_pitch,
               config_data.air6_range, config_data.air_min_hold_ms);
    } else {
        printf("CONFIG INIT: Flash load failed, using defaults\r\n");
        memcpy(&config_data, &default_config, sizeof(config_data));

        printf("CONFIG LOAD DEFAULT: touch=%d release=%d offset=%d pitch=%d air6=%d hold=%d\r\n",
               config_data.touch_threshold, config_data.release_threshold,
               config_data.tof_offset, config_data.tof_pitch,
               config_data.air6_range, config_data.air_min_hold_ms);
    }

    // 设置全局指针
    cfg = &config_data;

    printf("========================================\r\n\r\n");
}

/**
 * 保存当前配置到 Flash
 *
 * @return true 保存成功, false 失败
 */
bool config_save() {
    if (!cfg) {
        printf("CONFIG SAVE: ERROR - config not initialized\r\n");
        return false;
    }

    printf("CONFIG SAVE: Saving current config to Flash...\r\n");
    printf("CONFIG SAVE: touch=%d release=%d offset=%d pitch=%d air6=%d hold=%d\r\n",
           cfg->touch_threshold, cfg->release_threshold,
           cfg->tof_offset, cfg->tof_pitch,
           cfg->air6_range, cfg->air_min_hold_ms);

    bool success = save_write(cfg, sizeof(config_t));

    if (success) {
        printf("CONFIG SAVE: SUCCESS\r\n");
    } else {
        printf("CONFIG SAVE: FAILED\r\n");
    }

    return success;
}

/**
 * 重置配置为默认值并保存
 */
void config_reset() {
    if (!cfg) {
        printf("CONFIG RESET: ERROR - config not initialized\r\n");
        return;
    }

    printf("CONFIG RESET: Resetting to defaults...\r\n");
    memcpy(cfg, &default_config, sizeof(config_t));

    if (config_save()) {
        printf("CONFIG RESET: SUCCESS\r\n");
    } else {
        printf("CONFIG RESET: FAILED to save\r\n");
    }
}

} // namespace Chuni245Tof