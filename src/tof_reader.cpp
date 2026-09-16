/*
 * TOF Reader — Core1 任务
 *
 * Core1 是 I2C1 的唯一所有者:
 *   - I2C1 外设初始化在 Core1 上执行 (vl53l0x_init)
 *   - 5 个 VL53L0X 的轮询、故障检测、恢复全部在 Core1 上执行
 *   - Core0 通过 tof_reader_get_snapshot 无锁读取快照 (sequence counter 保证一致性)
 *
 * 恢复分级（故障设备级 Recovery, 以单个 VL53L0X 为最小恢复单位）:
 *   Level 1  瞬时错误: 记录计数, 继续轮询, 不恢复
 *            (任何成功通信 —— 含 NOT_READY 轮询 —— 都清零连续错误)
 *   Level 2  确认掉线(单传感器): 只对目标执行 XSHUT 硬复位 + 完整重初始化,
 *            其他传感器原地继续测量, 快照/序列号不受影响
 *   Level 3  总线级故障(所有就绪传感器同时静默, 或反复恢复失败且无人应答):
 *            I2C1 总线恢复 (9-clock + STOP, 不动 XSHUT/配置) → 存活甄别:
 *            探测通过的传感器原样保留, 无响应者才进入 Level 2 路径
 *   Level 4  最后手段: 仅当总线恢复失败 → 全传感器 XSHUT 重初始化
 *
 * 掉线判定 = "连续通信错误 + 通信静默时长" 共同确认,
 *          或 "通信正常但长时间无新数据"（测距序列卡死）
 * 总线疑似卡死 (>=2 颗且全部就绪传感器持续通信失败) 时抑制失联下线,
 *          传感器保持就绪等待 Level 3 恢复总线 + 存活甄别
 * 恢复退避: 首次立即尝试, 失败后按 200ms×2^n 退避 (上限 10s), 不影响其他传感器
 * 绝不伪造数据: 恢复期间快照立即失效 (valid=false), 等待真实新数据
 */

#include "tof_reader.h"
#include "vl53l0x.h"
#include "tof_events.h"

#include "pico/time.h"
#include "pico/multicore.h"

#include <string.h>

// 本文件运行在 Core1, 禁止 printf / 任何 TinyUSB API:
// 日志一律通过 tof_event_post() 进入非阻塞事件队列, 由 Core0 的 CDC 输出。

namespace Chuni245Tof {

//==============================================================================
// 常量
//==============================================================================

// 高频轮询: 每 100us 轮询一轮全部传感器
constexpr uint32_t MIN_POLL_SLEEP_US = 100u;

// ---- 恢复相关常量 ----
// 阈值依据 (测量推导, 非拍脑袋):
//   - 单颗传感器一轮轮询: 就绪时 ~0.1-0.3ms; 通信死亡时 3ms (I2C 超时)
//   - 4 颗健康 + 1 颗死亡 → 一轮 ~4ms; 全部死亡 → 一轮 ~15ms
//   - 测距周期 20ms: 健康传感器每 ~21ms 必产出一条新数据、每轮必有一次成功通信

// 连续 I2C 通信错误次数 (Level 2 掉线判定条件之一):
// 8 次连续失败意味着至少 8 轮轮询全部失败 —— 偶发错误必然夹杂成功通信
// 而被清零, 不可能累积到此值
constexpr uint32_t RECOVERY_ERROR_THRESHOLD = 8u;

// 通信静默时长 (ms, Level 2 掉线判定条件之二):
// 连续 8 次错误 且 距上次成功通信超过 40ms (≈2 个测距周期) 才算掉线。
// 两个条件缺一不可 —— 单纯的错误计数或单纯的静默都可能误判
constexpr uint32_t COMM_FAIL_WINDOW_MS = 40u;

// "通信正常但无新数据" 的判定阈值 (ms):
// 20ms 测距周期下 300ms 内应有约 14 次新数据, 超时说明测距序列卡死
constexpr uint32_t NO_DATA_RECOVERY_MS = 300u;

// 总线级静默判定 (ms, Level 3 触发条件之一):
// 所有就绪传感器全部超过 60ms 无任何成功通信 —— 健康传感器最大通信间隔
// 约 21ms (测距周期) + 让出轮询保障, 60ms 的全员静默只能是总线级事件
constexpr uint32_t BUS_SILENCE_MS = 60u;

// 升级判定窗口 (ms, Level 3 触发条件之二):
// 存在反复恢复失败的传感器, 且所有就绪传感器 200ms 内无任何成功通信
constexpr uint32_t BUS_SUSPECT_WINDOW_MS = 200u;

// 触发 Level 2 恢复所需的连续失败次数 (Level 3 触发条件之二):
// 首次立即 + 200/400ms 退避, 3 次失败约 1.4s —— 足够排除个体故障
constexpr uint32_t PER_SENSOR_FAIL_ESCALATION = 3u;

// 总线疑似卡死的单传感器证据下限: 所有(>=2 个)就绪传感器连续错误都达到该值
// 才认为 "全员持续失联" (单次偶发错误不会达到)
constexpr uint32_t BUS_SUSPECT_CONSEC_MIN = 2u;

// 两次总线恢复之间的最小间隔 (ms): 防止总线异常时恢复风暴。
// 连续 "无效果" 的总线恢复 (无人可甄别) 后按 ×2 递增, 上限 ×32
constexpr uint32_t BUS_RECOVERY_COOLDOWN_MS = 500u;
constexpr uint32_t BUS_RECOVERY_COOLDOWN_MAX_SHIFT = 5u;

// 首次恢复失败的退避基准时间 (ms), 之后按 ×2 指数退避
constexpr uint32_t RECOVERY_BACKOFF_BASE_MS = 200u;

// 恢复退避上限 (ms) —— 防止永久死亡的传感器产生持续 I2C 流量
constexpr uint32_t RECOVERY_BACKOFF_MAX_MS = 10000u;

// 恢复成功后若在短时间内再次故障, 视为反复故障, 退避时间随之增长
constexpr uint32_t RECOVERY_REPEAT_WINDOW_MS = 5000u;

// 全局停顿判定阈值 (us): 单轮轮询间隔超过该值说明双核被外部冻结
// (Core0 Flash 擦写时 multicore lockout + XIP 停摆, 典型 50-400ms),
// 而非正常的恢复让出 (正常最大 ~15ms)。此时刷新时间基准, 防止把停顿
// 误判成全员测距卡死而引发恢复风暴。
constexpr uint32_t SYSTEM_STALL_THRESHOLD_US = 150000u;

//==============================================================================
// 类型
//==============================================================================

typedef struct {
    uint16_t distance;      // 距离 (mm), 无效时为 8190
    uint32_t timestamp_ms;  // 本条数据产生时间
    uint32_t sequence;      // 递增序列号（快照一致性检查用）
    bool valid;             // 数据有效
} tof_data_t;

// 单传感器恢复管理状态
typedef struct {
    bool down;                  // 传感器已下线, 等待/正在恢复
    uint32_t last_attempt_ms;   // 上次恢复尝试时间
    uint32_t last_success_ms;   // 上次恢复成功时间
    uint32_t backoff_ms;        // 当前退避时间
    uint32_t fail_count;        // 连续恢复失败次数（决定退避长度）
} sensor_mgmt_t;

//==============================================================================
// 状态
//==============================================================================

static volatile tof_data_t sensor_data[5];
static volatile bool core1_running = false;

// 统计
static volatile uint32_t read_count = 0;
static volatile uint32_t new_data_count = 0;
static volatile uint32_t total_poll_rounds = 0;
static volatile uint32_t max_poll_interval_us = 0;
static volatile uint32_t avg_poll_interval_us = 0;
static volatile uint32_t bus_recovery_count = 0;
static volatile uint32_t core1_heartbeat = 0;   // Core1 活跃性心跳 (每轮 +1)
static volatile uint32_t last_stall_us = 0;     // 最近一次全局停顿的时长

// 恢复状态机
static sensor_mgmt_t mgmt[5];
static bool bus_recovery_in_progress = false;
static uint32_t last_bus_recovery_ms = 0;   // 上次总线恢复完成时刻 (0 = 尚未发生过)
static uint32_t bus_recovery_noop_count = 0; // 连续 "无效果" 总线恢复次数 (决定冷却递增)

//==============================================================================
// 时间差计算
//==============================================================================
// stamp 比 now 新 → 返回 0 (让出轮询会用比本轮开头更晚的时钟打点);
// 通过 int32 回绕解释差值, 32 位 ms 计数回绕 (~49.7 天) 后依然正确 ——
// 普通的 "钳位" 写法在回绕后会把差值永久钳成 0, 使全部掉线检测失明。
// 实践中 stamp 老化在达到阈值前就会触发恢复, 不会出现 >24.8 天的差值。
static inline uint32_t ms_since(uint32_t stamp, uint32_t now)
{
    int32_t delta = (int32_t)(now - stamp);
    return (delta < 0) ? 0u : (uint32_t)delta;
}

//==============================================================================
// 快照 seqlock（Core1 写 / Core0 读）
//==============================================================================
// 写者: sequence++ (奇数, 更新中) → 写入 payload → sequence++ (偶数, 稳定)
// 读者: 读 seq1 (必须为偶数) → 读 payload → 读 seq2; seq1==seq2 且偶数才接受。
// 奇偶括号保证任何 payload 撕裂都必然伴随奇数序列号被读者看到。

static inline void snapshot_begin_write(int i)
{
    sensor_data[i].sequence++;   // → 奇数: 更新进行中
}

static inline void snapshot_end_write(int i)
{
    sensor_data[i].sequence++;   // → 偶数: 更新完成
}

// 使快照立即失效（seqlock 括号内完成, 不伪造数据）
static void snapshot_invalidate(int i)
{
    snapshot_begin_write(i);
    sensor_data[i].valid = false;
    snapshot_end_write(i);
}

//==============================================================================
// 轮询与让出
//==============================================================================

// 轮询所有就绪传感器（互相独立, 个体故障不阻塞其他传感器）。
// 正常轮询与恢复期间的让出回调共用同一函数, 保证统计口径一致。
// 总线级故障不在这里判定 —— 由状态机根据 last_comm_ok 静默时长判定
// (轮询级计数无法区分 "1 颗个体故障" 与 "总线级事件")
static void poll_ready_sensors(uint32_t now_ms)
{
    for (int i = 0; i < 5; i++) {
        // 离线/恢复中的传感器: sensor_ready=false, 不产生任何 I2C 流量
        if (!vl53l0x_is_ready(i)) continue;

        uint16_t distance = 0;
        vl53l0x_read_result_t r = vl53l0x_read_distance_ex(i, &distance);

        if (r == VL53L0X_READ_NEW_DATA) {
            // 真实新数据: seqlock 更新快照
            snapshot_begin_write(i);
            sensor_data[i].distance = distance;
            sensor_data[i].timestamp_ms = now_ms;
            sensor_data[i].valid = true;
            snapshot_end_write(i);
            new_data_count++;
            read_count++;
        }
        // VL53L0X_READ_NOT_READY: 测量进行中, 通信正常
        // VL53L0X_READ_COMM_ERROR: 通信失败, 错误已在驱动内计数;
        //                          不更新快照, 旧数据按时间戳自然过期 (MAX_DATA_AGE_MS)
    }
}

// vl53l0x 驱动在初始化/恢复长等待期间调用的让出回调:
// 继续轮询健康传感器, 保证单传感器恢复期间其他传感器数据不超龄 (50ms)
static volatile bool in_yield_poll = false;
static void yield_poll_healthy(void)
{
    if (in_yield_poll) return;   // 防重入
    in_yield_poll = true;
    poll_ready_sensors(to_ms_since_boot(get_absolute_time()));
    in_yield_poll = false;
}

//==============================================================================
// 恢复辅助
//==============================================================================

// 计算下一次恢复尝试的等待时间:
// fail_count=0 → 立即尝试 (0ms); ≥1 → 200ms × 2^(n-1), 上限 10s
// (即: 首次立即, 失败后 200/400/800/.../10000ms)
static uint32_t next_backoff(uint32_t fail_count)
{
    if (fail_count == 0) return 0;

    uint32_t backoff = RECOVERY_BACKOFF_BASE_MS;
    for (uint32_t i = 1; i < fail_count && backoff < RECOVERY_BACKOFF_MAX_MS; i++) {
        backoff *= 2;
    }
    return backoff > RECOVERY_BACKOFF_MAX_MS ? RECOVERY_BACKOFF_MAX_MS : backoff;
}

// 将传感器下线并使快照立即失效（不伪造数据）
static void take_sensor_offline(int i, uint32_t now_ms)
{
    vl53l0x_force_offline(i);
    snapshot_invalidate(i);

    mgmt[i].down = true;
    mgmt[i].last_attempt_ms = now_ms;

    // 反复故障退避: 若上次恢复成功后很快又故障, 增加退避, 防止无限快速重启
    uint32_t since_success = ms_since(mgmt[i].last_success_ms, now_ms);
    if (mgmt[i].last_success_ms != 0 && since_success < RECOVERY_REPEAT_WINDOW_MS) {
        mgmt[i].fail_count++;
    } else {
        mgmt[i].fail_count = 0;
    }
    mgmt[i].backoff_ms = next_backoff(mgmt[i].fail_count);
}

// 尝试恢复单个传感器
static void attempt_recovery(int i, uint32_t now_ms)
{
    tof_event_post(TOF_EVT_RECOVERY_ATTEMPT, i, mgmt[i].fail_count);
    mgmt[i].last_attempt_ms = now_ms;

    if (vl53l0x_recover_sensor(i)) {
        tof_event_post(TOF_EVT_RECOVERY_OK, i, 0);
        mgmt[i].down = false;
        mgmt[i].fail_count = 0;
        mgmt[i].last_success_ms = now_ms;
        // 快照保持无效, 等待真实新数据后才置 valid
        snapshot_invalidate(i);
    } else {
        tof_event_post(TOF_EVT_RECOVERY_FAIL, i, mgmt[i].fail_count);
        mgmt[i].fail_count++;
        mgmt[i].backoff_ms = next_backoff(mgmt[i].fail_count);
    }
}

//==============================================================================
// 总线级恢复（Level 3 / Level 4）—— 只在确认总线级故障时才允许进入
//==============================================================================

// Level 3 触发条件 a: 所有就绪传感器同时通信静默。
// 任何个体故障（无论几颗）都至少保留一个通信者, 不会满足本条件;
// 全员静默只可能是总线级事件（从机卡死 SDA / 外设异常 / 供电跌落）
static bool all_ready_silent(uint32_t now_ms)
{
    bool any_ready = false;
    for (int i = 0; i < 5; i++) {
        if (!vl53l0x_is_ready(i)) continue;
        any_ready = true;

        uint32_t since_comm = ms_since(vl53l0x_get_last_comm_ok_ms(i), now_ms);
        if (since_comm < BUS_SILENCE_MS) return false;   // 还有传感器在正常通信
    }
    return any_ready;
}

// Level 3 触发条件 b: 存在反复恢复失败的传感器, 且没有任何就绪传感器在成功通信。
// 覆盖 "总线卡死后各传感器已逐个下线" 与 "全部传感器个体死亡" 这类
// 就绪数为 0、条件 a 永远无法覆盖的场景
static bool recovery_escalation_due(uint32_t now_ms)
{
    bool any_escalating = false;
    for (int i = 0; i < 5; i++) {
        if (mgmt[i].down && mgmt[i].fail_count >= PER_SENSOR_FAIL_ESCALATION) {
            any_escalating = true;
            break;
        }
    }
    if (!any_escalating) return false;

    // 只要还有任何一个就绪传感器在成功通信, 总线就是活的, 不升级
    for (int i = 0; i < 5; i++) {
        if (!vl53l0x_is_ready(i)) continue;
        uint32_t since_comm = ms_since(vl53l0x_get_last_comm_ok_ms(i), now_ms);
        if (since_comm < BUS_SUSPECT_WINDOW_MS) return false;
    }
    return true;
}

// 总线恢复 + 存活甄别（Level 3）; 总线恢复失败才进入全量重初始化（Level 4, 最后手段）。
// 关键: 总线恢复本身不动任何 XSHUT、不清任何传感器配置 ——
// 甄别通过的传感器原样保留（快照/序列号/状态全部不动）, 继续测距
static void run_bus_recovery(uint32_t now_ms)
{
    tof_event_post(TOF_EVT_BUS_RECOVERY_START, 0xFF, 0);
    bus_recovery_in_progress = true;
    bus_recovery_count++;

    int ready_before = 0;
    for (int i = 0; i < 5; i++) {
        if (vl53l0x_is_ready(i)) ready_before++;
    }

    bool bus_ok = vl53l0x_bus_recover();
    last_bus_recovery_ms = to_ms_since_boot(get_absolute_time());

    if (!bus_ok) {
        // Level 4 (最后手段): 9-clock 仍无法释放总线 → 全传感器 XSHUT 重初始化
        tof_event_post(TOF_EVT_BUS_RECOVERY_FAIL, 0xFF, 0);
        vl53l0x_reinit_all();

        // 全量重初始化已复位全部传感器: 所有快照失效, 恢复状态重新播种
        // (last_attempt_ms 打点为当前时刻, 使下线传感器的 200ms 基准退避真正生效)
        uint32_t fresh_ms = to_ms_since_boot(get_absolute_time());
        for (int i = 0; i < 5; i++) {
            snapshot_invalidate(i);
            mgmt[i].fail_count = 0;
            mgmt[i].last_attempt_ms = fresh_ms;
            if (vl53l0x_is_ready(i)) {
                mgmt[i].down = false;
                mgmt[i].last_success_ms = fresh_ms;
                mgmt[i].backoff_ms = 0;
            } else {
                mgmt[i].down = true;
                mgmt[i].backoff_ms = RECOVERY_BACKOFF_BASE_MS;
            }
        }
    } else {
        // Level 3 存活甄别: 只甄别就绪传感器, 已下线者由各自恢复路径继续处理
        int survivors = 0, casualties = 0;
        for (int i = 0; i < 5; i++) {
            if (!vl53l0x_is_ready(i)) continue;

            if (vl53l0x_probe_sensor(i)) {
                survivors++;   // 幸存: 快照/序列号/状态不动, 继续原地测距
            } else {
                // 无响应: 转入该传感器的独立 XSHUT 恢复
                // (退避由 take_sensor_offline 的防抖规则决定:
                //  首次故障立即重试; 5s 内反复故障则从 200ms 起指数退避)
                take_sensor_offline(i, to_ms_since_boot(get_absolute_time()));
                casualties++;
            }
        }
        // 幸存者统计: 借 arg16 通道随事件载荷发布
        tof_event_t ev;
        ev.timestamp_ms = to_ms_since_boot(get_absolute_time());
        ev.arg32 = (uint32_t)casualties;
        ev.sensor = 0xFF;
        ev.type = TOF_EVT_BUS_RECOVERY_OK;
        ev.arg16_lo = (uint8_t)survivors;
        ev.arg16_hi = (uint8_t)casualties;
        tof_event_push(&ev);
    }

    // "无效果" 总线恢复 (总线恢复成功但没有任何就绪传感器可甄别 ——
    // 典型场景: 全部传感器个体死亡而总线正常): 冷却时间按 ×2 递增,
    // 防止 2Hz 的永久空转恢复; 任何有效果的恢复将计数清零
    bus_recovery_noop_count = (bus_ok && ready_before == 0)
                                  ? bus_recovery_noop_count + 1 : 0;

    bus_recovery_in_progress = false;
}

// 总线疑似卡死判定: >=2 个就绪传感器 且 全部就绪传感器都处于持续通信失败中。
// 此时抑制 Level 2 的 "通信失联" 下线 (保留测距卡死判定), 让传感器保持就绪
// 等待 Level 3 总线恢复 + 存活甄别 —— 防止总线恢复冷却期内把全部(本可幸存的)
// 传感器逐个 XSHUT 重初始化。
// 个体故障不会误触发: 健康传感器的 consec=0, 只要有一颗就不满足 "全部失败"。
static bool bus_suspected_now(void)
{
    int ready_cnt = 0, failing_cnt = 0;
    for (int i = 0; i < 5; i++) {
        if (!vl53l0x_is_ready(i)) continue;
        ready_cnt++;
        if (vl53l0x_get_consecutive_errors(i) >= BUS_SUSPECT_CONSEC_MIN) {
            failing_cnt++;
        }
    }
    return ready_cnt >= 2 && failing_cnt == ready_cnt;
}

//==============================================================================
// Core1 主循环
//==============================================================================

static void core1_main(void)
{
    core1_running = true;

    // 注册为 flash 操作 lockout victim: Core0 执行 Flash 擦写 (SAVE 命令) 前
    // 会通过 multicore_lockout 把本核停到 RAM 中的安全点, 避免双核 XIP 取指
    // 撞上 Flash 编程窗口 (不注册的话 Core1 会在 Flash 忙碌期间从 XIP 取指,
    // 行为未定义)。lockout 期间本核冻结, 恢复后的停顿检测会善后时间基准。
    multicore_lockout_victim_init();

    // 注册让出回调: 之后所有初始化/恢复的长等待期间都会继续轮询健康传感器
    vl53l0x_set_yield_callback(yield_poll_healthy);

    // Core1 独占 I2C1: 外设初始化 + 全部传感器初始化都在这里完成
    // (每颗 ~100ms, 共 ~500ms, 期间 Core0 正常运行, AIR 数据等待真实测量)
    vl53l0x_init();

    memset((void *)mgmt, 0, sizeof(mgmt));
    for (int i = 0; i < 5; i++) {
        // 初始化成功的传感器从现在开始计时; 失败的传感器进入恢复队列
        mgmt[i].last_success_ms = vl53l0x_is_ready(i) ? to_ms_since_boot(get_absolute_time()) : 0;
        if (!vl53l0x_is_ready(i)) {
            mgmt[i].down = true;
            mgmt[i].backoff_ms = 0;   // 立即尝试第一次恢复
        }
    }

    tof_event_post(TOF_EVT_CORE1_STARTED, 0xFF, 0);

    uint64_t last_round_us = time_us_64();
    uint32_t poll_interval_sum_us = 0;
    uint32_t poll_interval_count = 0;

    while (core1_running) {
        core1_heartbeat++;
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        // ---- 1. 轮询所有就绪传感器 ----
        poll_ready_sensors(now_ms);

        total_poll_rounds++;

        // ---- 2. 总线级故障检测 (Level 3, 仅总线级证据才允许升级) ----
        // 单传感器/部分传感器故障在这里永远不会触发总线恢复:
        //   条件 a 要求 "全部就绪传感器同时静默" —— 只要还有一颗在通信就不满足
        //   条件 b 要求 "反复恢复失败 且 没有任何就绪传感器在成功通信"
        // 冷却期内不重复进入 (防止总线异常时恢复风暴);
        // 连续无效果的恢复会使冷却按 ×2 递增 (上限 ×32)
        bool bus_suspected = false;
        if (!bus_recovery_in_progress) {
            uint32_t shift = bus_recovery_noop_count > BUS_RECOVERY_COOLDOWN_MAX_SHIFT
                                 ? BUS_RECOVERY_COOLDOWN_MAX_SHIFT : bus_recovery_noop_count;
            uint32_t cooldown = BUS_RECOVERY_COOLDOWN_MS << shift;

            if (ms_since(last_bus_recovery_ms, now_ms) >= cooldown &&
                (all_ready_silent(now_ms) || recovery_escalation_due(now_ms))) {
                run_bus_recovery(now_ms);
            }

            bus_suspected = bus_suspected_now();
        }

        // ---- 3. 单传感器恢复状态机（Level 1 判定 + Level 2 恢复）----
        // 每颗传感器完全独立: 一颗的判定/恢复绝不触碰其他传感器的
        // XSHUT/配置/快照/序列号。attempt_recovery 内部的长等待会通过
        // yield 回调持续轮询健康传感器, 因此单个传感器的恢复不会使
        // 其他传感器数据超龄
        if (!bus_recovery_in_progress) {
            for (int i = 0; i < 5; i++) {
                bool ready = vl53l0x_is_ready(i);

                if (!mgmt[i].down && ready) {
                    // 掉线判定 = "连续错误 + 通信静默" 共同确认, 或 "测距卡死":
                    //   a) 通信失联: 连续 8 次通信错误 且 距上次成功通信 >= 40ms
                    //      (任何成功通信 —— 含 NOT_READY 轮询 —— 都会清零连续错误,
                    //       偶发错误永远累积不到阈值)
                    //   b) 测距卡死: 通信正常但 >= 300ms 无新数据
                    // 总线疑似卡死时抑制 a) (见 bus_suspected_now): 传感器保持就绪,
                    // 等待 Level 3 恢复总线后由存活甄别统一处理
                    uint32_t consec = vl53l0x_get_consecutive_errors(i);
                    uint32_t since_comm = ms_since(vl53l0x_get_last_comm_ok_ms(i), now_ms);
                    uint32_t since_data = ms_since(vl53l0x_get_last_new_data_ms(i), now_ms);

                    bool comm_dead = (consec >= RECOVERY_ERROR_THRESHOLD) &&
                                     (since_comm >= COMM_FAIL_WINDOW_MS);
                    bool range_stuck = (since_data >= NO_DATA_RECOVERY_MS);

                    if ((comm_dead && !bus_suspected) || range_stuck) {
                        // 掉线事件 (含原因) 进队列; arg32 打包:
                        // bit31 = 测距卡死 (0=通信失联), bit0-23 = 静默/无数据 ms (取 no_data)
                        uint32_t reason = range_stuck ? 0x80000000u : 0u;
                        tof_event_post(TOF_EVT_OFFLINE, i, reason | (since_data & 0xFFFFFFu));
                        take_sensor_offline(i, now_ms);
                    }
                }

                if (mgmt[i].down &&
                    ms_since(mgmt[i].last_attempt_ms, now_ms) >= mgmt[i].backoff_ms) {
                    attempt_recovery(i, now_ms);
                }
            }
        }

        // ---- 4. 轮询间隔统计 + 全局停顿检测 + 保持 ~100us 轮询节拍 ----
        uint64_t now_us = time_us_64();
        uint32_t interval = (uint32_t)(now_us - last_round_us);
        if (interval > max_poll_interval_us) {
            max_poll_interval_us = interval;
        }

        // 全局停顿检测: 双核同时被冻结 (Core0 Flash 擦写 / multicore lockout /
        // 调试器停止) 后, 本轮间隔会一次性跳变到几十至几百 ms。此时 "无数据"
        // 与 "通信静默" 计时被整体放大, 若不刷新时间基准, 恢复状态机会把
        // 停顿误判成全员测距卡死而引发恢复风暴。刷新后由真实轮询结果重新判定。
        if (interval > SYSTEM_STALL_THRESHOLD_US) {
            uint32_t stall_now = to_ms_since_boot(get_absolute_time());
            vl53l0x_note_global_stall(stall_now);
            for (int i = 0; i < 5; i++) {
                if (mgmt[i].down) {
                    mgmt[i].last_attempt_ms = stall_now;   // 退避计时同样顺延
                }
            }
            last_stall_us = interval;
            tof_event_post(TOF_EVT_STALL_NOTED, 0xFF, interval);
            // 停顿间隔不污染轮询统计: 丢弃当前累计窗口
            poll_interval_sum_us = 0;
            poll_interval_count = 0;
        }

        poll_interval_sum_us += interval;
        poll_interval_count++;
        if (poll_interval_count >= 1000) {
            avg_poll_interval_us = poll_interval_sum_us / poll_interval_count;
            poll_interval_sum_us = 0;
            poll_interval_count = 0;
        }
        last_round_us = now_us;

        sleep_us(MIN_POLL_SLEEP_US);
    }
}

//==============================================================================
// 公共 API（Core0 调用, 无锁快照读取）
//==============================================================================

void tof_reader_init(void)
{
    memset((void *)sensor_data, 0, sizeof(sensor_data));

    multicore_reset_core1();
    multicore_launch_core1(core1_main);
}

tof_data_snapshot_t tof_reader_get_snapshot(uint8_t index)
{
    tof_data_snapshot_t snapshot = { 0, 0, 0, false };

    if (index >= 5) return snapshot;

    // seqlock 读取（无锁）:
    // seq 为偶数 = 更新完成; 前后两次读到相同偶数序列号说明数据未被写穿
    for (int retry = 0; retry < 10; retry++) {
        uint32_t seq1 = sensor_data[index].sequence;
        if (seq1 & 1) {
            continue;   // 奇数 = 写入进行中, 重试
        }

        uint16_t dist = sensor_data[index].distance;
        uint32_t ts = sensor_data[index].timestamp_ms;
        bool valid = sensor_data[index].valid;
        uint32_t seq2 = sensor_data[index].sequence;

        if (seq1 == seq2) {
            snapshot.distance = dist;
            snapshot.timestamp_ms = ts;
            snapshot.sequence = seq2;
            snapshot.valid = valid;
            return snapshot;
        }
    }

    // 10 次重试全部撕裂（极不可能）: 返回无有效数据
    snapshot.valid = false;
    return snapshot;
}

uint16_t tof_reader_get_distance(uint8_t index)
{
    tof_data_snapshot_t s = tof_reader_get_snapshot(index);
    return s.valid ? s.distance : 0;
}

uint32_t tof_reader_get_age(uint8_t index)
{
    tof_data_snapshot_t s = tof_reader_get_snapshot(index);
    if (!s.valid) return 0xFFFFFFFFu;
    return to_ms_since_boot(get_absolute_time()) - s.timestamp_ms;
}

bool tof_reader_is_running(void)
{
    return core1_running;
}

uint32_t tof_reader_get_count(void)
{
    uint32_t count = 0;
    for (int i = 0; i < 5; i++) {
        if (vl53l0x_is_ready(i)) count++;
    }
    return count;
}

uint32_t tof_reader_get_error_count(void)
{
    uint32_t total = 0;
    for (int i = 0; i < 5; i++) {
        total += vl53l0x_get_error_count(i);
    }
    return total;
}

uint32_t tof_reader_get_avg_poll_interval_us(void)
{
    return avg_poll_interval_us;
}

uint32_t tof_reader_get_max_poll_interval_us(void)
{
    return max_poll_interval_us;
}

uint32_t tof_reader_get_new_data_count(void)
{
    return new_data_count;
}

// Core1 活跃性心跳: Core0 两次读取之间计数值增长 = Core1 存活
uint32_t tof_reader_get_heartbeat(void)
{
    return core1_heartbeat;
}

// 最近一次全局停顿的时长 (us), 0 = 自启动以来未检测到
uint32_t tof_reader_get_last_stall_us(void)
{
    return last_stall_us;
}

} // namespace Chuni245Tof
