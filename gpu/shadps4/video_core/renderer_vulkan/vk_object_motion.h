// SPDX-License-Identifier: GPL-2.0-or-later
// sanctuary: object motion vectors for temporal upscaling (docs/motion_vectors.md).
//
// G-buffer shaders preserve clip positions for the exact indexed range of each draw.
// Matching includes vertex streams, index contents and base offsets. A missing match falls
// back to camera motion. See motion_history.h for CPU bookkeeping and its regression test.

#pragma once

#include <array>
#include "video_core/renderer_vulkan/motion_history.h"

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;
struct RenderState;

class ObjectMotion {
public:
    ObjectMotion(const Instance& instance, Scheduler& scheduler);
    ~ObjectMotion();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    using DrawInfo = Motion::Draw;
    u32 PrepareDraw(const DrawInfo& draw);
    /// Small skeletons: whether the bone palette changed (Motion::History::Moving).
    bool Moving(const Motion::History::GateKey& key, u64 palette) {
        return history.Moving(key, palette);
    }
    /// Index range and topology hash of an indexed draw, reused across frames.
    template <class Scan>
    Motion::IndexRangeCache::Result IndexRange(const Motion::IndexRangeCache::Key& key,
                                               Scan&& scan) {
        return index_ranges.Get(key, frame, scan);
    }

    /// Rendering of a motion pipeline: the motion attachment (render-target size and sample
    /// count). Other passes (`scene` false, or another size) get a scratch target, so the
    /// frame's image is kept.
    void Attach(RenderState& state, u32 width, u32 height, u32 samples, bool scene);

    /// Start of a frame (display pass).
    void OnFrameStart();
    void InvalidateHistory() { history.NextFrame(); history.NextFrame(); }

    /// The motion image for the compose pass (layout General after this call) and whether it
    /// holds this frame's vectors at `width` x `height`.
    vk::ImageView PrepareRead(vk::CommandBuffer cmdbuf, u32 width, u32 height, bool& valid);
    /// The image of the last PrepareRead (layout General), or null.
    [[nodiscard]] vk::ImageView View() const noexcept {
        return Resolved() ? *resolved_view : view ? *view : vk::ImageView{};
    }
    [[nodiscard]] vk::Image Image(u32 width, u32 height) const noexcept {
        return written && width == image_width && height == image_height
            ? (Resolved() ? vk::Image(resolved) : vk::Image(image)) : vk::Image{};
    }

private:
    struct Target {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u32 width = 0, height = 0, samples = 0;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };
    [[nodiscard]] bool Resolved() const noexcept {
        return image_samples > 1 && resolved_view;
    }
    void EnsureImage(u32 width, u32 height, u32 samples);
    void Create(Target& target, u32 width, u32 height, u32 samples);

    const Instance& instance;
    Scheduler& scheduler;
    bool enabled = false;

    // Parameter ring (host visible): FrameSlots frames of ParamsPerFrame entries (two u32x4),
    // element 0 all zero (motion off).
    static constexpr u32 FrameSlots = 4;
    static constexpr u32 ParamsPerFrame = 8192;
    // Positions (device local): two halves (current/previous frame) of vec4; element 0 is reserved.
    static constexpr u32 PositionsPerFrame = 4u << 20;
    Motion::History history{PositionsPerFrame};
    Motion::IndexRangeCache index_ranges;
    std::array<u64, FrameSlots> params_ticks{};
    vk::Buffer params_buffer{};
    VmaAllocation params_allocation{};
    u32* params_mapped{};
    vk::Buffer positions_buffer{};
    VmaAllocation positions_allocation{};

    u64 frame = 0;
    u32 params_used = 0;
    VideoCore::UniqueImage image;
    vk::UniqueImageView view;
    u32 image_width = 0, image_height = 0, image_samples = 1;
    bool written = false; ///< this frame's vectors are in the image
    vk::ImageLayout image_layout = vk::ImageLayout::eUndefined;
    // Multisampled scenes (Diablo III): the single-sample resolve the compose pass reads.
    VideoCore::UniqueImage resolved;
    vk::UniqueImageView resolved_view;
    vk::ImageLayout resolved_layout = vk::ImageLayout::eUndefined;
    Target scratch; ///< passes of another size (never read)
};

} // namespace Vulkan
