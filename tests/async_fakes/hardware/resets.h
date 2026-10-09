#pragma once
#include <cstdint>
struct FakeResets { uint32_t reset_done=3; };
extern FakeResets fake_resets;
#define resets_hw (&fake_resets)
void reset_block_mask(uint32_t mask);
void unreset_block_mask(uint32_t mask);
