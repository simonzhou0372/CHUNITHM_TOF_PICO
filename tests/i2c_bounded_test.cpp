// Run the production helper against deterministic FIFO/clock fault injection.
#include "i2c_bounded.h"
#include <cassert>
#include <iostream>
#include <limits>

uint64_t fake_now_us = 0;
unsigned fake_spins = 0;
using Chuni245Tof::i2c_read_bounded_until;

static void reset_clock(uint64_t now = 0) {
    fake_now_us = now;
    fake_spins = 0;
}

int main() {
    {
        reset_clock();
        i2c_hw_t hw;
        hw.data_cmd.received = {0x34, 0x12};
        i2c_inst_t bus{&hw, true};
        uint8_t data[2]{};
        assert(i2c_read_bounded_until(&bus, 0x5a, data, 2, false, 500) == 2);
        assert(data[0] == 0x34 && data[1] == 0x12);
        assert(hw.tar == 0x5a && !bus.restart_on_next);
        assert(hw.data_cmd.commands[0] == (I2C_IC_DATA_CMD_CMD_BITS | I2C_IC_DATA_CMD_RESTART_BITS));
        assert(hw.data_cmd.commands[1] == (I2C_IC_DATA_CMD_CMD_BITS | I2C_IC_DATA_CMD_STOP_BITS));
    }
    {
        reset_clock();
        i2c_hw_t hw;
        hw.tx_ready_at = 40;
        hw.rx_ready_at = 80;
        hw.data_cmd.received = {0xab};
        i2c_inst_t bus{&hw, false};
        uint8_t data = 0;
        assert(i2c_read_bounded_until(&bus, 0x2a, &data, 1, true, 500) == 1);
        assert(data == 0xab && bus.restart_on_next);
        assert(hw.data_cmd.commands[0] == I2C_IC_DATA_CMD_CMD_BITS);
    }
    // Regress the SDK's unchecked TX FIFO loop on BOTH instances.
    for (unsigned stalled_bus = 0; stalled_bus < 2; ++stalled_bus) {
        reset_clock();
        i2c_hw_t hw[2];
        i2c_inst_t buses[2]{{&hw[0], true}, {&hw[1], true}};
        hw[stalled_bus].tx_full = true;
        uint8_t data = 0xcd;
        assert(i2c_read_bounded_until(&buses[stalled_bus], 0x5a, &data, 1, false, 500) == PICO_ERROR_TIMEOUT);
        assert(fake_now_us <= 502 && data == 0xcd);
        assert(hw[stalled_bus].data_cmd.commands.empty());
        assert(hw[stalled_bus].enable == 0 && !buses[stalled_bus].restart_on_next);
        const unsigned other = 1 - stalled_bus;
        assert(hw[other].enable == 1 && hw[other].tar == 0 && buses[other].restart_on_next);
    }
    // RX never arrives, or a later byte stalls: same absolute deadline.
    for (bool first_byte_arrives : {false, true}) {
        reset_clock();
        i2c_hw_t hw;
        if (first_byte_arrives) hw.data_cmd.received = {0x45};
        i2c_inst_t bus{&hw, true};
        uint8_t data[2]{0xcd, 0xcd};
        assert(i2c_read_bounded_until(&bus, 0x5a, data, 2, false, 500) == PICO_ERROR_TIMEOUT);
        assert(fake_now_us <= 502 && data[1] == 0xcd);
        assert(hw.enable == 0 && !bus.restart_on_next);
    }
    {
        reset_clock();
        i2c_hw_t hw;
        hw.full_after_first = true;
        hw.data_cmd.received = {0x55};
        i2c_inst_t bus{&hw, false};
        uint8_t data[2]{};
        assert(i2c_read_bounded_until(&bus, 0x5a, data, 2, false, 500) == PICO_ERROR_TIMEOUT);
        assert(fake_now_us <= 502 && hw.data_cmd.commands.size() == 1);
    }
    for (bool full_fifo : {false, true}) {
        reset_clock();
        i2c_hw_t hw;
        hw.tx_full = full_fifo;
        hw.raw_intr_stat = I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS;
        i2c_inst_t bus{&hw, true};
        uint8_t data = 0xcd;
        assert(i2c_read_bounded_until(&bus, 0x5a, &data, 1, false, 500) == PICO_ERROR_GENERIC);
        assert(data == 0xcd && fake_now_us < 500 && !bus.restart_on_next);
    }
    {
        // Already expired: don't enqueue another command on a broken bus.
        reset_clock(1000);
        i2c_hw_t hw;
        i2c_inst_t bus{&hw, false};
        uint8_t data = 0;
        assert(i2c_read_bounded_until(&bus, 0x5a, &data, 1, false, 999) == PICO_ERROR_TIMEOUT);
        assert(hw.data_cmd.commands.empty());
    }
    {
        // Pico's absolute clock is 64-bit: crossing 32-bit micros must work.
        reset_clock(std::numeric_limits<uint32_t>::max() - 10ull);
        const uint64_t deadline = fake_now_us + 500;
        i2c_hw_t hw;
        hw.tx_full = true;
        i2c_inst_t bus{&hw, false};
        uint8_t data = 0;
        assert(i2c_read_bounded_until(&bus, 0x5a, &data, 1, false, deadline) == PICO_ERROR_TIMEOUT);
        assert(fake_now_us >= deadline && fake_now_us <= deadline + 2);
    }
    std::cout << "PASS: data/RESTART/STOP, delayed FIFO, TX/RX stalls, partial read, "
                 "abort, expired deadline, timer wrap and bus isolation\n";
}
