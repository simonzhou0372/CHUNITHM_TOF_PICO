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

#endif /* BOARD_DEFS_H */