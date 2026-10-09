#pragma once
#include <cstdint>
constexpr int clk_sys=0;
inline uint32_t clock_get_hz(int) { return 125000000; }
