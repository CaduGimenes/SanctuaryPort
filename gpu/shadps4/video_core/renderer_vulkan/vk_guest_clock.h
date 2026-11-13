// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <mutex>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

/// sanctuary: GPU timestamps for end-of-pipe events that write the GPU clock (DataSelect::
/// GpuClock64).
///
/// Diablo III's dynamic resolution brackets its frame with six such timestamps and sizes the
/// scene from the GPU time between them. Written when the commands were recorded, they measured
/// the recording thread instead (fast, then stalled on the GPU at random points), so the scene
/// size jumped between 70% and 95% at 4K on a GPU that could not keep up with either. Each
/// timestamp is now a Vulkan timestamp written by the GPU where the event sits, copied to guest
/// memory (100 MHz ticks, like the PS4's counter) once the GPU has passed it, on the scheduler's
/// priority thread. SANCTUARY_GPU_CLOCK=record keeps the recording-time value.
class GuestGpuClock {
public:
    explicit GuestGpuClock(const Instance& instance, Scheduler& scheduler);

    /// Whether SANCTUARY_GPU_CLOCK asks for the old recording-time timestamps.
    static bool Disabled();

    /// Draw recording thread, in order with the draws. Returns false when no query slot is free:
    /// the caller then writes the recording-time value as before.
    bool Stamp(u64* address);

private:
    static constexpr u32 PoolSize = 256;

    vk::Device device;
    Scheduler& scheduler;
    vk::UniqueQueryPool pool;
    double ticks_per_timestamp = 0.1; ///< 100 MHz ticks per Vulkan timestamp unit
    u64 valid_mask = ~0ULL;
    std::mutex mutex;
    std::vector<u32> free_slots;
    bool have_offset = false;
    s64 offset = 0; ///< added to converted timestamps: starts near the recording-time clock
};

} // namespace Vulkan
