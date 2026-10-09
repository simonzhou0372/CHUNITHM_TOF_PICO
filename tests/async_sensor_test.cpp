#include "i2c_async.h"
#include "cooperative_task.h"
#include "mpr121.h"
#include "config.h"
#include "tof_events.h"
#include <cassert>
#include <array>
#include <iostream>
#include <algorithm>

// Include scheduler to run the real Core1 loop against the simulated clock.
// Drivers themselves are compiled as separate translation units.
#include "../src/tof_reader.cpp"
#undef printf

uint64_t fake_clock=0;
void (*fake_time_hook)()=nullptr;
i2c_hw_t fake_hw[2]{i2c_hw_t(0),i2c_hw_t(1)};
i2c_inst_t fake_i2c[2]{{&fake_hw[0]},{&fake_hw[1]}};
FakeResets fake_resets;
static uint32_t reset_events[2]{};
static bool reset_never_finishes=false, bus_stuck=false;
struct FakeSensor {
    bool active=false, missing=false, spad_stall=false, calibration_stall=false;
    uint8_t addr=0x29, page=0;
    uint8_t regs[8][256]{};
    uint64_t next_sample=0;
};
static FakeSensor sensors[5];
static uint8_t mpr_regs[3][256]{};
static std::vector<std::array<unsigned,3>> mpr_writes;

void EnableReg::operator=(uint32_t n) {
    value=n;
    if (!hw->disable_stuck) hw->enable_status=n&1;
}
ClearReg::operator uint32_t() const {
    hw->raw_intr_stat=0;
    hw->tx.clear(); hw->read_index=0;
    return 0;
}
void reset_block_mask(uint32_t bits) {
    for (unsigned i=0;i<2;++i) if (bits&(1u<<i)) {
        ++reset_events[i];
        fake_resets.reset_done &= ~(1u<<i);
        auto& hw=fake_hw[i];
        hw.rx.clear(); hw.tx.clear(); hw.commands.clear(); hw.read_index=0;
        hw.raw_intr_stat=hw.status=hw.enable_status=0;
        hw.enable.value=0;
    }
}
void unreset_block_mask(uint32_t bits) { if (!reset_never_finishes) fake_resets.reset_done |= bits; }
void gpio_put(unsigned pin, bool value) {
    if (pin<1 || pin>5) return;
    FakeSensor& sensor=sensors[5-pin];
    sensor.active=value;
    if (!value) {
        sensor.addr=0x29; sensor.page=0; sensor.next_sample=0;
        std::fill(&sensor.regs[0][0], &sensor.regs[0][0]+sizeof(sensor.regs), 0);
        sensor.regs[0][0xc0]=0xee;
        sensor.regs[1][0x91]=0xab;
        sensor.regs[7][0x92]=0x8f;
        for (int i=0;i<6;++i) sensor.regs[0][0xb0+i]=0xff;
    }
}
bool gpio_get(unsigned pin) { return !(bus_stuck && (pin==6 || pin==7)); }
static FakeSensor* find_sensor(uint8_t addr) {
    FakeSensor* found=nullptr;
    for (auto& sensor:sensors) if (sensor.active && sensor.addr==addr) {
        // Identical MODEL_ID reads can ACK even when several reset sensors
        // share 0x29. Configuration/address WRITES must never reach this state.
        found=&sensor;
    }
    return found;
}
void DataReg::operator=(uint32_t n) {
    hw->commands.push_back(n);
    if (hw->number==1 && bus_stuck) return; // no ACK, no STOP, timeout
    FakeSensor* sensor=hw->number==1 ? find_sensor(hw->tar) : nullptr;
    if (hw->number==1 && (!sensor || sensor->missing)) {
        hw->raw_intr_stat |= I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS;
        return;
    }
    if (n&I2C_IC_DATA_CMD_CMD_BITS) {
        assert(hw->tx.size()==1);
        uint8_t reg=hw->tx[0]+hw->read_index++;
        uint8_t value=0;
        if (sensor) {
            value=sensor->regs[sensor->page][reg];
            if (sensor->page==7 && reg==0x83) value=sensor->spad_stall ? 0 : 1;
            if (sensor->page==0 && reg==0x13)
                value=(sensor->regs[0][1]==1 || sensor->regs[0][1]==2 || fake_clock>=sensor->next_sample) ? 7 : 0;
            if (sensor->page==0 && reg==0x13 && sensor->calibration_stall &&
                (sensor->regs[0][1]==1 || sensor->regs[0][1]==2)) value=0;
            if (sensor->page==0 && reg==0x1e) value=0;
            if (sensor->page==0 && reg==0x1f) value=150;
        } else {
            assert(hw->tar>=0x5a && hw->tar<=0x5c);
            value=mpr_regs[hw->tar-0x5a][reg];
        }
        if (!hw->no_rx) hw->rx.push_back(value);
    } else hw->tx.push_back(static_cast<uint8_t>(n));
    if (sensor && hw->tx.size()>1) {
        unsigned targets=0;
        for (auto& item:sensors) targets += item.active && item.addr==hw->tar;
        assert(targets==1 && "broadcast configuration/address write after shared reset");
    }
    if (n&I2C_IC_DATA_CMD_STOP_BITS) {
        if (!(n&I2C_IC_DATA_CMD_CMD_BITS)) {
            assert(hw->tx.size()>=2);
            const uint8_t start=hw->tx[0];
            for (size_t i=1;i<hw->tx.size();++i) {
                uint8_t reg=start+i-1, value=hw->tx[i];
                if (sensor) {
                    sensor->regs[sensor->page][reg]=value;
                    if (reg==0xff) sensor->page=value;
                    assert(sensor->page<8);
                    if (sensor->page==0 && reg==0x8a) sensor->addr=value;
                    if (sensor->page==0 && reg==0x0b && value==1) sensor->next_sample=fake_clock+20000;
                } else {
                    mpr_regs[hw->tar-0x5a][reg]=value;
                    if (reg==0x80) mpr_regs[hw->tar-0x5a][0x5c]=0x10;
                    mpr_writes.push_back({hw->tar,reg,value});
                }
            }
        }
        if (!hw->no_stop) hw->raw_intr_stat |= I2C_IC_RAW_INTR_STAT_STOP_DET_BITS;
    }
}
DataReg::operator uint8_t() {
    assert(!hw->rx.empty());
    uint8_t v=hw->rx.front(); hw->rx.pop_front(); return v;
}
namespace Chuni245Tof {
static config_t test_cfg{1,3,2,120,30,150,100,1,{1500,1500,1500,1500,1500}};
config_t* cfg=&test_cfg;
uint8_t config_get_barrier_mode() { return 2; }
void tof_event_post(uint8_t,uint8_t,uint32_t) {}
void tof_event_push(const tof_event_t*) {}
}

static void transport_tests() {
    using namespace Chuni245Tof;
    AsyncI2c bus(i2c0);
    bus.init(400000); bus.step(); assert(!bus.busy());
    uint8_t reg=0;
    mpr_regs[0][0]=0x34; mpr_regs[0][1]=0x12;
    assert(bus.start(0x5a,&reg,1,2,500));
    for (unsigned i=0; bus.busy() && i<1000; ++i) bus.step();
    assert(!bus.busy() && bus.result()==3);
    assert(bus.data()[0]==0x34 && bus.data()[1]==0x12);
    auto commands=fake_hw[0].commands;
    assert(commands[commands.size()-2] & I2C_IC_DATA_CMD_RESTART_BITS);
    assert(commands.back() & I2C_IC_DATA_CMD_STOP_BITS);
    const auto other_resets=reset_events[1];
    for (int fault=0;fault<5;++fault) {
        auto& hw=fake_hw[0];
        hw.full=fault==0; hw.no_rx=fault==1; hw.no_stop=fault==2;
        hw.disable_stuck=fault==3;
        if (fault==3) hw.enable_status=1;
        reset_never_finishes=fault==4;
        if (fault==4) hw.full=true;
        const auto started=fake_clock;
        assert(bus.start(0x5a,&reg,1,2,500));
        for (int i=0;bus.busy() && i<1000;++i) bus.step();
        assert(!bus.busy() && bus.result()==PICO_ERROR_TIMEOUT);
        assert(fake_clock-started<=605);
        assert(reset_events[1]==other_resets);
        hw.full=hw.no_rx=hw.no_stop=hw.disable_stuck=false;
        reset_never_finishes=false;
        bus.init(400000); bus.step();
        assert(bus.start(0x5a,&reg,1,2,500));
        for (int i=0;bus.busy() && i<1000;++i) bus.step();
        assert(bus.result()==3); // recovery after stale/partial FIFO state
    }
}
static uint64_t scenario_start;
static uint32_t healthy_samples=0, single_fault_samples=0, recovered_samples=0;
static bool hook_active=false;
static bool pair_reset=false, all_reset=false;
static void scenario_hook() {
    if (hook_active) return;
    hook_active=true;
    using namespace Chuni245Tof;
    const auto elapsed=fake_clock-scenario_start;
    sensors[1].spad_stall=elapsed<500000;
    sensors[3].calibration_stall=elapsed<700000;
    sensors[2].missing=elapsed>=1200000 && elapsed<1550000;
    bus_stuck=elapsed>=2000000 && elapsed<2250000;
    if (elapsed>=3400000 && !pair_reset) {
        pair_reset=true;
        for (unsigned pin : {1u,3u}) { gpio_put(pin,false); gpio_put(pin,true); }
    }
    if (elapsed>=4400000 && !all_reset) {
        all_reset=true;
        for (unsigned pin=1;pin<=5;++pin) { gpio_put(pin,false); gpio_put(pin,true); }
    }
    mpr121_update(); // stand-in for Core0 servicing HID between every call
    if (elapsed>950000 && elapsed<1150000) healthy_samples=tof_reader_get_new_data_count();
    if (elapsed>1500000 && elapsed<1600000) single_fault_samples=tof_reader_get_new_data_count();
    if (elapsed>2900000) recovered_samples=tof_reader_get_new_data_count();
    if (elapsed>=5500000) core1_running=false;
    hook_active=false;
}
int main() {
    using namespace Chuni245Tof;
    transport_tests();
    mpr121_init();
    scenario_start=fake_clock;
    fake_time_hook=scenario_hook;
    core1_main();
    fake_time_hook=nullptr;
    assert(TaskFrames::failures==0);
    assert(healthy_samples>10);
    assert(single_fault_samples>healthy_samples+10); // other sensors stay live
    assert(recovered_samples>single_fault_samples+10);
    for (int i=0;i<5;++i) {
        assert(vl53l0x_is_ready(i));
        assert(tof_reader_get_age(i)<50);
        assert(sensors[i].addr==0x30+i);
    }
    assert(vl53l0x_get_recovery_count(2)>0);
    assert(vl53l0x_get_bus_resets()>0);
    assert(mpr121_get_touch_state(0)==0x1234);
    for (int d=0;d<3;++d) {
        assert(mpr_regs[d][0x5c]==0x20 && mpr_regs[d][0x5d]==0x22 && mpr_regs[d][0x5e]==0x8c);
        for (int ch=0;ch<12;++ch) assert(mpr_regs[d][0x41+ch*2]==3 && mpr_regs[d][0x42+ch*2]==2);
        const uint8_t filters[]{1,1,14,0,1,5,1,0,0,0,0};
        for (int i=0;i<11;++i) assert(mpr_regs[d][0x2b+i]==filters[i]);
    }
    // Commands may arrive between any two bytes of a previous CONFIG.
    mpr121_set_thresholds(10,8);
    for (int i=0;i<100;++i) mpr121_update();
    mpr121_set_thresholds(7,5);
    for (int i=0;i<100000 && mpr121_op_busy();++i) mpr121_update();
    assert(!mpr121_op_busy());
    for (int d=0;d<3;++d) for (int ch=0;ch<12;++ch)
        assert(mpr_regs[d][0x41+ch*2]==7 && mpr_regs[d][0x42+ch*2]==5);
    // RESET must reapply the saved configuration, with no sleep or USB wait.
    mpr121_reset_baseline();
    for (int i=0;i<1000000 && mpr121_op_busy();++i) mpr121_update();
    assert(!mpr121_op_busy());
    for (int d=0;d<3;++d) {
        assert(mpr_regs[d][0x5e]==0x8c);
        for (int ch=0;ch<12;++ch) assert(mpr_regs[d][0x41+ch*2]==3 && mpr_regs[d][0x42+ch*2]==2);
    }
    unsigned frames_in_use=0;
    for (bool used : TaskFrames::used) frames_in_use += used;
    assert(frames_in_use<=6); // completed jobs reclaimed; no frame leak over 600+ samples
    std::cout << "PASS: FIFO/RX/STOP/disable/reset faults; exact RESTART/STOP; instance isolation; "
                 "SPAD/calibration stalls; 5-sensor init/budget verification; individual/bus/shared-reset recovery; "
                 "MPR config/touch and overlapping CONFIG/RESET preserved. "
              << "samples=" << recovered_samples << " heartbeat=" << core1_heartbeat << "\n";
}
