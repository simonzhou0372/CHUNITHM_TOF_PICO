#pragma once
#include <cstdint>
using absolute_time_t = uint64_t;
extern uint64_t fake_now_us;
inline bool time_reached(absolute_time_t deadline) {
    return fake_now_us++ >= deadline;
}
