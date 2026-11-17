// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "common/types.h"

namespace AmdGpu {

/// sanctuary: Diablo III's dynamic resolution sizes the scene from the GPU time it reads off the
/// GPU clock (DataSelect::GpuClock64, 100 MHz). SANCTUARY_DYNRES=fixed keeps the scene at 100%:
/// every clock value the guest sees runs 64 times slower from the first one on, so the game
/// always finds its GPU time far under budget. Applied to every writer (recording-time and GPU
/// timestamps alike) so the clock stays one monotonic timeline.
inline u64 GuestClock(u64 ticks) {
    static const bool fixed = [] {
        const char* env = std::getenv("SANCTUARY_DYNRES");
        return env && std::strcmp(env, "fixed") == 0;
    }();
    if (!fixed) {
        return ticks;
    }
    static std::atomic<u64> base{0};
    u64 start = base.load(std::memory_order_relaxed);
    if (start == 0) {
        u64 expected = 0;
        base.compare_exchange_strong(expected, ticks, std::memory_order_relaxed);
        start = base.load(std::memory_order_relaxed);
    }
    // 64: even a one-second loading stall reads as about 16 ms, under a 60 FPS frame.
    return ticks <= start ? ticks : start + (ticks - start) / 64;
}

} // namespace AmdGpu
