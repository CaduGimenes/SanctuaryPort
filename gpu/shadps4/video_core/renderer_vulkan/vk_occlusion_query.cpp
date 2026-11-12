// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdio>
#include <cstdlib>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_occlusion_query.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {
constexpr u64 ValidMask = 0x8000000000000000ULL;

bool Trace() {
    static const bool enabled = [] {
        const char* env = std::getenv("SANCTUARY_OCCLUSION_TRACE");
        return env && env[0] == '1';
    }();
    return enabled;
}
} // namespace

OcclusionQueries::OcclusionQueries(const Instance& instance, Scheduler& scheduler_)
    : device{instance.GetDevice()}, scheduler{scheduler_} {
    const vk::QueryPoolCreateInfo info = {
        .queryType = vk::QueryType::eOcclusion,
        .queryCount = PoolSize,
    };
    pool = Check<"create occlusion query pool">(device.createQueryPoolUnique(info));
    if (instance.IsOcclusionQueryPreciseSupported()) {
        control = vk::QueryControlFlagBits::ePrecise;
    }
    free_slots.reserve(PoolSize);
    for (u32 i = PoolSize; i-- > 0;) {
        free_slots.push_back(i);
    }
    scheduler.SetCmdbufHooks([this](vk::CommandBuffer cmdbuf) { Suspend(cmdbuf); },
                             [this](vk::CommandBuffer cmdbuf) { Resume(cmdbuf); });
}

OcclusionQueries::~OcclusionQueries() {
    scheduler.SetCmdbufHooks({}, {});
}

bool OcclusionQueries::AllocateSlot(u32& slot) {
    if (free_slots.empty()) {
        return false;
    }
    slot = free_slots.back();
    free_slots.pop_back();
    return true;
}

bool OcclusionQueries::Dump(u64* results, s32 pairs) {
    if (pairs <= 0) {
        return false;
    }
    // Held while recording: a submission (Suspend) must see the query as recorded.
    std::scoped_lock lk{mutex};
    if (has_active) {
        has_active = false;
        Active query = std::move(active);
        scheduler.EndRendering();
        if (!query.suspended) {
            scheduler.Record([pool = *pool, slot = query.slots.back()](vk::CommandBuffer cmdbuf) {
                cmdbuf.endQuery(pool, slot);
            });
        }
        if (query.begin + 1 == results && query.pairs == pairs) {
            // End dump: read every slot once the GPU is done.
            Finish(std::move(query), true);
            return true;
        }
        // Another begin before the end: drop the open one (its slots come back once the GPU
        // is done with them) and start this one.
        Finish(std::move(query), false);
    }
    u32 slot;
    if (!AllocateSlot(slot)) {
        return false;
    }
    has_active = true;
    active = Active{results, pairs, next_generation++, {slot}, false};
    generations[results] = active.generation;
    // Outside render passes, so the query may span the render passes of its draws.
    scheduler.EndRendering();
    scheduler.Record([pool = *pool, slot, flags = control](vk::CommandBuffer cmdbuf) {
        cmdbuf.resetQueryPool(pool, slot, 1);
        cmdbuf.beginQuery(pool, slot, flags);
    });
    // The begin slots stay as the game's DMA fill left them (zero, not valid) until the
    // end result is known: both halves are written together.
    return true;
}

void OcclusionQueries::Suspend(vk::CommandBuffer cmdbuf) {
    std::scoped_lock lk{mutex};
    if (has_active && !active.suspended) {
        cmdbuf.endQuery(*pool, active.slots.back());
        active.suspended = true;
    }
}

void OcclusionQueries::Resume(vk::CommandBuffer cmdbuf) {
    std::scoped_lock lk{mutex};
    if (!has_active || !active.suspended) {
        return;
    }
    u32 slot;
    if (!AllocateSlot(slot)) {
        return; // stays suspended: the rest of the query is not counted
    }
    cmdbuf.resetQueryPool(*pool, slot, 1);
    cmdbuf.beginQuery(*pool, slot, control);
    active.slots.push_back(slot);
    active.suspended = false;
}

void OcclusionQueries::Finish(Active&& query, bool write) {
    scheduler.DeferPriorityOperation([this, query = std::move(query), write] {
        if (!write) {
            std::scoped_lock lk{mutex};
            for (const u32 slot : query.slots) {
                free_slots.push_back(slot);
            }
            return;
        }
        u64 samples = 0;
        bool ok = true;
        for (const u32 slot : query.slots) {
            u64 value = 0;
            const vk::Result result = device.getQueryPoolResults(
                *pool, slot, 1, sizeof(value), &value, sizeof(value),
                vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
            if (result != vk::Result::eSuccess) {
                ok = false;
            }
            samples += value;
        }
        std::scoped_lock lk{mutex};
        for (const u32 slot : query.slots) {
            free_slots.push_back(slot);
        }
        const auto it = generations.find(query.begin);
        if (it == generations.end() || it->second != query.generation) {
            return; // the game restarted this block meanwhile
        }
        generations.erase(it);
        if (!ok) {
            samples = 1; // unknown: report visible, as before the real counters
        }
        if (static const char* force = std::getenv("SANCTUARY_OCCLUSION_FORCE"); force) {
            samples = std::strtoull(force, nullptr, 0); // debugging: a fixed count
        }
        // The game sums end - begin over the pairs: the whole count goes to the first pair.
        u64* slot = query.begin;
        for (s32 i = 0; i < query.pairs; ++i, slot += 2) {
            slot[0] = ValidMask;
            slot[1] = ValidMask | (i == 0 ? samples : 0);
        }
        if (Trace()) {
            std::fprintf(stderr, "occlusion: %p pairs=%d slots=%zu samples=%llu\n",
                         static_cast<void*>(query.begin), query.pairs, query.slots.size(),
                         static_cast<unsigned long long>(samples));
        }
    });
}

} // namespace Vulkan
