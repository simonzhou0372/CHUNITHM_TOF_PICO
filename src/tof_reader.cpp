/* Core1 cooperative scheduler. Only this core touches I2C1 and ToF XSHUT.
 * No sleep/alarm pool, recursive yield callbacks, or synchronous recovery.
 * Distance validation/air thresholds remain in the existing driver/air code.
 */
#include "tof_reader.h"
#include "vl53l0x.h"
#include "tof_events.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "hardware/sync.h"
#include <string.h>

namespace Chuni245Tof {
struct tof_data_t {
    uint16_t distance;
    uint32_t timestamp_ms, sequence;
    bool valid;
};
static volatile tof_data_t sensor_data[5];
static volatile bool core1_running = false;
static volatile uint32_t new_data_count = 0, core1_heartbeat = 0;
static volatile uint32_t max_poll_interval_us = 0, avg_poll_interval_us = 0;
static volatile uint32_t last_stall_us = 0, last_progress_us = 0, core1_stage = 0;
static volatile bool flash_victim_ready = false;

static void snapshot_begin_write(int i) {
    sensor_data[i].sequence = sensor_data[i].sequence + 1;
    __dmb();
}
static void snapshot_end_write(int i) {
    __dmb();
    sensor_data[i].sequence = sensor_data[i].sequence + 1;
}
#if CHUNI_TOF_RECOVERY_ENABLE
static void snapshot_invalidate(int i) {
    snapshot_begin_write(i);
    sensor_data[i].valid = false;
    snapshot_end_write(i);
}
static uint32_t ms_since(uint32_t stamp, uint32_t now) {
    int32_t delta = static_cast<int32_t>(now - stamp);
    return delta < 0 ? 0 : static_cast<uint32_t>(delta);
}

struct SensorManagement {
    uint32_t attempted = 0, succeeded = 0, failures = 0, backoff = 0;
    bool down = false;
};
static SensorManagement mgmt[5];
enum Work { Boot, Idle, Sensor, Bus, Probe, Full };
static Work work = Boot;
static uint8_t work_sensor = 0, probe_next = 0;
static uint32_t last_bus_attempt = 0;
static bool need_full_reset = false;

static uint32_t backoff(uint32_t n) {
    if (!n) return 0;
    if (n > 6) return 10000;
    return 200u << (n - 1);
}
static void mark_down(int i, uint32_t now) {
    vl53l0x_force_offline(i);
    snapshot_invalidate(i);
    mgmt[i].down = true;
    mgmt[i].attempted = now;
    mgmt[i].failures = mgmt[i].succeeded && ms_since(mgmt[i].succeeded, now) < 5000
        ? mgmt[i].failures + 1 : 0;
    mgmt[i].backoff = backoff(mgmt[i].failures);
}
static void seed_management(uint32_t now) {
    for (int i = 0; i < 5; ++i) {
        mgmt[i].down = !vl53l0x_is_ready(i);
        mgmt[i].attempted = now;
        mgmt[i].backoff = mgmt[i].down ? 200 : 0;
        mgmt[i].succeeded = mgmt[i].down ? 0 : now;
    }
}
static void management_step(uint32_t now) {
    if (vl53l0x_management_busy()) return;
    if (work == Boot || work == Full) {
        seed_management(now);
        work = Idle;
    } else if (work == Sensor) {
        auto& state = mgmt[work_sensor];
        if (vl53l0x_management_result()) {
            state.down = false;
            state.succeeded = now;
            // Keep repeat-failure history until 5 seconds of healthy service.
            tof_event_post(TOF_EVT_RECOVERY_OK, work_sensor, 0);
        } else {
            state.down = true;
            state.attempted = now;
            if (state.failures < 32) ++state.failures;
            state.backoff = backoff(state.failures);
            tof_event_post(TOF_EVT_RECOVERY_FAIL, work_sensor, state.failures);
        }
        work = Idle;
    } else if (work == Bus) {
        last_bus_attempt = now;
        if (vl53l0x_management_result()) {
            probe_next = 0;
            work = Probe;
            work_sensor = 0xff;
        } else {
            need_full_reset = true;
            work = Idle;
        }
    } else if (work == Probe && work_sensor < 5) {
        if (!vl53l0x_management_result()) mark_down(work_sensor, now);
        work_sensor = 0xff;
    }
#if CHUNI_TOF_RECOVERY_ENABLE
    if (vl53l0x_needs_readdress()) need_full_reset = true;
    if (work == Probe) {
        while (probe_next < 5 && !vl53l0x_is_ready(probe_next)) ++probe_next;
        if (probe_next < 5) {
            if (vl53l0x_start_probe(probe_next)) work_sensor = probe_next++;
            return;
        }
        work = Idle;
    }
    if (need_full_reset) {
        if (vl53l0x_start_reinit_all()) {
            for (int i = 0; i < 5; ++i) snapshot_invalidate(i);
            work = Full;
            need_full_reset = false;
        }
        return;
    }
    bool any_ready = false, all_silent = true, any_escalating = false;
    for (int i = 0; i < 5; ++i) {
        if (vl53l0x_is_ready(i)) {
            any_ready = true;
            if (ms_since(vl53l0x_get_last_comm_ok_ms(i), now) < 60) all_silent = false;
        }
        if (mgmt[i].down && mgmt[i].failures >= 3) any_escalating = true;
    }
    if (all_silent && (any_ready || any_escalating) && ms_since(last_bus_attempt, now) >= 500) {
        if (vl53l0x_start_bus_recovery()) {
            work = Bus;
            last_bus_attempt = now;
            tof_event_post(TOF_EVT_BUS_RECOVERY_START, 0xff, 0);
            return;
        }
    }
    for (uint8_t i = 0; i < 5; ++i) {
        if (vl53l0x_is_ready(i)) {
            bool comm_dead = vl53l0x_get_consecutive_errors(i) >= 8 &&
                ms_since(vl53l0x_get_last_comm_ok_ms(i), now) >= 40;
            bool range_stuck = ms_since(vl53l0x_get_last_new_data_ms(i), now) >= 300;
            // Give shared-bus recovery priority over five individual resets.
            if (!all_silent && (comm_dead || range_stuck)) mark_down(i, now);
        }
        if (mgmt[i].down && ms_since(mgmt[i].attempted, now) >= mgmt[i].backoff) {
            if (vl53l0x_start_recovery(i)) {
                snapshot_invalidate(i);
                work = Sensor;
                work_sensor = i;
                mgmt[i].attempted = now;
                tof_event_post(TOF_EVT_RECOVERY_ATTEMPT, i, mgmt[i].failures);
                return;
            }
        }
    }
#else
    work = Idle;
#endif
}

#endif

static void core1_main() {
    core1_running = true;
    core1_stage = 1;
    flash_victim_ready = flash_safe_execute_core_init();
    vl53l0x_init(); // schedules startup; it does not wait for any sensor
    tof_event_post(TOF_EVT_CORE1_STARTED, 0xff, 0);
    uint8_t index = 0;
    uint64_t next_poll = 0, last_round = time_us_64(), previous = last_round;
    uint32_t sum = 0, rounds = 0;
    while (core1_running) {
        const uint64_t now = time_us_64();
        core1_heartbeat = core1_heartbeat + 1;
        last_progress_us = static_cast<uint32_t>(now);
        // External Flash/debug pause is handled BEFORE fault decisions.
#if CHUNI_TOF_RECOVERY_ENABLE
        if (now - previous > 150000) {
            last_stall_us = static_cast<uint32_t>(now - previous);
            vl53l0x_note_global_stall(static_cast<uint32_t>(now / 1000));
            for (auto& state : mgmt) state.attempted = static_cast<uint32_t>(now / 1000);
        }
#endif
        previous = now;
        core1_stage = 2;
        vl53l0x_task_step();
        core1_stage = 3;
        if (now >= next_poll) {
            uint16_t distance = 0;
            auto result = vl53l0x_read_distance_ex(index, &distance);
            if (result == VL53L0X_READ_NEW_DATA && vl53l0x_is_ready(index)) {
                snapshot_begin_write(index);
                sensor_data[index].distance = distance;
                sensor_data[index].timestamp_ms = to_ms_since_boot(get_absolute_time());
                sensor_data[index].valid = true;
                snapshot_end_write(index);
                new_data_count = new_data_count + 1;
            }
            if (result != VL53L0X_READ_PENDING) {
                index = (index + 1) % 5;
                if (!index) {
                    uint32_t interval = static_cast<uint32_t>(now - last_round);
                    if (interval > max_poll_interval_us) max_poll_interval_us = interval;
                    sum += interval;
                    if (++rounds == 1000) { avg_poll_interval_us = sum / rounds; sum = rounds = 0; }
                    last_round = now;
                    next_poll = now + 100;
                }
            }
        }
        core1_stage = 4;
#if CHUNI_TOF_RECOVERY_ENABLE
        management_step(to_ms_since_boot(get_absolute_time()));
#endif
        core1_stage = 5;
        // Deliberately no sleep_us(): the shared alarm pool is not needed.
    }
}

uint32_t tof_reader_get_progress_age_us() {
    // Read publication first: Core1 may update it between Core0's two reads.
    // Sampling "now" first could otherwise report UINT32_MAX for a live core.
    const uint32_t stamp = last_progress_us;
    return time_us_32() - stamp;
}
uint32_t tof_reader_get_stage() { return core1_stage; }
bool tof_reader_flash_ready() { return flash_victim_ready; }

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

        __dmb();
        uint16_t dist = sensor_data[index].distance;
        uint32_t ts = sensor_data[index].timestamp_ms;
        bool valid = sensor_data[index].valid;
        __dmb();
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
