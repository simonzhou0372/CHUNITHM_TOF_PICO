/*
 * Button Implementation
 */

#include "button.h"
#include "board_defs.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

namespace Chuni245Tof {

// Button state
static uint8_t button_state = 0;

// Debounce state
static uint8_t debounce_count[2] = {0, 0};
#define DEBOUNCE_THRESHOLD 5

void button_init() {
    // Initialize CARD button
    gpio_init(BUTTON_CARD);
    gpio_set_dir(BUTTON_CARD, GPIO_IN);
    gpio_pull_up(BUTTON_CARD);

    // Initialize TEST button
    gpio_init(BUTTON_TEST);
    gpio_set_dir(BUTTON_TEST, GPIO_IN);
    gpio_pull_up(BUTTON_TEST);

    button_state = 0;
}

void button_update() {
    // Read button states (active low)
    bool card_pressed = !gpio_get(BUTTON_CARD);
    bool test_pressed = !gpio_get(BUTTON_TEST);

    // Debounce CARD button
    if (card_pressed) {
        if (debounce_count[0] < DEBOUNCE_THRESHOLD) {
            debounce_count[0]++;
        }
        if (debounce_count[0] >= DEBOUNCE_THRESHOLD) {
            button_state |= 0x01;
        }
    } else {
        if (debounce_count[0] > 0) {
            debounce_count[0]--;
        }
        if (debounce_count[0] == 0) {
            button_state &= ~0x01;
        }
    }

    // Debounce TEST button
    if (test_pressed) {
        if (debounce_count[1] < DEBOUNCE_THRESHOLD) {
            debounce_count[1]++;
        }
        if (debounce_count[1] >= DEBOUNCE_THRESHOLD) {
            button_state |= 0x02;
        }
    } else {
        if (debounce_count[1] > 0) {
            debounce_count[1]--;
        }
        if (debounce_count[1] == 0) {
            button_state &= ~0x02;
        }
    }
}

uint8_t button_read() {
    return button_state;
}

bool button_card_pressed() {
    return (button_state & 0x01) != 0;
}

bool button_test_pressed() {
    return (button_state & 0x02) != 0;
}

} // namespace Chuni245Tof