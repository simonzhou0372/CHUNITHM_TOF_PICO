#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <vector>

constexpr int PICO_ERROR_GENERIC = -1;
constexpr int PICO_ERROR_TIMEOUT = -2;
constexpr uint32_t I2C_IC_DATA_CMD_CMD_BITS = 1u << 8;
constexpr uint32_t I2C_IC_DATA_CMD_STOP_BITS = 1u << 9;
constexpr uint32_t I2C_IC_DATA_CMD_RESTART_BITS = 1u << 10;
constexpr uint32_t I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS = 1u << 6;
extern uint64_t fake_now_us;
extern unsigned fake_spins;

struct FakeDataCommand {
    std::vector<uint32_t> commands;
    std::deque<uint8_t> received;
    unsigned consumed = 0;
    void operator=(uint32_t command) { commands.push_back(command); }
    explicit operator uint8_t() {
        if (received.empty()) throw std::runtime_error("RX underflow");
        uint8_t result = received.front();
        received.pop_front();
        ++consumed;
        return result;
    }
};

struct i2c_hw_t {
    uint32_t enable = 1, tar = 0, raw_intr_stat = 0;
    volatile uint32_t clr_tx_abrt = 0;
    FakeDataCommand data_cmd;
    uint64_t tx_ready_at = 0, rx_ready_at = 0;
    bool tx_full = false, full_after_first = false;
};
struct i2c_inst_t {
    i2c_hw_t* hw;
    bool restart_on_next;
};
inline i2c_hw_t* i2c_get_hw(i2c_inst_t* instance) { return instance->hw; }
inline size_t i2c_get_write_available(i2c_inst_t* instance) {
    auto* hw = instance->hw;
    return !hw->tx_full && fake_now_us >= hw->tx_ready_at &&
        !(hw->full_after_first && !hw->data_cmd.commands.empty());
}
inline size_t i2c_get_read_available(i2c_inst_t* instance) {
    auto* hw = instance->hw;
    return fake_now_us >= hw->rx_ready_at && !hw->data_cmd.received.empty() &&
        hw->data_cmd.commands.size() > hw->data_cmd.consumed;
}
inline void tight_loop_contents() {
    if (++fake_spins > 10000) throw std::runtime_error("unbounded FIFO wait");
}
