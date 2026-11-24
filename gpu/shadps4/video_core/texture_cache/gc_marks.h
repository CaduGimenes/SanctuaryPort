// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

// sanctuary (GitHub #61): when the texture cache starts evicting images. The collector can only
// free images, so it compares the bytes of its own images with the room they have: the driver's
// budget minus what the rest of the process holds (buffer arenas, page tables, the port's own
// images, the system heap on integrated GPUs). Before, it compared the whole process usage with
// fixed percentages of the budget: with ~1.2 GB of buffers it sat "pressured" on a small budget
// and evicted nothing, as every image was in use.
// The margins below the room scale with the card: a twentieth of its budget, 64 MiB to 512 MiB
// (3 GB card: ~130 MiB; 8 GB: ~360 MiB). No game or Vulkan device required (tests/test_gc_marks.cpp).
namespace VideoCore::GcMarks {

constexpr uint64_t MiB = uint64_t{1} << 20;

struct Marks {
    uint64_t room;     // bytes the images may use
    uint64_t trigger;  // image bytes from which the collector looks for old images
    uint64_t pressure; // ... evicts more, older ones first, writing back GPU-written ones
    uint64_t critical; // ... evicts aggressively
};

// budget: the driver's current budget (or SANCTUARY_GC_BUDGET_MB); usage: the process's device
// memory usage; images: the bytes of the cache's registered images.
constexpr Marks Compute(uint64_t budget, uint64_t usage, uint64_t images) {
    const uint64_t other = usage > images ? usage - images : 0;
    // The images keep at least 30% of the budget: when other allocations alone fill it, evicting
    // every image would not make room, only thrash.
    const uint64_t floor = budget / 10 * 3;
    const uint64_t room = std::max(budget > other ? budget - other : 0, floor);
    const uint64_t margin = std::clamp(budget / 20, 64 * MiB, 512 * MiB);
    const auto below = [room](uint64_t bytes, uint64_t percent_floor) {
        const uint64_t at_least = room / 100 * percent_floor;
        return std::max(room > bytes ? room - bytes : 0, at_least);
    };
    return Marks{
        .room = room,
        .trigger = below(4 * margin, 50),
        .pressure = below(2 * margin, 65),
        .critical = below(margin, 80),
    };
}

} // namespace VideoCore::GcMarks
