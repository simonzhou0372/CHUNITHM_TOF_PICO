#pragma once
#include <cstdint>
using absolute_time_t=uint64_t;
extern uint64_t fake_clock;
extern void (*fake_time_hook)();
inline uint64_t time_us_64() {
    ++fake_clock;
    if (fake_time_hook) fake_time_hook();
    return fake_clock;
}
inline uint32_t time_us_32() { return static_cast<uint32_t>(time_us_64()); }
inline absolute_time_t get_absolute_time() { return time_us_64(); }
inline uint32_t to_ms_since_boot(absolute_time_t t) { return static_cast<uint32_t>(t/1000); }
