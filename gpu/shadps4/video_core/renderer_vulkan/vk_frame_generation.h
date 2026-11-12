// SPDX-License-Identifier: GPL-2.0-or-later
// sanctuary: FSR 3 frame generation for Diablo III (experimental, frame_generation=1).
//
// Per real frame, on the GPU thread: at the display copy the frame's depth and motion vectors
// (TemporalUpscaler::FrameGenInputs) are copied, and before the HUD's first draw the display
// buffer (HUD-less frame), into a slot of that display buffer. At its flip (another thread,
// which can run after the next frame started) the presenter turns the HUD-less copy into a
// window-size image, and FSR 3's frame interpolation makes the frame between the previous
// real one and this one. The presenter shows the generated frame, then the real one half a
// frame later.

#pragma once

#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/texture_cache/image.h"

struct FfxVkPortableFrameGenerationContext;

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
struct Frame;

class FrameGeneration {
public:
    FrameGeneration(const Instance& instance, Scheduler& scheduler,
                    VideoCore::TextureCache& texture_cache, Runtime& runtime);
    ~FrameGeneration();

    /// Set at start (frame_generation=1 with temporal_aa=fsr3) and not failed.
    [[nodiscard]] bool Enabled() const noexcept {
        return enabled && !failed;
    }

    /// GPU thread, at the display copy into `address`: keeps this frame's depth and motion
    /// vectors (invalid inputs: no FSR this frame, nothing is generated for it).
    void BeginFrame(VAddr address, const TemporalUpscaler::FrameGenInputs& inputs);
    /// GPU thread: the display buffer before the HUD's first draw (in `layout`).
    void CaptureHudless(VAddr address, vk::Image image, vk::ImageLayout layout, vk::Format format,
                        u32 width, u32 height);

    /// Presenter, at the flip of `address`: the HUD-less copy of that frame (format as
    /// captured, shader read layout) and its size; false when there is none.
    bool HudlessSource(VAddr address, vk::Format view_format, vk::ImageView& view,
                       vk::Extent2D& size);
    /// A window-size image the presenter draws the HUD-less frame into (frame format).
    Frame* HudlessFrame(u32 width, u32 height, vk::Format format);
    /// Records the frame between the previous real frame and `real` (the flip of `address`)
    /// and returns it, or null (e.g. the first frame of a history). `real` and the HUD-less
    /// frame are in layout General.
    Frame* Generate(vk::CommandBuffer cmdbuf, const Frame& real, VAddr address);
    /// Presenter: no generated frame for this flip (its data is dropped).
    void Skip(VAddr address);

    /// Presenter thread: the generated frame was submitted for presentation.
    void OnPresented(Frame* frame);

private:
    struct Slot {
        VAddr address = 0;
        u64 stamp = 0; ///< BeginFrame order (least recently used slot is reused)
        TemporalUpscaler::FrameGenInputs inputs; ///< .depth/.motion unused: the copies below
        bool has_inputs = false, has_hudless = false;
        VideoCore::UniqueImage depth, motion, hudless;
        vk::UniqueImageView hudless_view, depth_view;
        vk::Format depth_format{}, hudless_format{}, hudless_view_format{};
        u32 depth_w = 0, depth_h = 0, hudless_w = 0, hudless_h = 0;
    };
    Slot* FindSlot(VAddr address);
    bool EnsureContext(u32 render_w, u32 render_h, u32 display_w, u32 display_h,
                       vk::Format source_format);
    void DestroyContext();
    void MakeFrame(Frame& frame, u32 width, u32 height, vk::Format format,
                   vk::ImageUsageFlags usage);
    void FreeFrame(Frame& frame);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    bool enabled = false;
    bool failed = false;

    std::mutex slot_mutex;
    std::array<Slot, 4> slots;
    u64 slot_stamp = 0;

    std::unique_ptr<Frame> hudless_frame;
    static constexpr size_t GeneratedFrames = 3;
    std::array<std::unique_ptr<Frame>, GeneratedFrames> generated;
    std::array<bool, GeneratedFrames> pending{}; ///< handed to the presenter, not presented yet
    size_t next_generated = 0;
    std::mutex pending_mutex;
    std::condition_variable pending_cv;

    FfxVkPortableFrameGenerationContext* context = nullptr;
    std::array<u32, 4> context_size{}; ///< render w/h, display w/h
    vk::Format context_format{};
    bool history = false; ///< the context has seen the previous real frame
    u64 frame_id = 0;
    std::array<u32, 8> skipped{}; ///< SANCTUARY_FRAME_STATS: why frames were not generated
    // depth_copy.comp: the frame's depth into the slot's R32F image.
    vk::UniqueDescriptorSetLayout copy_desc_layout;
    vk::UniquePipelineLayout copy_pipeline_layout;
    vk::UniquePipeline copy_pipeline;
};

} // namespace Vulkan
