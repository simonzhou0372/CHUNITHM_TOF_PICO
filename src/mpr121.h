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
#define MPR121_TOUCH_BARRIER_MODE     0    /* 0=直接接触, 1=物体间隔 */

#if MPR121_TOUCH_BARRIER_MODE == 1
/* 物体间隔模式：低阈值，高灵敏度 */
#define MPR121_DEFAULT_TOUCH_THRESHOLD    10
#define MPR121_DEFAULT_RELEASE_THRESHOLD   8
#else
/* 直接接触模式：高阈值，稳定可靠（默认） */
#define MPR121_DEFAULT_TOUCH_THRESHOLD    20
#define MPR121_DEFAULT_RELEASE_THRESHOLD  18
#endif

/*
 * ===== MPR121 Debug System =====
 *
 * Set DEBUG_MPR121 to 0 to completely disable all debug code (zero overhead).
 * When enabled, the debug system adds:
 *   - One-shot CONFIG dump after init (thresholds, CONFIG1/2, ECR, filter params)
 *   - TOUCH/RELEASE event logging (only on state transitions)
 *   - Periodic statistics table (filtered/baseline/delta min/max per electrode)
 *
 * Register layout (packed, per MPR121 datasheet):
 *   Filtered: 0x04 + ch*2   (ELE0-ELE11 readable, no overlap)
 *   Baseline: 0x1E + ch*2   (ELE0-ELE5 readable; ELE6+ addresses overlap
 *                             with filter config registers 0x2A-0x35)
 *
 * Delta formula:
 *   delta = (int16_t)(filtered & 0x0FFF) - (int16_t)(baseline & 0x03FF)
 *   Negative delta means filtered < baseline (touch direction).
 *
 * I2C overhead is minimized by:
 *   - Cycling reads: one (dev,ch) pair per N loop iterations (not all at once)
 *   - Event reads: only triggered on state transitions
 *   - Stats print: rate-limited to MPR121_DEBUG_STAT_INTERVAL_MS
 */
#define DEBUG_MPR121                    0   /* 0 = disable, 1 = enable           */
#define MPR121_DEBUG_FIRST_ELECTRODE    0   /* first electrode to monitor (0-5)  */
#define MPR121_DEBUG_LAST_ELECTRODE     5   /* last  electrode to monitor (0-5)  */
                                            /* NOTE: E6+ baseline is unreadable  */
                                            /* (overlaps with filter config)     */
#define MPR121_DEBUG_STAT_INTERVAL_MS 1000  /* statistics table print interval   */
#define MPR121_DEBUG_SAMPLE_DIVIDER      5  /* read debug data every Nth loop    */
                                            /* iteration (1 = every loop)        */

namespace Chuni245Tof {

void mpr121_init();
void mpr121_set_thresholds(uint8_t touch_thr, uint8_t release_thr);
void mpr121_update();
uint32_t mpr121_get_touch_state(uint8_t device);
bool mpr121_is_touched(uint8_t device, uint8_t channel);
void mpr121_debug_print();  // 打印 Baseline/FilteredData/Delta 调试信息
void mpr121_reset_baseline();  // 重置所有通道的 Baseline
uint32_t mpr121_get_error_count(uint8_t device);  // 获取 I2C 错误计数

#if DEBUG_MPR121
/*
 * Call once after mpr121_init() completes.
 * Prints the [MPR121 CONFIG] table: CONFIG1, CONFIG2, ECR, DEBOUNCE,
 * filter parameters, and per-electrode touch/release thresholds.
 */
void mpr121_debug_init();

/*
 * Call every main loop iteration (after slider_update / mpr121_update).
 * Handles event detection, cycling I2C reads, and periodic stats output.
 * Returns immediately if not enough loop iterations have passed (divider).
 */
void mpr121_debug_tick();
#endif

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* MPR121_H */