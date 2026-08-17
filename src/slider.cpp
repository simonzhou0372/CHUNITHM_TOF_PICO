/*
 * Slider Touch Detection Implementation
 * MPR121 mapping:
 *   0x5C: pin 8-1   -> Cell 1-8
 *   0x5B: pin 12-1  -> Cell 9-20
 *   0x5A: pin 12-1  -> Cell 21-32
 */

#include "slider.h"
#include "mpr121.h"
#include "config.h"
#include <string.h>

namespace Chuni245Tof {

// Touch state (32 bits)
static uint32_t touch_state = 0;

void slider_init() {
    mpr121_init();
    touch_state = 0;
}

void slider_update() {
    mpr121_update();

    // Read from each MPR121 and map to cells
    touch_state = 0;

    // MPR121 @ 0x5C: pins 8-1 -> Cell 1-8
    // Pin 8 = Cell 1, Pin 1 = Cell 8
    uint32_t state_5c = mpr121_get_touch_state(2);  // Device 2 = 0x5C
    for (int pin = 1; pin <= 8; pin++) {
        int cell = 9 - pin;  // Pin 8 -> Cell 1, Pin 1 -> Cell 8
        if (state_5c & (1 << (pin - 1))) {
            touch_state |= (1 << (cell - 1));
        }
    }

    // MPR121 @ 0x5B: pins 12-1 -> Cell 9-20
    // Pin 12 = Cell 9, Pin 1 = Cell 20
    uint32_t state_5b = mpr121_get_touch_state(1);  // Device 1 = 0x5B
    for (int pin = 1; pin <= 12; pin++) {
        int cell = 21 - pin;  // Pin 12 -> Cell 9, Pin 1 -> Cell 20
        if (state_5b & (1 << (pin - 1))) {
            touch_state |= (1 << (cell - 1));
        }
    }

    // MPR121 @ 0x5A: pins 12-1 -> Cell 21-32
    // Pin 12 = Cell 21, Pin 1 = Cell 32
    uint32_t state_5a = mpr121_get_touch_state(0);  // Device 0 = 0x5A
    for (int pin = 1; pin <= 12; pin++) {
        int cell = 33 - pin;  // Pin 12 -> Cell 21, Pin 1 = Cell 32
        if (state_5a & (1 << (pin - 1))) {
            touch_state |= (1 << (cell - 1));
        }
    }
}

bool slider_touched(uint8_t cell) {
    if (cell >= 1 && cell <= 32) {
        return (touch_state >> (cell - 1)) & 1;
    }
    return false;
}

uint32_t slider_get_state() {
    return touch_state;
}

} // namespace Chuni245Tof