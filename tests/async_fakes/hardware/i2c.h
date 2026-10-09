#pragma once
#include <cstdint>
#include <cstddef>
#include <deque>
#include <vector>
constexpr int PICO_ERROR_GENERIC=-1, PICO_ERROR_TIMEOUT=-2;
constexpr uint32_t I2C_IC_DATA_CMD_CMD_BITS=0x100, I2C_IC_DATA_CMD_STOP_BITS=0x200,
    I2C_IC_DATA_CMD_RESTART_BITS=0x400, I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS=0x40,
    I2C_IC_RAW_INTR_STAT_STOP_DET_BITS=0x200, I2C_IC_STATUS_MST_ACTIVITY_BITS=0x20;
constexpr uint32_t I2C_IC_CON_SPEED_VALUE_FAST=2, I2C_IC_CON_SPEED_LSB=1,
    I2C_IC_CON_MASTER_MODE_BITS=1, I2C_IC_CON_IC_SLAVE_DISABLE_BITS=0x40,
    I2C_IC_CON_IC_RESTART_EN_BITS=0x20, I2C_IC_CON_TX_EMPTY_CTRL_BITS=0x100,
    I2C_IC_DMA_CR_TDMAE_BITS=2, I2C_IC_DMA_CR_RDMAE_BITS=1;
struct i2c_hw_t;
struct DataReg {
    i2c_hw_t* hw;
    void operator=(uint32_t value);
    explicit operator uint8_t();
};
struct ClearReg {
    i2c_hw_t* hw;
    operator uint32_t() const;
};
struct EnableReg {
    i2c_hw_t* hw;
    uint32_t value=0;
    void operator=(uint32_t value);
};
struct i2c_hw_t {
    explicit i2c_hw_t(unsigned number): number(number), enable{this}, data_cmd{this}, clr_intr{this}, clr_stop_det{this} {}
    unsigned number;
    uint32_t enable_status=0, tar=0, raw_intr_stat=0, status=0;
    uint32_t con=0, fs_scl_hcnt=0, fs_scl_lcnt=0, fs_spklen=0, sda_hold=0, tx_tl=0, rx_tl=0, dma_cr=0;
    EnableReg enable;
    DataReg data_cmd;
    ClearReg clr_intr, clr_stop_det;
    std::vector<uint32_t> commands;
    std::vector<uint8_t> tx;
    std::deque<uint8_t> rx;
    unsigned read_index=0;
    bool full=false, no_rx=false, no_stop=false, disable_stuck=false;
};
struct i2c_inst_t { i2c_hw_t* hw; bool restart_on_next=false; };
extern i2c_inst_t fake_i2c[2];
#define i2c0 (&fake_i2c[0])
#define i2c1 (&fake_i2c[1])
#define I2C_RESET_NUM(port) ((port)->hw->number)
inline i2c_hw_t* i2c_get_hw(i2c_inst_t* p) { return p->hw; }
inline size_t i2c_get_write_available(i2c_inst_t* p) { return p->hw->full ? 0 : 16; }
inline size_t i2c_get_read_available(i2c_inst_t* p) { return p->hw->rx.size(); }
