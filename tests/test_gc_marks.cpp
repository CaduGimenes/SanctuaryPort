// SPDX-License-Identifier: GPL-2.0-or-later
// Texture cache eviction marks (video_core/texture_cache/gc_marks.h, GitHub #61): they compare
// the cache's own images with the room the rest of the process leaves them, with margins scaled
// to the card. No game or Vulkan device required.
#include <cassert>
#include <cstdio>

#include "video_core/texture_cache/gc_marks.h"

using namespace VideoCore::GcMarks;

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    // The #61 measurement on a 1550 MiB test budget: 1689 MiB used, 339 MiB of it images. The old
    // rule pressed at 80% of the budget (1240 MiB) by the whole usage: "pressured" for good. The
    // room is floored at 30% of the budget (465 MiB), the images stay below the critical mark.
    auto m = Compute(1550 * MiB, 1689 * MiB, 339 * MiB);
    assert(m.room == 1550 * MiB / 10 * 3);
    assert(m.critical == m.room - 1550 * MiB / 20);
    assert(339 * MiB < m.critical);
    // A 3 GB card (~2600 MiB budget) with 900 MiB of other allocations: room 1700 MiB, margin
    // 130 MiB; marks at room - 4/2/1 margins.
    m = Compute(2600 * MiB, 1300 * MiB, 400 * MiB);
    assert(m.room == 1700 * MiB);
    const uint64_t margin = 2600 * MiB / 20;
    assert(m.critical == m.room - margin);
    assert(m.pressure == m.room - 2 * margin);
    assert(m.trigger == m.room - 4 * margin);
    assert(400 * MiB < m.trigger);
    // A 16 GB card: the margin stops at 512 MiB.
    m = Compute(15000 * MiB, 2000 * MiB, 1000 * MiB);
    assert(m.room == 14000 * MiB);
    assert(m.critical == m.room - 512 * MiB);
    // Tiny budget: the margin starts at 64 MiB and the marks stay ordered and positive.
    m = Compute(512 * MiB, 0, 0);
    assert(m.room == 512 * MiB);
    assert(m.critical == 512 * MiB - 64 * MiB);
    assert(m.trigger <= m.pressure && m.pressure <= m.critical && m.critical <= m.room);
    // Images above the room: the other allocations count only what is not images.
    m = Compute(4000 * MiB, 3000 * MiB, 2500 * MiB);
    assert(m.room == 3500 * MiB);
    // Usage reported below the images (guest sizes are an estimate): nothing else counted.
    m = Compute(4000 * MiB, 100 * MiB, 500 * MiB);
    assert(m.room == 4000 * MiB);
    for (uint64_t budget = 256; budget <= 32768; budget += 256) {
        for (uint64_t other = 0; other <= budget * 2; other += budget / 4) {
            const auto x = Compute(budget * MiB, (other + 300) * MiB, 300 * MiB);
            assert(x.trigger <= x.pressure && x.pressure <= x.critical && x.critical <= x.room);
            assert(x.trigger > 0);
        }
    }
    std::puts("gc marks: ok");
    return 0;
}
