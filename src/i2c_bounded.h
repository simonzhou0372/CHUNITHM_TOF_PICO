/*
 * Bounded master reads for Pico SDK 2.3.0.
 *
 * The SDK read path waits for TX FIFO space without checking its deadline.
 * Keep its byte-at-a-time RESTART/STOP protocol, but bound both FIFO waits.
 * Each bus still belongs to its calling core. This does not reset a sensor,
 * change GPIOs, touch the other I2C instance, or retry a failed transaction.
 */
#ifndef CHUNI_I2C_BOUNDED_H
#define CHUNI_I2C_BOUNDED_H

#include "hardware/i2c.h"
#include "pico/time.h"
#include <limits.h>

namespace Chuni245Tof {

inline int i2c_read_bounded_until(i2c_inst_t* i2c, uint8_t addr,
                                  uint8_t* dst, size_t len, bool nostop,
                                  absolute_time_t deadline) {
    if (!dst || !len || len > INT_MAX || addr >= 0x80 ||
        (addr & 0x78) == 0 || (addr & 0x78) == 0x78) {
        return PICO_ERROR_GENERIC;
    }

    i2c_hw_t* hw = i2c_get_hw(i2c);
    auto fail = [i2c, hw](int error) {
        // Request disable without waiting for a stuck bus. It flushes the
        // controller FIFOs once disable completes; never reset either sensor
        // bus here. The next call re-enables this same controller, as in SDK.
        hw->enable = 0;
        i2c->restart_on_next = false;
        return error;
    };

    if (time_reached(deadline)) return fail(PICO_ERROR_TIMEOUT);

    hw->enable = 0;
    hw->tar = addr;
    hw->enable = 1;

    for (size_t n = 0; n < len; ++n) {
        // Unlike SDK 2.3.0, a full TX FIFO cannot trap this core forever.
        while (!i2c_get_write_available(i2c)) {
            if (time_reached(deadline)) return fail(PICO_ERROR_TIMEOUT);
            if (hw->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
                (void)hw->clr_tx_abrt;
                return fail(PICO_ERROR_GENERIC);
            }
            tight_loop_contents();
        }
        if (time_reached(deadline)) return fail(PICO_ERROR_TIMEOUT);

        hw->data_cmd = I2C_IC_DATA_CMD_CMD_BITS |
            ((n == 0 && i2c->restart_on_next) ? I2C_IC_DATA_CMD_RESTART_BITS : 0) |
            ((n == len - 1 && !nostop) ? I2C_IC_DATA_CMD_STOP_BITS : 0);

        // One absolute deadline covers the whole read, including later bytes.
        do {
            if (time_reached(deadline)) return fail(PICO_ERROR_TIMEOUT);
            if (hw->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
                (void)hw->clr_tx_abrt;
                return fail(PICO_ERROR_GENERIC);
            }
        } while (!i2c_get_read_available(i2c));

        dst[n] = static_cast<uint8_t>(hw->data_cmd);
    }

    i2c->restart_on_next = nostop;
    return static_cast<int>(len);
}

} // namespace Chuni245Tof
#endif
