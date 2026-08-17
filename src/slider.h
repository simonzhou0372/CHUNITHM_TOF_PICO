/*
 * Slider Touch Detection Header
 */

#ifndef SLIDER_H
#define SLIDER_H

#include <stdint.h>
#include <stdbool.h>

namespace Chuni245Tof {

void slider_init();
void slider_update();
bool slider_touched(uint8_t channel);
uint32_t slider_get_state();

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* SLIDER_H */