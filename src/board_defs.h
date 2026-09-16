/*
 * Hardware Board Definitions
 * Pin assignments for Chuni245Tof Controller
 */

#ifndef BOARD_DEFS_H
#define BOARD_DEFS_H

// I2C Ports
#define I2C0_PORT i2c0
#define I2C0_SDA 16
#define I2C0_SCL 17

#define I2C1_PORT i2c1
#define I2C1_SDA 6
#define I2C1_SCL 7

// MPR121 Touch Controller Addresses
#define MPR121_ADDR_1 0x5A
#define MPR121_ADDR_2 0x5B
#define MPR121_ADDR_3 0x5C

// VL53L0X TOF Sensor XSHUT pins
#define TOF1_XSHUT 5
#define TOF2_XSHUT 4
#define TOF3_XSHUT 3
#define TOF4_XSHUT 2
#define TOF5_XSHUT 1

// Button pins
#define BUTTON_CARD 28
#define BUTTON_TEST 27

// LED pin
#define LED_PIN 25

//------------------------------------------------------------------------------
// 编译期引脚隔离校验 (Recovery Access Boundary 的硬件前提)
//
// I2C0 (MPR121, Core0) 与 I2C1 (VL53L0X, Core1) 是两个完全独立的总线域。
// 上述所有引脚两两不得重叠 —— 若重叠, ToF 恢复期间对 XSHUT/I2C1 引脚的
// GPIO 操作将直接破坏另一条总线的电气状态。此不变量由编译器强制保证。
//------------------------------------------------------------------------------
#ifdef __cplusplus
#include <stddef.h>
constexpr inline unsigned chuni_board_pins[] = {
    I2C0_SDA, I2C0_SCL,                                    // I2C0 (MPR121)
    I2C1_SDA, I2C1_SCL,                                    // I2C1 (VL53L0X)
    TOF1_XSHUT, TOF2_XSHUT, TOF3_XSHUT, TOF4_XSHUT, TOF5_XSHUT, // ToF XSHUT
    BUTTON_CARD, BUTTON_TEST, LED_PIN,
};
constexpr inline bool chuni_board_pins_distinct()
{
    for (size_t i = 0; i < sizeof(chuni_board_pins) / sizeof(chuni_board_pins[0]); ++i)
        for (size_t j = i + 1; j < sizeof(chuni_board_pins) / sizeof(chuni_board_pins[0]); ++j)
            if (chuni_board_pins[i] == chuni_board_pins[j]) return false;
    return true;
}
static_assert(chuni_board_pins_distinct(),
              "I2C0/I2C1/XSHUT/GPIO pin overlap: I2C bus isolation broken");
#endif

#endif /* BOARD_DEFS_H */