#pragma once
#include <atomic>
inline void __dmb() { std::atomic_thread_fence(std::memory_order_seq_cst); }
