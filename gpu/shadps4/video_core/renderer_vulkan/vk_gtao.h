// SPDX-License-Identifier: GPL-2.0-or-later
// Optional Diablo III PS4 ambient occlusion replacement. The game draws its own
// SSAO first; GTAO then writes the same full-size R8 target, before scene
// lighting samples it.
#pragma once

#include <array>

#include "common/types.h"
#include "video_core/renderer_vulkan/gtao_cache_policy.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {
class Instance;
class Runtime;
class Scheduler;

class Gtao {
  public:
    Gtao(const Instance& instance, Scheduler& scheduler, VideoCore::TextureCache& texture_cache,
         Runtime& runtime);
    ~Gtao();

    // Returns false without changing the stock target when the captured pass
    // layout differs.
    bool Run(VideoCore::ImageId depth, VideoCore::ImageId target,
             const std::array<float, 4>& projection, vk::ImageView vignette,
             vk::ImageLayout vignette_layout, vk::Extent2D scene_extent);

    void InvalidateHistory() {
        history_valid = false;
        cache_policy.Reset();
        history_quality = 0;
    }

  private:
    void CreatePipelines();
    bool EnsureDepthHistory();
    void EnsureTargets(u32 width, u32 height);
    vk::ImageView GuestView(const VideoCore::Image& image, vk::ImageAspectFlags aspect,
                            vk::UniqueImageView& view, vk::Image& cached_image, u64& cached_uid);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;

    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline prefilter_pipeline;
    vk::UniquePipeline prefilter_msaa_pipeline;
    vk::UniquePipeline low_evaluate_pipeline;
    vk::UniquePipeline high_evaluate_pipeline;
    vk::UniquePipeline cached_low_pipeline;
    vk::UniquePipeline cached_high_pipeline;
    std::array<vk::UniquePipeline, 2> classify_depth;
    vk::UniquePipeline dilate_tiles;
    vk::UniquePipeline cached_denoise_pipeline;
    std::array<VideoCore::UniqueImage, 3> history_images;
    std::array<vk::UniqueImageView, 3> history_views;
    std::array<float, 4> history_projection{};
    vk::SampleCountFlagBits history_samples{};
    int history_quality = 0;
    bool history_initialized = false;
    bool history_valid = false;
    bool history_unavailable = false;
    GtaoCachePolicy cache_policy;
    vk::UniquePipeline denoise_pipeline;
    vk::UniquePipeline store_pipeline;
    vk::UniqueSampler point_sampler;

    VideoCore::UniqueImage distances;
    std::array<vk::UniqueImageView, 5> distance_mips;
    vk::UniqueImageView distance_view;
    std::array<VideoCore::UniqueImage, 2> ao_images;
    std::array<vk::UniqueImageView, 2> ao_views;
    u32 width = 0, height = 0;
    bool targets_initialized = false;
    bool warned = false;
    int announced_quality = 0;

    vk::UniqueImageView depth_view;
    vk::Image depth_image{};
    u64 depth_uid = 0;
    vk::UniqueImageView target_view;
    vk::Image target_image{};
    u64 target_uid = 0;
};
} // namespace Vulkan
