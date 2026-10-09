/*
 * MPR121 Touch Controller Header
 */

#ifndef MPR121_H
#define MPR121_H

#include <stdint.h>
#include <stdbool.h>

/*
 * ===== MPR121 Default Threshold Configuration =====
 *
 * 根据实际使用场景选择合适的默认阈值：
 *
 * 1. 直接接触模式 (MPR121_TOUCH_BARRIER_MODE = 0)
 *    - 触点直接接触皮肤或导电材料
 *    - 高阈值，稳定性好，抗干扰能力强
 *    - Touch=20, Release=18
 *
 * 2. 物体间隔模式 (MPR121_TOUCH_BARRIER_MODE = 1)
 *    - 触点上有手套、贴纸、塑料片等绝缘材料
 *    - 低阈值，高灵敏度，更容易触发
 *    - Touch=10, Release=8
 *
 * 使用方法：
 *   - 修改 MPR121_TOUCH_BARRIER_MODE 定义值
 *   - 重新编译固件
 *   - 运行时仍可通过 CONFIG 命令调整
 */
#define MPR121_TOUCH_BARRIER_MODE     2    /* 0=直接接触, 1=物体间隔, 2=物体间隔+手套 */

#if MPR121_TOUCH_BARRIER_MODE == 1
/* 物体间隔模式：低阈值，高灵敏度 */
#define MPR121_DEFAULT_TOUCH_THRESHOLD    3
#define MPR121_DEFAULT_RELEASE_THRESHOLD   2
#elif MPR121_TOUCH_BARRIER_MODE == 2
/* 物体间隔模式 + 手套：中等阈值 */
#define MPR121_DEFAULT_TOUCH_THRESHOLD    3
#define MPR121_DEFAULT_RELEASE_THRESHOLD   2
#else
/* 直接接触模式：高阈值，稳定可靠（默认） */
#define MPR121_DEFAULT_TOUCH_THRESHOLD    20
#define MPR121_DEFAULT_RELEASE_THRESHOLD  18
#endif

// Deadline for the complete asynchronous transfer (not a busy-wait budget).
#define MPR121_I2C_UPDATE_TIMEOUT_US  500
#define MPR121_I2C_RUNTIME_TIMEOUT_US 500

// Diagnostics use cached values; there is no second I2C bus owner.
#define DEBUG_MPR121 0

namespace Chuni245Tof {
void mpr121_init(); // schedules cold startup; call update() to advance it
void mpr121_set_thresholds(uint8_t touch_thr, uint8_t release_thr);
void mpr121_reset_baseline();
void mpr121_update();
void mpr121_task_step(); // command step, invoked by update(); never waits for I/O
bool mpr121_op_busy();
uint32_t mpr121_get_touch_state(uint8_t device);
bool mpr121_is_touched(uint8_t device, uint8_t channel);
void mpr121_debug_print(); // cached touch/error counts only
uint32_t mpr121_get_error_count(uint8_t device);
#if DEBUG_MPR121
void mpr121_debug_init();
void mpr121_debug_tick();
#endif
} // namespace Chuni245Tof
using namespace Chuni245Tof;
#endif /* MPR121_H */
