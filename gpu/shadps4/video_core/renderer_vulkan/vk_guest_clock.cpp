// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "core/memory.h"
#include "sanctuary_write_log.h"
#include "video_core/renderer_vulkan/vk_guest_clock.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {
/// The recording-time clock of pm4_cmds.h (GetGpuClock64): nanoseconds / 10.
u64 RecordClock() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::high_resolution_clock::now().time_since_epoch())
                        .count();
    return static_cast<u64>(ns) / 10;
}
} // namespace

bool GuestGpuClock::Disabled() {
    static const bool disabled = [] {
        const char* env = std::getenv("SANCTUARY_GPU_CLOCK");
        return env && std::strcmp(env, "record") == 0;
    }();
    return disabled;
}

GuestGpuClock::GuestGpuClock(const Instance& instance, Scheduler& scheduler_)
    : device{instance.GetDevice()}, scheduler{scheduler_} {
    const vk::QueryPoolCreateInfo info = {
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = PoolSize,
    };
    pool = Check<"create guest clock query pool">(device.createQueryPoolUnique(info));
    const auto physical = instance.GetPhysicalDevice();
    ticks_per_timestamp = double(physical.getProperties().limits.timestampPeriod) / 10.0;
    // The graphics queue's valid bits (0 would mean no timestamps; then Stamp is never used).
    const auto families = physical.getQueueFamilyProperties();
    const u32 family = instance.GetGraphicsQueueFamilyIndex();
    if (family < families.size()) {
        const u32 bits = families[family].timestampValidBits;
        valid_mask = bits >= 64 ? ~0ULL : bits == 0 ? 0 : (1ULL << bits) - 1;
    }
    free_slots.reserve(PoolSize);
    for (u32 i = PoolSize; i-- > 0;) {
        free_slots.push_back(i);
    }
}

bool GuestGpuClock::Stamp(u64* address) {
    if (valid_mask == 0) {
        return false;
    }
    u32 slot;
    {
        std::scoped_lock lk{mutex};
        if (free_slots.empty()) {
            return false;
        }
        slot = free_slots.back();
        free_slots.pop_back();
    }
    // Outside render passes (radv_CmdWriteTimestamp2 crashed inside some, see vk_gpu_profiler).
    scheduler.EndRendering();
    scheduler.Record([pool = *pool, slot](vk::CommandBuffer cmdbuf) {
        cmdbuf.resetQueryPool(pool, slot, 1);
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, slot);
    });
    scheduler.DeferPriorityOperation([this, slot, address] {
        u64 stamp = 0;
        const vk::Result result = device.getQueryPoolResults(
            *pool, slot, 1, sizeof(stamp), &stamp, sizeof(stamp),
            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
        u64 value;
        {
            std::scoped_lock lk{mutex};
            free_slots.push_back(slot);
            const s64 ticks = s64(double(stamp & valid_mask) * ticks_per_timestamp);
            if (!have_offset) {
                // The GPU has just passed this point: start the GPU clock at the recording one.
                offset = s64(RecordClock()) - ticks;
                have_offset = true;
            }
            value = result == vk::Result::eSuccess ? u64(ticks + offset) : RecordClock();
        }
        SanctuaryWriteLog::Note(reinterpret_cast<u64>(address), &value, sizeof(value),
                                SanctuaryWriteLog::Fence);
        if (!Core::Memory::Instance()->TryWriteBacking(address, &value, sizeof(value))) {
            std::memcpy(address, &value, sizeof(value));
        }
    });
    return true;
}

} // namespace Vulkan
