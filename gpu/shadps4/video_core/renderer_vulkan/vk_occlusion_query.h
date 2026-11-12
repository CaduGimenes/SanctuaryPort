// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

/// sanctuary: real occlusion counters for ZPASS_DONE + PIXEL_PIPE_STAT_DUMP.
///
/// The guest brackets draws with two dumps into one result block: the begin dump writes the
/// first u64 of each of `pairs` {begin, end} pairs and the end dump (address + 8) the second;
/// the game sums end - begin over the pairs once bit 63 (valid) is set in every slot. Diablo
/// III uses it for its hero: an occlusion draw with depth test decides whether the x-ray
/// silhouette shows (hero behind a wall or a tree).
///
/// Each guest query maps to a Vulkan occlusion query begun outside render passes. A
/// submission in between ends it and the next command buffer continues on a new slot, so a
/// guest query is the sum of its slots. The results are written to guest memory once the
/// GPU has finished them, on the scheduler's priority thread.
class OcclusionQueries {
public:
    explicit OcclusionQueries(const Instance& instance, Scheduler& scheduler);
    ~OcclusionQueries();

    /// Draw recording thread, in order with the draws. Returns false when no query could
    /// start (pool full): the caller then writes a fixed "visible" value as before.
    bool Dump(u64* results, s32 pairs);

    /// Scheduler, around a submission: ends the active slot in the closing command buffer and
    /// begins a new one in the next.
    void Suspend(vk::CommandBuffer cmdbuf);
    void Resume(vk::CommandBuffer cmdbuf);

private:
    static constexpr u32 PoolSize = 1024;

    struct Active {
        u64* begin{};
        s32 pairs{};
        u64 generation{};
        std::vector<u32> slots;
        bool suspended{};
    };

    bool AllocateSlot(u32& slot);
    /// `write`: an ended query (results to guest memory), else an abandoned one (slots only).
    void Finish(Active&& query, bool write);

    vk::Device device;
    Scheduler& scheduler;
    vk::UniqueQueryPool pool;
    vk::QueryControlFlags control{};

    std::mutex mutex;
    std::vector<u32> free_slots;
    bool has_active{};
    Active active;
    /// Generation of the latest begin per result block: a late result for a block the game
    /// already restarted is dropped instead of overwriting the new query's zeroed slots.
    std::unordered_map<u64*, u64> generations;
    u64 next_generation{1};
};

} // namespace Vulkan
