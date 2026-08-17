/*
 * Button Header
 */

#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>
#include <stdbool.h>

namespace Chuni245Tof {

void button_init();
void button_update();
uint8_t button_read();
bool button_card_pressed();
bool button_test_pressed();

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* BUTTON_H */