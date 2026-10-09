#pragma once
#include "vl53l0x.h"
#include "hardware/i2c.h"
#include "hardware/resets.h"
#include "hardware/clocks.h"
#include "pico/time.h"
#include <cstring>

namespace Chuni245Tof {
// One owner/core per instance. step() never waits for hardware. A register read
// is ONE transfer: write pointer, RESTART, read, STOP. TAR is not changed between
// its two halves. No SDK FIFO loops, locks, heap, IRQs or sleep/alarm pool calls.
class AsyncI2c {
public:
    explicit AsyncI2c(i2c_inst_t* port) : port_(port), hw_(i2c_get_hw(port)) {}
    void init(uint32_t baud) {
        const uint32_t hz = clock_get_hz(clk_sys);
        const uint32_t period = (hz + baud / 2) / baud;
        low_ = period * 3 / 5;
        high_ = period - low_;
        hold_ = hz * 3 / 10000000 + 1;
        reset(0);
    }
    bool busy() const { return phase_ != Idle; }
    int result() const { return result_; }
    const uint8_t* data() const { return rx_; }
    uint32_t reset_count() const { return resets_; }
    uint32_t error_count() const { return errors_; }
    uint32_t phase() const { return phase_; }
    bool start(uint8_t addr, const uint8_t* tx, size_t txlen, size_t rxlen, uint32_t timeout_us) {
        if (busy()) return false;
        if (!tx || !txlen || txlen > sizeof(tx_) || rxlen > sizeof(rx_) || addr >= 0x80) {
            result_ = PICO_ERROR_GENERIC;
            return false;
        }
        memcpy(tx_, tx, txlen);
        txlen_ = txlen; rxlen_ = rxlen; addr_ = addr;
        sent_ = received_ = 0;
        result_ = 0;
        deadline_ = time_us_64() + timeout_us;
        hw_->enable = 0;
        phase_ = Disable;
        return true;
    }
    void step() {
        if (phase_ == Idle) return;
        const uint64_t now = time_us_64();
        if (phase_ == Reset) {
            if ((resets_hw->reset_done & mask()) == mask()) {
                configure();
                phase_ = Idle;
            } else if (now >= deadline_) {
                result_ = PICO_ERROR_TIMEOUT;
                phase_ = Idle; // hardware failure still must not trap the core
            }
            return;
        }
        if (now >= deadline_) { reset(PICO_ERROR_TIMEOUT); return; }
        if (phase_ == Disable) {
            // IC_ENABLE=0 is a request, NOT proof that disable completed.
            if (hw_->enable_status & 1u) return;
            (void)static_cast<uint32_t>(hw_->clr_intr);
            hw_->tar = addr_;
            hw_->enable = 1;
            phase_ = Transfer;
            return;
        }
        if (hw_->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
            reset(PICO_ERROR_GENERIC);
            return;
        }
        if (received_ < rxlen_ && i2c_get_read_available(port_)) {
            rx_[received_++] = static_cast<uint8_t>(hw_->data_cmd);
        }
        const size_t total = txlen_ + rxlen_;
        if (sent_ < total && i2c_get_write_available(port_)) {
            uint32_t cmd = sent_ < txlen_ ? tx_[sent_] : I2C_IC_DATA_CMD_CMD_BITS;
            if (sent_ == txlen_) cmd |= I2C_IC_DATA_CMD_RESTART_BITS;
            if (sent_ == total - 1) cmd |= I2C_IC_DATA_CMD_STOP_BITS;
            hw_->data_cmd = cmd;
            ++sent_;
        }
        if (sent_ == total && received_ == rxlen_ &&
            (hw_->raw_intr_stat & I2C_IC_RAW_INTR_STAT_STOP_DET_BITS) &&
            !(hw_->status & I2C_IC_STATUS_MST_ACTIVITY_BITS)) {
            (void)static_cast<uint32_t>(hw_->clr_stop_det);
            port_->restart_on_next = false;
            result_ = static_cast<int>(txlen_ + rxlen_);
            phase_ = Idle;
        }
    }
    // Abort locally by resetting this controller only. Unlike disable/ABORT,
    // reset does not depend on an external slave releasing SDA/SCL.
    void reset(int error) {
        result_ = error;
#if !CHUNI_FAULT_RECOVERY_ENABLE
        if (error < 0) {
            errors_ = errors_ + 1;
            hw_->enable = 0; // terminate failed transaction, no reset/recovery
            port_->restart_on_next = false;
            phase_ = Idle;
            return;
        }
#endif
        if (error < 0) { errors_ = errors_ + 1; resets_ = resets_ + 1; }
        reset_block_mask(mask());
        unreset_block_mask(mask());
        port_->restart_on_next = false;
        deadline_ = time_us_64() + 100;
        phase_ = Reset;
    }
private:
    enum Phase { Idle, Reset, Disable, Transfer };
    uint32_t mask() const { return 1u << I2C_RESET_NUM(port_); }
    void configure() {
        hw_->enable = 0;
        hw_->con = (I2C_IC_CON_SPEED_VALUE_FAST << I2C_IC_CON_SPEED_LSB) |
            I2C_IC_CON_MASTER_MODE_BITS | I2C_IC_CON_IC_SLAVE_DISABLE_BITS |
            I2C_IC_CON_IC_RESTART_EN_BITS | I2C_IC_CON_TX_EMPTY_CTRL_BITS;
        hw_->fs_scl_hcnt = high_; hw_->fs_scl_lcnt = low_;
        hw_->fs_spklen = low_ < 16 ? 1 : low_ / 16;
        hw_->sda_hold = hold_;
        hw_->tx_tl = 0;
        hw_->rx_tl = 0;
        hw_->dma_cr = I2C_IC_DMA_CR_TDMAE_BITS | I2C_IC_DMA_CR_RDMAE_BITS;
        hw_->enable = 1;
    }
    i2c_inst_t* port_;
    i2c_hw_t* hw_;
    volatile Phase phase_ = Idle;
    uint32_t high_ = 0, low_ = 0, hold_ = 0;
    volatile uint32_t resets_ = 0, errors_ = 0;
    uint64_t deadline_ = 0;
    int result_ = 0;
    uint8_t addr_ = 0, tx_[8]{}, rx_[8]{};
    size_t txlen_ = 0, rxlen_ = 0, sent_ = 0, received_ = 0;
};
}
