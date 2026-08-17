/*
 * Flash Save Header
 */

#ifndef SAVE_H
#define SAVE_H

#include <stdint.h>
#include "pico/mutex.h"

namespace Chuni245Tof {

void save_init(uint32_t magic, mutex_t* mutex);
bool save_load(void* data, uint32_t len);
bool save_write(const void* data, uint32_t len);
void save_loop();

} // namespace Chuni245Tof

using namespace Chuni245Tof;

#endif /* SAVE_H */