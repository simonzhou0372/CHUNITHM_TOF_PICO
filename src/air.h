/*
 * Air Sensor Header
 */

#ifndef AIR_H
#define AIR_H

#include <stdint.h>
#include <stdbool.h>

namespace Chuni245Tof {

void air_init();
void air_update();
uint8_t air_get_bitmap();
bool air_is_triggered(uint8_t sensor);
uint16_t air_get_distance(uint8_t sensor);
void air_set_threshold(uint8_t sensor, uint16_t threshold_mm);

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* AIR_H */