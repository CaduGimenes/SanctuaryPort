// SPDX-License-Identifier: GPL-2.0-or-later
// sanctuary: FXAA of Diablo III's finished frame (host_shaders/fxaa.comp), sanctuary.ini `fxaa=1`.
// Runs where FSR 3 would (the frame before its display copy, without the HUD) and only while
// FSR 3 is off: FSR 3 already smooths edges.
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Runtime;
class Scheduler;

class Fxaa {
public:
    Fxaa(const Instance& instance, Scheduler& scheduler, VideoCore::TextureCache& texture_cache,
         Runtime& runtime);
    ~Fxaa();

    /// Smooths `frame` in place when FXAA is on and FSR 3 off; a no-op otherwise.
    void Run(VideoCore::ImageId frame);

private:
    void CreatePipelines();

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;

    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline smooth_pipeline;
    vk::UniquePipeline store_pipeline;

    VideoCore::UniqueImage smoothed;
    vk::UniqueImageView smoothed_view;
    u32 width = 0, height = 0;

    vk::Image frame_image{};
    u64 frame_uid = 0;
    vk::UniqueImageView frame_view;
    bool logged = false;
};

} // namespace Vulkan
