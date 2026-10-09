/*
 * MPR121 Touch Controller Implementation
 * Uses I2C0 (SDA=16, SCL=17)
 *
 * I2C0 总线域独占声明:
 *   本文件是全工程唯一接触 I2C0 外设与 GPIO16/17 的翻译单元。
 *   I2C0 初始化 (mpr121_init) 仅发生在 Core0 冷启动 (slider_init 调用),
 *   Core1 的 VL53L0X 恢复路径 (recover_sensor / bus_recover / reinit_all)
 *   不经过本文件, 也不会触碰 I2C0 —— MPR121 在任何 ToF 恢复期间连续运行。
 *
 * 零运行时恢复原则:
 *   MPR121 几乎不会掉线, 固件不存在任何运行时 MPR121 恢复/下线/重试逻辑:
 *   mpr_ready 仅在冷启动赋值, 运行期永不撤销; mpr121_update 遇 I2C 错误
 *   仅计数并保留旧触摸状态, 下一次成功轮询自动续读。
 *   唯一的手动重初始化入口是串口 RESET 命令 (mpr121_reset_baseline)。
 *
 * ==============================================================================
 * MPR121 Touch Detection Architecture
 * ==============================================================================
 *
 * 【重要】Touch 判定由 MPR121 芯片内部硬件完成，不是 RP2040 根据 delta 软件判断。
 *
 * 完整链路：
 *   MPR121 Electrode E0~E11
 *       ↓ (电容变化)
 *   MPR121 Internal Hardware:
 *       - Filtered Data (12-bit, 0x04-0x1B)
 *       - Baseline (10-bit, 0x1E-0x35, E6+ 与滤波配置寄存器重叠，不可读)
 *       - Delta = Filtered - Baseline (有符号 16-bit)
 *       - Touch Threshold (寄存器 0x41-0x57, E0~E11)
 *       - Release Threshold (寄存器 0x42-0x58, E0~E11)
 *       ↓ (硬件比较)
 *   MPR121 Touch Status (寄存器 0x00-0x01, 12-bit bitmap)
 *       ↓ (I2C 读取)
 *   RP2040 touch_state[] (mpr121_update)
 *       ↓
 *   mpr121_is_touched() → slider/HID
 *
 * 【Touch/Release 阈值定义】
 *   - Touch Threshold: 当 delta < -touch_threshold 时触发 Touch
 *   - Release Threshold: 当 delta > -release_threshold 时触发 Release
 *   - 迟滞区：[-touch_threshold, -release_threshold]，避免频繁抖动
 *   - 要求：touch_threshold > release_threshold（至少差 2）
 *
 * 【关于 debug delta】
 *   delta = filtered - baseline，可以是正数或负数：
 *   - 负数：filtered < baseline，表示电容增加（手指靠近）
 *   - 正数：filtered > baseline，表示电容减少（手指离开）
 *   - 绝对值：变化幅度，不一定与 Touch 状态直接对应
 *
 * 【为什么 delta 绝对值可以很大但仍未触发 Touch？】
 *   1. Baseline 动态跟踪：MPR121 会根据环境变化调整 baseline
 *   2. Touch Threshold 是相对阈值：delta 必须低于 -touch_threshold 才触发
 *   3. 静态 delta 大不代表动态变化大：静态偏差会被 baseline 吸收
 *
 * 【不要根据 debug delta 软件判断 Touch】
 *   错误做法：if (delta < -touch_threshold) touched = true;
 *   正确做法：使用 MPR121_TOUCHSTATUS 寄存器（已由硬件完成判定）
 *
 * 【Barrier Mode 运行时选择】
 *   - Mode 0: 直接接触模式 (Touch=20, Release=18, CONFIG1=0x35, CONFIG2=0x02)
 *   - Mode 1: 物体间隔模式 (Touch=3, Release=2, CONFIG1=0x35, CONFIG2=0x22)
 *   - Mode 2: 物体间隔+手套模式 (Touch=3, Release=2, CONFIG1=0x20, CONFIG2=0x22)
 *   - 由 config_get_barrier_mode() 在运行时确定
 *
 * ==============================================================================
 */

#include "mpr121.h"
#include "board_defs.h"
#include "config.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "i2c_async.h"
#include <stdio.h>
#include <string.h>
// 注意: log_output.h 必须在所有 SDK/系统头之后 —— 它把 printf 宏定义为空操作,
// 而 SDK 头中的 __printflike 会展开为 format(printf,...), 若先于头文件
// 定义会导致解析失败
#include "log_output.h"   // printf 总开关 (默认禁用, 只发送 HID 报文)

namespace Chuni245Tof {

// MPR121 Register addresses
#define MPR121_TOUCHSTATUS_L  0x00
#define MPR121_TOUCHSTATUS_H  0x01
#define MPR121_DEBOUNCE       0x5B
#define MPR121_CONFIG1        0x5C
#define MPR121_CONFIG2        0x5D
#define MPR121_ECR            0x5E
#define MPR121_SOFTRESET      0x80
#define MPR121_TOUCHTH_L      0x41
#define MPR121_RELEASETH_L    0x42  // 修正: ELE0 Release Threshold

// Auto-Configuration registers
#define MPR121_USL            0x7B  // Upper Signal Limit
#define MPR121_LSL            0x7C  // Lower Signal Limit
#define MPR121_TL             0x7D  // Target Level
#define MPR121_ACCR0          0x7E  // Auto-Configuration Control Register 0

// Filter configuration registers
#define MPR121_MHDR           0x2B
#define MPR121_NHDR           0x2C
#define MPR121_NCLR           0x2D
#define MPR121_FDLR           0x2E
#define MPR121_MHDF           0x2F
#define MPR121_NHDF           0x30
#define MPR121_NCLF           0x31
#define MPR121_FDLF           0x32
#define MPR121_NHDT           0x33
#define MPR121_NCLT           0x34
#define MPR121_FDLT           0x35

// Data registers for each electrode (PACKED layout, per MPR121 datasheet):
//   Filtered data: 0x04-0x1B  (12 electrodes × 2 bytes, packed)
//   Baseline data: 0x1E-0x35  (12 electrodes × 2 bytes, packed)
//
// IMPORTANT: Baseline addresses for ELE6+ (0x2A+) overlap with filter config
// registers (MHDR=0x2B, NHDR=0x2C, ...), so only ELE0-ELE5 baseline is
// reliably readable. ELE6-ELE11 baseline CANNOT be read back via I2C.
#define MPR121_ELE0_FILTERED  0x04
#define MPR121_ELE0_BASELINE  0x1E
#define MPR121_MAX_READABLE_BASELINE_CH  5  // E6+ baseline overlaps filter cfg

static uint8_t mpr_addr[3] = {MPR121_ADDR_1, MPR121_ADDR_2, MPR121_ADDR_3};
static uint32_t touch_state[3] = {0, 0, 0};
static bool mpr_ready[3] = {false, false, false};
static uint32_t i2c_error_count[3] = {0, 0, 0};  // I2C 错误计数

// Core0-only transaction engine. A pending transaction yields to USB/HID.
static AsyncI2c mpr_bus(I2C0_PORT);
enum IoOwner { IO_NONE, IO_COMMAND, IO_TOUCH };
static IoOwner io_owner = IO_NONE;
enum IoResult { IO_ERROR = -1, IO_PENDING = 0, IO_OK = 1 };
static IoResult command_io(uint8_t addr, uint8_t reg, uint8_t value, uint8_t* read) {
    if (io_owner == IO_NONE) {
        if (mpr_bus.busy()) return IO_PENDING;
        uint8_t bytes[2] = {reg, value};
        if (!mpr_bus.start(addr, bytes, read ? 1 : 2, read ? 1 : 0,
                           MPR121_I2C_RUNTIME_TIMEOUT_US)) return IO_ERROR;
        io_owner = IO_COMMAND;
        return IO_PENDING;
    }
    if (io_owner != IO_COMMAND || mpr_bus.busy()) return IO_PENDING;
    io_owner = IO_NONE;
    if (mpr_bus.result() != 2) return IO_ERROR;
    if (read) *read = mpr_bus.data()[0];
    return IO_OK;
}
static IoResult mpr_rt_write_byte(uint8_t addr, uint8_t reg, uint8_t val) {
    return command_io(addr, reg, val, nullptr);
}
static IoResult mpr_rt_read_byte(uint8_t addr, uint8_t reg, uint8_t* val) {
    return command_io(addr, reg, 0, val);
}
static bool cold_boot = true;
static uint64_t boot_deadline = 0;

// 计算单芯片初始化寄存器值 (barrier mode + 用户 cfg)
// mpr_init_single (上电路径) 与异步 RESET 状态机共用, 保证两条路径
// 写入的寄存器序列永远一致
static void mpr_compute_config(uint8_t *touch_thr, uint8_t *release_thr,
                               uint8_t *config1, uint8_t *config2) {
    uint8_t barrier_mode = config_get_barrier_mode();

    switch (barrier_mode) {
        case 0: // Mode 0: 直接接触
            *touch_thr = 20; *release_thr = 18;
            *config1 = 0x35; *config2 = 0x02;
            break;
        case 1: // Mode 1: 物体间隔
            *touch_thr = 3; *release_thr = 2;
            *config1 = 0x35; *config2 = 0x22;
            break;
        case 2: // Mode 2: 物体间隔+手套
        default:
            *touch_thr = 3; *release_thr = 2;
            *config1 = 0x20; *config2 = 0x22;
            break;
    }

    // 如果 cfg 已初始化，使用 Flash 中的用户配置
    if (cfg) {
        *touch_thr = cfg->touch_threshold;
        *release_thr = cfg->release_threshold;
    }
}

void mpr121_init() {
    gpio_set_function(I2C0_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C0_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C0_SDA);
    gpio_pull_up(I2C0_SCL);
    mpr_bus.init(400000);
    boot_deadline = time_us_64() + 10000;
    cold_boot = true;
}

// CONFIG / RESET and cold startup share one asynchronous I2C owner.
// Each step submits/checks a transfer. The 500us deadline spans loop iterations;
// it is not a busy-wait budget. Reset/settling waits also yield to USB.

typedef enum {
    THR_IDLE = 0,        // 无操作
    THR_APPLY,           // 阈值下发进行中 (72 步)
    THR_RESET_WRITE,     // [RESET] 软复位写
    THR_RESET_WAIT1,     // [RESET] 等待复位完成 (≥3ms, deadline 判定)
    THR_RESET_CHECK,     // [RESET] 读 0x5C 校验响应
    THR_RESET_INIT,      // [RESET] 重初始化写序列 (39 步)
    THR_RESET_WAIT2,     // [RESET] 等待 ECR 生效 (≥100ms, deadline 判定)
} thr_state_t;

static thr_state_t thr_state = THR_IDLE;
static bool thresholds_pending = false;
static uint8_t  thr_dev = 0;          // 当前设备
static uint16_t thr_step = 0;         // 当前步 (THR_APPLY: 0..71 / THR_RESET_INIT: 0..38)
static uint8_t  thr_touch = 0, thr_release = 0;
static uint8_t  thr_config1 = 0, thr_config2 = 0;
static uint64_t thr_deadline = 0;

// RESET 重初始化写序列 (与 mpr_init_single 完全一致, 在 THR_RESET_CHECK 生成)
typedef struct { uint8_t reg; uint8_t val; } mpr_reg_pair_t;
#define MPR_INIT_SEQ_LEN 39
static mpr_reg_pair_t thr_init_seq[MPR_INIT_SEQ_LEN];

static void build_init_sequence() {
    uint8_t seq = 0;
    for (int i = 0; i < 12; i++) {
        thr_init_seq[seq++] = { (uint8_t)(MPR121_TOUCHTH_L + i * 2), thr_touch };
        thr_init_seq[seq++] = { (uint8_t)(MPR121_RELEASETH_L + i * 2), thr_release };
    }
    thr_init_seq[seq++] = { MPR121_MHDR,     0x01 };
    thr_init_seq[seq++] = { MPR121_NHDR,     0x01 };
    thr_init_seq[seq++] = { MPR121_NCLR,     0x0E };
    thr_init_seq[seq++] = { MPR121_FDLR,     0x00 };
    thr_init_seq[seq++] = { MPR121_MHDF,     0x01 };
    thr_init_seq[seq++] = { MPR121_NHDF,     0x05 };
    thr_init_seq[seq++] = { MPR121_NCLF,     0x01 };
    thr_init_seq[seq++] = { MPR121_FDLF,     0x00 };
    thr_init_seq[seq++] = { MPR121_NHDT,     0x00 };
    thr_init_seq[seq++] = { MPR121_NCLT,     0x00 };
    thr_init_seq[seq++] = { MPR121_FDLT,     0x00 };
    thr_init_seq[seq++] = { MPR121_DEBOUNCE, 0x00 };
    thr_init_seq[seq++] = { MPR121_CONFIG1,  thr_config1 };
    thr_init_seq[seq++] = { MPR121_CONFIG2,  thr_config2 };
    thr_init_seq[seq++] = { MPR121_ECR,      0x8C };
}

bool mpr121_op_busy() {
    return cold_boot || thresholds_pending || thr_state != THR_IDLE;
}

void mpr121_set_thresholds(uint8_t touch_thr, uint8_t release_thr) {
    // 确保 Touch > Release，至少差 2，避免迟滞区为 0
    // MPR121 硬件要求：
    //   Touch:   delta < -touch_threshold 触发
    //   Release: delta > -release_threshold 释放
    // 因此 touch_threshold 必须大于 release_threshold，迟滞区大小 = touch - release
    if (touch_thr <= release_thr) {
        uint8_t old_touch = touch_thr;
        touch_thr = release_thr + 2;
        printf("[MPR121] WARNING: touch_threshold (%d) <= release_threshold (%d), auto-corrected to %d\r\n",
               old_touch, release_thr, touch_thr);

        // 同步修正 cfg，保证 Flash 中保存的值与实际写入的一致
        if (cfg) {
            cfg->touch_threshold = touch_thr;
            printf("[MPR121] cfg->touch_threshold updated to %d\r\n", touch_thr);
        }
    }

    // A new CONFIG during an in-flight write needs another complete pass;
    // otherwise channels already written would retain the previous value.
    thr_touch = touch_thr;
    thr_release = release_thr;
    thresholds_pending = true;
    printf("[MPR121] Threshold apply queued (async): Touch=%d, Release=%d (hysteresis=%d)\n",
           touch_thr, release_thr, touch_thr - release_thr);
}

//------------------------------------------------------------------------------
// 状态机推进: 每次调用最多 1 个 I2C 事务 (≤500us)。
// 由 mpr121_update() 在主循环中每轮调用。
//------------------------------------------------------------------------------
static void mpr_task_step_impl(void) {
    if (thr_state == THR_IDLE && thresholds_pending) {
        thresholds_pending = false;
        thr_state = THR_APPLY;
        thr_step = 0;
    }
    switch (thr_state) {
        case THR_IDLE:
            return;

        case THR_APPLY: {
            // 步进映射: step 0..71 → (dev = step/24, rem = step%24,
            //   rem 偶数 = TOUCHTH, rem 奇数 = RELEASETH, ch = rem/2)
            while (thr_step < 72 && !mpr_ready[thr_step / 24]) {
                thr_step += 24;           // 跳过未就绪设备的 24 步
            }
            if (thr_step >= 72) {
                thr_state = THR_IDLE;
                return;
            }
            uint8_t dev = thr_step / 24;
            uint8_t rem = thr_step % 24;
            uint8_t reg = (rem & 1) ? (uint8_t)(MPR121_RELEASETH_L + (rem >> 1) * 2)
                                    : (uint8_t)(MPR121_TOUCHTH_L + (rem >> 1) * 2);
            uint8_t val = (rem & 1) ? thr_release : thr_touch;
            IoResult result = mpr_rt_write_byte(mpr_addr[dev], reg, val);
            if (result == IO_PENDING) return;
            if (result == IO_ERROR) {
                i2c_error_count[dev]++;
                // 瞬时失败不回滚不重试: 后续步骤继续推进, 失败寄存器
                // 保持旧值 (与轮询路径 "失败保留旧状态" 一致)
            }
            thr_step++;
            return;
        }

        case THR_RESET_WRITE: {
            // 与旧实现一致: RESET 只重置已就绪设备
            while (thr_dev < 3 && !mpr_ready[thr_dev]) thr_dev++;
            if (thr_dev >= 3) {
                thr_state = THR_IDLE;
                printf("[MPR121] Baseline reset complete\n");
                return;
            }
            IoResult result = mpr_rt_write_byte(mpr_addr[thr_dev], MPR121_SOFTRESET, 0x63);
            if (result == IO_PENDING) return;
            if (result == IO_ERROR) {
                i2c_error_count[thr_dev]++;
                // 软复位写失败: 芯片无响应, 标记未就绪并跳到下一设备
                mpr_ready[thr_dev] = false;
                thr_dev++;
                return;
            }
            thr_deadline = time_us_64() + 3000;  // 数据手册 2ms + 余量
            thr_state = THR_RESET_WAIT1;
            return;

        }
        case THR_RESET_WAIT1:
            // deadline 判定代替 sleep_ms: 等待期间主循环正常运转
            if (time_us_64() < thr_deadline) return;
            thr_state = THR_RESET_CHECK;
            return;

        case THR_RESET_CHECK: {
            uint8_t check = 0;
            IoResult result = mpr_rt_read_byte(mpr_addr[thr_dev], 0x5C, &check);
            if (result == IO_PENDING) return;
            if (result == IO_ERROR ||
                check == 0xFF || check == 0x00) {
                // 与 mpr_init_single 的冷启动判定完全一致
                mpr_ready[thr_dev] = false;
                thr_dev++;
                thr_state = THR_RESET_WRITE;
                return;
            }
            // 生成重初始化写序列 (与 mpr_init_single 寄存器序列一致;
            // 阈值用最近一次 CONFIG/DEFAULT 的值)
            build_init_sequence();
            thr_step = 0;
            thr_state = THR_RESET_INIT;
            return;
        }

        case THR_RESET_INIT: {
            IoResult result = mpr_rt_write_byte(mpr_addr[thr_dev], thr_init_seq[thr_step].reg,
                                                 thr_init_seq[thr_step].val);
            if (result == IO_PENDING) return;
            if (result == IO_ERROR) {
                i2c_error_count[thr_dev]++;
            }
            thr_step++;
            if (thr_step >= MPR_INIT_SEQ_LEN) {
                thr_deadline = time_us_64() + 100000;  // 旧 mpr_init_single 的 sleep_ms(100)
                thr_state = THR_RESET_WAIT2;
            }
            return;

        }
        case THR_RESET_WAIT2:
            if (time_us_64() < thr_deadline) return;
            touch_state[thr_dev] = 0;
            thr_dev++;
            thr_state = THR_RESET_WRITE;   // 下一设备 (内部会跳过未就绪)
            return;
    }
}

void mpr121_task_step() {
    mpr_task_step_impl();
}

// Round-robin complete register transfers; only successful reads publish touch
// state. A failed transfer retains the previous state and yields to USB.
// Startup deadlines replace sleep_ms(2/20/100/500). No USB blackout.
static void boot_step() {
    enum Phase { Reset, WaitReset, Check, Program, WaitRun, Next, Settle };
    static Phase phase = Reset;
    static uint8_t dev = 0, attempt = 0, reg_index = 0;
    const uint64_t now = time_us_64();
    if (now < boot_deadline || mpr_bus.busy()) return;
    IoResult result;
    bool failed = false;
    switch (phase) {
    case Reset:
        result = mpr_rt_write_byte(mpr_addr[dev], MPR121_SOFTRESET, 0x63);
        if (result == IO_PENDING) return;
        if (result == IO_ERROR) { failed = true; break; }
        boot_deadline = now + 2000; phase = WaitReset; return;
    case WaitReset: phase = Check; return;
    case Check: {
        uint8_t check = 0;
        result = mpr_rt_read_byte(mpr_addr[dev], 0x5C, &check);
        if (result == IO_PENDING) return;
        if (result == IO_ERROR || check == 0 || check == 0xff) { failed = true; break; }
        mpr_compute_config(&thr_touch, &thr_release, &thr_config1, &thr_config2);
        build_init_sequence();
        reg_index = 0; phase = Program; return;
    }
    case Program:
        result = mpr_rt_write_byte(mpr_addr[dev], thr_init_seq[reg_index].reg, thr_init_seq[reg_index].val);
        if (result == IO_PENDING) return;
        if (result == IO_ERROR) { failed = true; break; }
        if (++reg_index == MPR_INIT_SEQ_LEN) { boot_deadline = now + 100000; phase = WaitRun; }
        return;
    case WaitRun: mpr_ready[dev] = true; phase = Next; return;
    case Next:
        attempt = 0;
        if (++dev == 3) { boot_deadline = now + 500000; phase = Settle; }
        else phase = Reset;
        return;
    case Settle: cold_boot = false; return;
    }
    if (failed) {
        ++i2c_error_count[dev];
        if (++attempt < 3) { phase = Reset; boot_deadline = now + 20000; }
        else { mpr_ready[dev] = false; phase = Next; }
    }
}

void mpr121_update() {
    static uint8_t rr_index = 0, reading_device = 0;
    mpr_bus.step();
    if (cold_boot) { boot_step(); return; }
    if (io_owner == IO_TOUCH) {
        if (mpr_bus.busy()) return;
        if (mpr_bus.result() == 3) {
            const uint8_t* status = mpr_bus.data();
            touch_state[reading_device] = status[0] | ((uint32_t)status[1] << 8);
        } else ++i2c_error_count[reading_device];
        io_owner = IO_NONE;
        return;
    }
    mpr121_task_step();
    if (io_owner != IO_NONE || mpr_bus.busy()) return;
    reading_device = rr_index;
    rr_index = (rr_index + 1) % 3;
    if (!mpr_ready[reading_device]) return;
    const uint8_t reg = MPR121_TOUCHSTATUS_L;
    if (mpr_bus.start(mpr_addr[reading_device], &reg, 1, 2, MPR121_I2C_UPDATE_TIMEOUT_US))
        io_owner = IO_TOUCH;
}

uint32_t mpr121_get_touch_state(uint8_t device) {
    if (device < 3) {
        return touch_state[device];
    }
    return 0;
}

bool mpr121_is_touched(uint8_t device, uint8_t channel) {
    if (device < 3 && channel < 12) {
        return (touch_state[device] >> channel) & 1;
    }
    return false;
}

uint32_t mpr121_get_error_count(uint8_t device) {
    if (device < 3) {
        return i2c_error_count[device];
    }
    return 0;
}

void mpr121_debug_print() {
    for (int i = 0; i < 3; ++i)
        printf("MPR%u touch=%lu errors=%lu\n", i, touch_state[i], i2c_error_count[i]);
}

// Reset baseline for all MPR121 chips (soft reset)
// ★ 异步: 只启动状态机, 不做任何 I2C / sleep (旧实现同步执行
//   3 × (软复位 + sleep 2ms + 39 次初始化写 + sleep 100ms) ≈ 1.5s,
//   期间主循环完全停摆)
void mpr121_reset_baseline() {
    if (thr_state == THR_IDLE) {
        mpr_compute_config(&thr_touch, &thr_release, &thr_config1, &thr_config2);
        thr_state = THR_RESET_WRITE;
        thr_dev = 0;
        thr_step = 0;
    }
    printf("[MPR121] Baseline reset queued (async)\n");
}

#if DEBUG_MPR121
void mpr121_debug_init() { mpr121_debug_print(); }
void mpr121_debug_tick() {} // diagnostics use cached state only
#endif
} // namespace Chuni245Tof
