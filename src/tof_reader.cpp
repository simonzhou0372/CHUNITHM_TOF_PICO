/*
 * TOF Reader — Core1 任务
 *
 * Core1 是 I2C1 的唯一所有者:
 *   - I2C1 外设初始化在 Core1 上执行 (vl53l0x_init)
 *   - 5 个 VL53L0X 的轮询、故障检测、恢复全部在 Core1 上执行
 *   - Core0 通过 tof_reader_get_snapshot 无锁读取快照 (sequence counter 保证一致性)
 *
 * 恢复策略:
 *   - 单传感器: 连续 I2C 错误 / 长时间无新数据 → XSHUT 硬复位 + 完整重初始化
 *   - 总线级:   一轮中 >= 3 个传感器同时通信失败 (连续 2 轮) → I2C1 总线恢复
 *               (9-clock + STOP) + 全传感器 XSHUT 恢复
 *   - 恢复退避: 首次立即尝试, 失败后按 200ms×2^n 退避 (上限 10s), 不影响其他传感器
 *   - 绝不伪造数据: 恢复期间快照立即失效 (valid=false), 等待真实新数据
 */

#include "tof_reader.h"
#include "vl53l0x.h"

#include "pico/time.h"
#include "pico/multicore.h"

#include <string.h>
#include <stdio.h>

namespace Chuni245Tof {

//==============================================================================
// 常量
//==============================================================================

// 高频轮询: 每 100us 轮询一轮全部传感器
constexpr uint32_t MIN_POLL_SLEEP_US = 100u;

// ---- 恢复相关常量 ----

// 连续 I2C 通信错误达到该值 → 触发单传感器恢复（轮询周期 ~100us + 超时 3ms,
// 死亡传感器约 60ms 内累积 10 次 → 快速响应）
constexpr uint32_t RECOVERY_ERROR_THRESHOLD = 10u;

// 首次恢复失败的退避基准时间 (ms), 之后按 ×2 指数退避
constexpr uint32_t RECOVERY_BACKOFF_BASE_MS = 200u;

// 恢复退避上限 (ms) —— 防止永久死亡的传感器产生持续 I2C 流量
constexpr uint32_t RECOVERY_BACKOFF_MAX_MS = 10000u;

// "I2C 正常但无新数据" 的判定阈值 (ms): 20ms 测距周期下 500ms 内应有
// 约 25 次新数据, 超时说明测距序列卡死（中断未置位等）
constexpr uint32_t NO_DATA_RECOVERY_MS = 500u;

// 单轮轮询中同时通信失败的传感器数达到该值 → 判定 I2C 总线故障
// (5 颗中 >=3 颗同时失败, 几乎不可能是传感器个体问题)
constexpr uint32_t BUS_FAULT_MIN_SENSORS = 3u;

// 总线故障需要连续多少轮确认 (防止单次毛刺触发全量恢复)
constexpr uint32_t BUS_FAULT_ROUNDS_TRIGGER = 2u;

// 恢复成功后若在短时间内再次故障, 视为反复故障, 退避时间随之增长
constexpr uint32_t RECOVERY_REPEAT_WINDOW_MS = 5000u;

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

// 恢复状态机
static sensor_mgmt_t mgmt[5];
static uint32_t bus_fault_rounds = 0;
static bool bus_recovery_in_progress = false;

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

// 每轮轮询标记（用于总线故障检测: 按传感器计数, 不受让出重复轮询影响）
static bool round_sensor_polled[5];
static bool round_sensor_failed[5];

// 轮询所有就绪传感器（互相独立, 个体故障不阻塞其他传感器）。
// 正常轮询与恢复期间的让出回调共用同一函数, 保证统计口径一致。
static void poll_ready_sensors(uint32_t now_ms)
{
    for (int i = 0; i < 5; i++) {
        // 离线/恢复中的传感器: sensor_ready=false, 不产生任何 I2C 流量
        if (!vl53l0x_is_ready(i)) continue;

        round_sensor_polled[i] = true;

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
        } else if (r == VL53L0X_READ_COMM_ERROR) {
            // 通信失败: 不更新快照, 旧数据按时间戳自然过期 (MAX_DATA_AGE_MS)
            round_sensor_failed[i] = true;
        }
        // VL53L0X_READ_NOT_READY: 测量进行中, 正常等待
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
    // (last_success_ms 可能由总线恢复路径用更晚的时钟打点, 同样需要钳位防下溢)
    uint32_t since_success = (now_ms >= mgmt[i].last_success_ms)
                                 ? (now_ms - mgmt[i].last_success_ms) : 0u;
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
    printf("[TOF_READER] TOF%d recovery attempt (fail=%u)\n", i + 1, mgmt[i].fail_count);
    mgmt[i].last_attempt_ms = now_ms;

    if (vl53l0x_recover_sensor(i)) {
        printf("[TOF_READER] TOF%d recovered\n", i + 1);
        mgmt[i].down = false;
        mgmt[i].fail_count = 0;
        mgmt[i].last_success_ms = now_ms;
        // 快照保持无效, 等待真实新数据后才置 valid
        snapshot_invalidate(i);
    } else {
        printf("[TOF_READER] TOF%d recovery FAILED\n", i + 1);
        mgmt[i].fail_count++;
        mgmt[i].backoff_ms = next_backoff(mgmt[i].fail_count);
    }
}

//==============================================================================
// Core1 主循环
//==============================================================================

static void core1_main(void)
{
    core1_running = true;

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

    printf("[TOF_READER] Core1 polling started\n");

    uint64_t last_round_us = time_us_64();
    uint32_t poll_interval_sum_us = 0;
    uint32_t poll_interval_count = 0;

    while (core1_running) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        // ---- 1. 轮询所有就绪传感器 ----
        memset(round_sensor_polled, 0, sizeof(round_sensor_polled));
        memset(round_sensor_failed, 0, sizeof(round_sensor_failed));
        poll_ready_sensors(now_ms);

        total_poll_rounds++;

        // ---- 2. 总线级故障检测（只在无总线恢复进行中时计数）----
        // 规则:
        //   a) 单轮中 >= 3 个传感器通信失败 → 总线故障
        //   b) 就绪传感器 >= 2 且本轮全部 I2C 事务都失败 → 总线故障
        //      (覆盖部分传感器已离线后总线才死亡的场景)
        int polled_cnt = 0, failed_cnt = 0;
        if (!bus_recovery_in_progress) {
            for (int i = 0; i < 5; i++) {
                polled_cnt += round_sensor_polled[i] ? 1 : 0;
                failed_cnt += round_sensor_failed[i] ? 1 : 0;
            }

            bool all_ready_failing = (polled_cnt >= 2 && failed_cnt == polled_cnt);
            if (failed_cnt >= (int)BUS_FAULT_MIN_SENSORS || all_ready_failing) {
                bus_fault_rounds++;
            } else {
                bus_fault_rounds = 0;
            }
        }

        if (bus_fault_rounds >= BUS_FAULT_ROUNDS_TRIGGER) {
            printf("[TOF_READER] BUS FAULT detected (%d/%d sensors failing), full recovery\n",
                   failed_cnt, polled_cnt);
            bus_recovery_in_progress = true;
            bus_recovery_count++;

            // 所有传感器下线 + 快照失效 + 复位恢复状态
            for (int i = 0; i < 5; i++) {
                vl53l0x_force_offline(i);
                snapshot_invalidate(i);
                mgmt[i].down = false;
                mgmt[i].fail_count = 0;
                mgmt[i].backoff_ms = RECOVERY_BACKOFF_BASE_MS;
                mgmt[i].last_success_ms = 0;
            }

            // 1) 总线恢复: 9-clock + STOP + I2C 外设重新初始化
            //    (此期间所有传感器已下线, 快照本已全部失效)
            vl53l0x_bus_recover();

            // 2) 全传感器 XSHUT 恢复（地址冲突防护: 逐颗释放）
            vl53l0x_reinit_all();

            // 恢复失败的传感器进入正常单传感器退避恢复路径
            for (int i = 0; i < 5; i++) {
                if (vl53l0x_is_ready(i)) {
                    mgmt[i].last_success_ms = to_ms_since_boot(get_absolute_time());
                } else {
                    mgmt[i].down = true;
                    mgmt[i].backoff_ms = RECOVERY_BACKOFF_BASE_MS;
                }
            }

            bus_fault_rounds = 0;
            bus_recovery_in_progress = false;
        }

        // ---- 3. 单传感器恢复状态机（总线恢复进行中时跳过）----
        // 注意: attempt_recovery 内部的长等待会通过 yield 回调持续轮询
        // 健康传感器, 因此单个传感器的恢复不会使其他传感器数据超龄
        if (!bus_recovery_in_progress) {
            for (int i = 0; i < 5; i++) {
                bool ready = vl53l0x_is_ready(i);

                if (!mgmt[i].down && ready) {
                    // 检测隐性故障:
                    //   a) 连续 I2C 通信错误 → 传感器死亡
                    //   b) I2C 正常但长时间无新数据 → 测距序列卡死
                    // 注意: last_new_data_ms 由轮询(含让出轮询)用采集时刻的时钟打点,
                    // 可能比本轮开头的 now_ms 更新 —— 无符号减法必须钳位,
                    // 否则下溢成 ~4.29e9 会把刚收到数据的健康传感器误判为故障
                    uint32_t consec = vl53l0x_get_consecutive_errors(i);
                    uint32_t last_data = vl53l0x_get_last_new_data_ms(i);
                    uint32_t since_data = (now_ms >= last_data) ? (now_ms - last_data) : 0u;

                    if (consec >= RECOVERY_ERROR_THRESHOLD || since_data > NO_DATA_RECOVERY_MS) {
                        printf("[TOF_READER] TOF%d fault: consec_err=%u no_data=%ums\n",
                               i + 1, consec, since_data);
                        take_sensor_offline(i, now_ms);
                    }
                }

                if (mgmt[i].down && (now_ms - mgmt[i].last_attempt_ms) >= mgmt[i].backoff_ms) {
                    attempt_recovery(i, now_ms);
                }
            }
        }

        // ---- 4. 轮询间隔统计 + 保持 ~100us 轮询节拍 ----
        uint64_t now_us = time_us_64();
        uint32_t interval = (uint32_t)(now_us - last_round_us);
        if (interval > max_poll_interval_us) {
            max_poll_interval_us = interval;
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

} // namespace Chuni245Tof
