// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_gtao.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "sanctuary_settings.h"
#include "video_core/host_shaders/gtao_denoise_cached_comp.h"
#include "video_core/host_shaders/gtao_denoise_comp.h"
#include "video_core/host_shaders/gtao_dilate_comp.h"
#include "video_core/host_shaders/gtao_dirty_fine_comp.h"
#include "video_core/host_shaders/gtao_dirty_fine_msaa_comp.h"
#include "video_core/host_shaders/gtao_dirty_fine_subgroup_comp.h"
#include "video_core/host_shaders/gtao_dirty_fine_subgroup_msaa_comp.h"
#include "video_core/host_shaders/gtao_evaluate_cached_fine_comp.h"
#include "video_core/host_shaders/gtao_evaluate_high_comp.h"
#include "video_core/host_shaders/gtao_evaluate_low_cached_fine_comp.h"
#include "video_core/host_shaders/gtao_evaluate_low_comp.h"
#include "video_core/host_shaders/gtao_prefilter_comp.h"
#include "video_core/host_shaders/gtao_prefilter_msaa_comp.h"
#include "video_core/host_shaders/gtao_store_comp.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {
namespace {
struct PushConstants {
    std::array<float, 4> projection;
    std::array<float, 4> dims;
    std::array<float, 4> params0; // radius, falloff, max screen radius, power
    std::array<float, 4> params1; // extra cache reach (px), padding, denoise depth tolerance, padding
    std::array<float, 4> params2; // mip offset, thickness, far distance, final occlusion strength
};
static_assert(sizeof(PushConstants) == 80);
constexpr vk::ImageSubresourceRange color_level{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
constexpr u32 HistoryTileSize = 16;

void ComputeBarrier(vk::CommandBuffer cmdbuf) {
    constexpr auto compute = vk::PipelineStageFlagBits2::eComputeShader;
    constexpr auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    const vk::MemoryBarrier2 barrier{.srcStageMask = compute,
                                     .srcAccessMask = rw,
                                     .dstStageMask = compute,
                                     .dstAccessMask = rw};
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}
} // namespace

Gtao::Gtao(const Instance& instance_, Scheduler& scheduler_,
           VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_}, runtime{runtime_} {
}

Gtao::~Gtao() = default;

void Gtao::CreatePipelines() {
    const auto device = instance.GetDevice();
    std::array<vk::DescriptorSetLayoutBinding, 7> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = i < 2 ? vk::DescriptorType::eCombinedImageSampler
                                               : vk::DescriptorType::eStorageImage,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(PushConstants)};
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    point_sampler = Check(device.createSamplerUnique({
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = 4.0f,
    }));
    const auto compile = [&](const auto& code) {
        const auto module = CompileSPV(code, device);
        auto pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *pipeline_layout,
                }));
        device.destroyShaderModule(module);
        return pipeline;
    };
    prefilter_pipeline = compile(GTAO_PREFILTER_COMP);
    prefilter_msaa_pipeline = compile(GTAO_PREFILTER_MSAA_COMP);
    low_evaluate_pipeline = compile(GTAO_EVALUATE_LOW_COMP);
    high_evaluate_pipeline = compile(GTAO_EVALUATE_HIGH_COMP);
    denoise_pipeline = compile(GTAO_DENOISE_COMP);
    store_pipeline = compile(GTAO_STORE_COMP);
    const auto physical = instance.GetPhysicalDevice();
    // The cache is optional. On devices without the required format or limits,
    // the same presets retain their full evaluator and denoiser.
    const auto limits = physical.getProperties().limits;
    if (limits.maxComputeWorkGroupInvocations >= 64 &&
        limits.maxComputeWorkGroupSize[0] >= 8 && limits.maxComputeWorkGroupSize[1] >= 8 &&
        (physical.getFormatProperties(vk::Format::eR32Uint).optimalTilingFeatures &
         vk::FormatFeatureFlagBits::eStorageImage)) {
        const auto properties = physical.getProperties2<vk::PhysicalDeviceProperties2,
                                                       vk::PhysicalDeviceSubgroupProperties>();
        const auto& subgroup = properties.get<vk::PhysicalDeviceSubgroupProperties>();
        const auto required = vk::SubgroupFeatureFlagBits::eBasic |
                              vk::SubgroupFeatureFlagBits::eArithmetic;
        if ((subgroup.supportedStages & vk::ShaderStageFlagBits::eCompute) &&
            (subgroup.supportedOperations & required) == required) {
            classify_depth[0] = compile(GTAO_DIRTY_FINE_SUBGROUP_COMP);
            classify_depth[1] = compile(GTAO_DIRTY_FINE_SUBGROUP_MSAA_COMP);
        } else {
            classify_depth[0] = compile(GTAO_DIRTY_FINE_COMP);
            classify_depth[1] = compile(GTAO_DIRTY_FINE_MSAA_COMP);
        }
        dilate_tiles = compile(GTAO_DILATE_COMP);
        cached_low_pipeline = compile(GTAO_EVALUATE_LOW_CACHED_FINE_COMP);
        cached_high_pipeline = compile(GTAO_EVALUATE_CACHED_FINE_COMP);
        cached_denoise_pipeline = compile(GTAO_DENOISE_CACHED_COMP);
    }
}

bool Gtao::EnsureDepthHistory() {
    if (history_views[0]) {
        return true;
    }
    if (history_unavailable) {
        return false;
    }
    const auto fallback = [&] {
        for (auto& view : history_views) {
            view.reset();
        }
        for (auto& image : history_images) {
            image.Destroy();
        }
        history_unavailable = true;
        std::printf("GTAO: depth history unavailable, using full evaluation\n");
        return false;
    };
    const auto device = instance.GetDevice();
    for (u32 i = 0; i < history_images.size(); ++i) {
        history_images[i] = VideoCore::UniqueImage(device, instance.GetAllocator());
        const auto format = i == 0 ? vk::Format::eR32Uint : vk::Format::eR32Sfloat;
        const auto extent = i == 0
                                ? vk::Extent3D{width, height, 1}
                                : vk::Extent3D{(width + HistoryTileSize - 1) / HistoryTileSize,
                                               (height + HistoryTileSize - 1) / HistoryTileSize, 1};
        const auto result = history_images[i].TryCreate(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D, .format = format,
            .extent = extent, .mipLevels = 1, .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
            .usage = i == 0 ? vk::ImageUsageFlags{vk::ImageUsageFlagBits::eStorage}
                            : vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        if (result != vk::Result::eSuccess) {
            return fallback();
        }
        auto view = device.createImageViewUnique({
            .image = vk::Image(history_images[i]), .viewType = vk::ImageViewType::e2D,
            .format = format, .subresourceRange = color_level,
        });
        if (view.result != vk::Result::eSuccess) {
            return fallback();
        }
        history_views[i] = std::move(view.value);
    }
    return true;
}

void Gtao::EnsureTargets(u32 w, u32 h) {
    if (w == width && h == height && distance_view) {
        return;
    }
    const auto device = instance.GetDevice();
    scheduler.Finish(); // old images may still be referenced by recorded compute commands
    for (auto& view : distance_mips) {
        view.reset();
    }
    distance_view.reset();
    for (auto& view : history_views) {
        view.reset();
    }
    for (auto& image : history_images) {
        image.Destroy();
    }
    history_initialized = history_unavailable = false;
    InvalidateHistory();
    for (auto& view : ao_views) {
        view.reset();
    }
    for (auto& image : ao_images) {
        image.Destroy();
    }
    distances = VideoCore::UniqueImage(device, instance.GetAllocator());
    distances.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR32Sfloat,
        .extent = {w, h, 1},
        .mipLevels = 5,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    distance_view = Check(device.createImageViewUnique({
        .image = vk::Image(distances),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR32Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 5, 0, 1},
    }));
    for (u32 mip = 0; mip < 5; ++mip) {
        distance_mips[mip] = Check(device.createImageViewUnique({
            .image = vk::Image(distances),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR32Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, mip, 1, 0, 1},
        }));
    }
    for (u32 i = 0; i < ao_images.size(); ++i) {
        ao_images[i] = VideoCore::UniqueImage(device, instance.GetAllocator());
        ao_images[i].Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16Sfloat,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        ao_views[i] = Check(device.createImageViewUnique({
            .image = vk::Image(ao_images[i]),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16Sfloat,
            .subresourceRange = color_level,
        }));
    }
    width = w;
    height = h;
    targets_initialized = false;
    std::printf("GTAO: targets %ux%u (R32F depth pyramid, R16F visibility x2)\n", w, h);
}

vk::ImageView Gtao::GuestView(const VideoCore::Image& image, vk::ImageAspectFlags aspect,
                              vk::UniqueImageView& view, vk::Image& cached_image, u64& cached_uid) {
    const auto handle = image.GetImage();
    const auto device = instance.GetDevice();
    if (!view || cached_image != handle || cached_uid != image.image_uid) {
        if (view) {
            scheduler.DeferOperation(
                [device, old = view.release()] { device.destroyImageView(old); });
        }
        cached_image = handle;
        cached_uid = image.image_uid;
        view = Check(device.createImageViewUnique({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = image.info.pixel_format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        }));
    }
    return *view;
}

bool Gtao::Run(VideoCore::ImageId depth_id, VideoCore::ImageId target_id,
               const std::array<float, 4>& proj, vk::ImageView vignette,
               vk::ImageLayout vignette_layout, vk::Extent2D scene_extent) {
    const int quality = SanctuarySettings::Get().gtao.load(std::memory_order_relaxed);
    const bool high = quality == SanctuarySettings::GtaoHigh;
    if ((!high && quality != SanctuarySettings::GtaoLow) || !depth_id || !target_id || !vignette) {
        InvalidateHistory();
        return false;
    }
    auto& depth = texture_cache.GetImage(depth_id);
    auto& target = texture_cache.GetImage(target_id);
    const auto& depth_ci = depth.backing->image.image_ci;
    const auto& target_ci = target.backing->image.image_ci;
    const auto depth_samples = depth_ci.samples;
    const u32 w = target.info.size.width, h = target.info.size.height;
    const float near = -proj[3] / proj[2];
    const float far = proj[3] / (1.0f - proj[2]);
    const auto supports_image = [&](vk::Format format, vk::FormatFeatureFlags flags) {
        const auto available =
            instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
        return (available & flags) == flags;
    };
    const auto sampled_storage =
        vk::FormatFeatureFlagBits::eStorageImage | vk::FormatFeatureFlagBits::eSampledImage;
    const bool supported =
        instance.GetPhysicalDevice().getProperties().limits.maxPerStageDescriptorStorageImages >=
            5 &&
        supports_image(vk::Format::eR32Sfloat, sampled_storage) &&
        supports_image(vk::Format::eR16Sfloat, sampled_storage) &&
        supports_image(vk::Format::eR8Unorm, vk::FormatFeatureFlagBits::eStorageImage);
    if (!supported) {
        InvalidateHistory();
        if (!warned) {
            warned = true;
            std::printf("GTAO: stock SSAO retained; compute storage formats or descriptors "
                        "unsupported\n");
        }
        return false;
    }
    const bool valid =
        (depth.info.pixel_format == vk::Format::eD32Sfloat ||
         depth.info.pixel_format == vk::Format::eD32SfloatS8Uint) &&
        target.info.pixel_format == vk::Format::eR8Unorm &&
        depth_ci.format == depth.info.pixel_format &&
        target_ci.format == target.info.pixel_format &&
        (target.usage_flags & vk::ImageUsageFlagBits::eStorage) &&
        (target_ci.usage & vk::ImageUsageFlagBits::eStorage) &&
        (depth_samples == vk::SampleCountFlagBits::e1 ||
         depth_samples == vk::SampleCountFlagBits::e2 ||
         depth_samples == vk::SampleCountFlagBits::e4) &&
        target_ci.samples == vk::SampleCountFlagBits::e1 && depth_ci.mipLevels == 1 &&
        depth_ci.arrayLayers == 1 && target_ci.mipLevels == 1 && target_ci.arrayLayers == 1 &&
        depth_ci.extent.width == depth.info.size.width &&
        depth_ci.extent.height == depth.info.size.height && target_ci.extent.width == w &&
        target_ci.extent.height == h && w == depth.info.size.width && h == depth.info.size.height &&
        scene_extent.width >= 16 && scene_extent.height >= 16 &&
        scene_extent.width <= w && scene_extent.height <= h &&
        std::isfinite(proj[0]) && std::isfinite(proj[1]) &&
        std::isfinite(proj[2]) && std::isfinite(proj[3]) && std::abs(proj[0]) > 1e-6f &&
        std::abs(proj[1]) > 1e-6f && std::abs(proj[2] - 1.0f) > 1e-7f && std::isfinite(near) &&
        std::isfinite(far) && near > 0.0f && far > near && far < 1e7f;
    if (!valid) {
        InvalidateHistory();
        if (!warned) {
            warned = true;
            std::printf("GTAO: stock SSAO retained; depth %s %ux%u, AO %s %ux%u, "
                        "projection %.4f %.4f %.4f %.4f\n",
                        vk::to_string(depth.info.pixel_format).c_str(), depth.info.size.width,
                        depth.info.size.height, vk::to_string(target.info.pixel_format).c_str(), w,
                        h, proj[0], proj[1], proj[2], proj[3]);
        }
        return false;
    }
    if (!prefilter_pipeline) {
        CreatePipelines();
    }
    EnsureTargets(w, h);
    if (auto* profiler = GpuProfiler::Get()) {
        const u64 key = high ? 0x67A0'0002ull : 0x67A0'0001ull;
        profiler->Mark(key, [high] { return std::string{high ? "GTAO (High)" : "GTAO (Low)"}; });
    }
    const auto depth_v =
        GuestView(depth, vk::ImageAspectFlagBits::eDepth, depth_view, depth_image, depth_uid);
    const auto target_v =
        GuestView(target, vk::ImageAspectFlagBits::eColor, target_view, target_image, target_uid);
    const PushConstants push{
        .projection = proj,
        .dims = {float(scene_extent.width), float(scene_extent.height),
                 1.0f / scene_extent.width, 1.0f / scene_extent.height},
        .params0 = {3.0f, 0.5f, 0.10f, 2.2f},
        // Sizes that are not multiples of 16 (1920x1080) give mip levels whose texels can come from
        // the neighboring 16x16 tile: the cache then reaches one tile further.
        .params1 = {w % HistoryTileSize || h % HistoryTileSize ? float(HistoryTileSize) : 0.0f,
                    0.0f, 0.03f, 0.0f},
        .params2 = {4.5f, 1.0f, far, high ? 0.85f : 0.90f},
    };
    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&target, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderWrite);
    runtime.FlushBarriers();
    const vk::Image dist_image = vk::Image(distances);
    const std::array<vk::Image, 2> visibility_images{vk::Image(ao_images[0]),
                                                    vk::Image(ao_images[1])};
    std::array<vk::ImageView, 5> mips{};
    for (u32 i = 0; i < 5; ++i)
        mips[i] = *distance_mips[i];
    const vk::ImageView dist_view = *distance_view;
    const std::array<vk::ImageView, 2> ao_v{*ao_views[0], *ao_views[1]};
    const vk::Sampler sampler = *point_sampler;
    const vk::PipelineLayout layout = *pipeline_layout;
    const vk::Pipeline prefilter = depth_samples == vk::SampleCountFlagBits::e1
                                       ? *prefilter_pipeline
                                       : *prefilter_msaa_pipeline;
    const vk::Pipeline evaluate = high ? *high_evaluate_pipeline : *low_evaluate_pipeline;
    const auto& cached_evaluate = high ? cached_high_pipeline : cached_low_pipeline;
    // Diablo III's dynamic resolution draws the scene into the top-left part of the targets:
    // the history is valid for one scene size (the evaluator's coordinates depend on it).
    const bool eligible = cached_evaluate && !history_unavailable;
    const bool same_configuration = history_quality == quality && history_projection == proj &&
                                    history_samples == depth_samples &&
                                    history_extent == scene_extent;
    // Exact matching remains mandatory in the classifier. Warm-up avoids spending
    // time classifying depth while the captured projection is changing.
    const bool cache = cache_policy.Update(eligible && same_configuration) && EnsureDepthHistory();
    const bool reuse = cache && history_valid;
    history_valid = cache;
    history_quality = quality;
    history_projection = proj;
    history_samples = depth_samples;
    history_extent = scene_extent;
    std::array<vk::Image, 3> history{};
    std::array<vk::ImageView, 3> views{};
    if (cache) {
        for (u32 i = 0; i < history.size(); ++i) {
            history[i] = vk::Image(history_images[i]);
            views[i] = *history_views[i];
        }
    }
    const bool init_history = cache && !std::exchange(history_initialized, true);
    const bool init_targets = !std::exchange(targets_initialized, true);
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        if (!init_targets) {
            ComputeBarrier(cmdbuf); // previous frame's pyramid/visibility writes
        }
        if (!init_targets && !init_history) {
            return;
        }
        std::array<vk::ImageMemoryBarrier2, 6> barriers{};
        u32 count = 0;
        const auto add = [&](vk::Image image, vk::ImageSubresourceRange range) {
            barriers[count++] = {
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = image,
                .subresourceRange = range,
            };
        };
        if (init_history) {
            for (auto img : history) {
                add(img, color_level);
            }
        }
        if (init_targets) {
            add(dist_image, {vk::ImageAspectFlagBits::eColor, 0, 5, 0, 1});
            for (const auto image : visibility_images) {
                add(image, color_level);
            }
        }
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = count,
                                 .pImageMemoryBarriers = barriers.data()});
    });
    const auto pass = [&](vk::Pipeline pipe, vk::ImageView in0, vk::ImageView in1,
                          const std::array<vk::ImageView, 5>& out, u32 gx, u32 gy,
                          bool wait_for_compute = true,
                          vk::ImageLayout second_layout = vk::ImageLayout::eGeneral) {
        auto params = push;
        if (cache && pipe == *classify_depth[depth_samples != vk::SampleCountFlagBits::e1]) {
            params.params2[3] = reuse ? 1.f : 0.f;
        }
        scheduler.Record([=](vk::CommandBuffer cmdbuf) {
            if (wait_for_compute) {
                ComputeBarrier(cmdbuf);
            }
            std::array<vk::DescriptorImageInfo, 7> info{};
            info[0] = {
                .sampler = sampler, .imageView = in0, .imageLayout = vk::ImageLayout::eGeneral};
            info[1] = {.sampler = sampler, .imageView = in1, .imageLayout = second_layout};
            for (u32 i = 0; i < 5; ++i) {
                info[i + 2] = {.imageView = out[i], .imageLayout = vk::ImageLayout::eGeneral};
            }
            std::array<vk::WriteDescriptorSet, 7> writes{};
            for (u32 i = 0; i < 7; ++i) {
                writes[i] = {.dstBinding = i,
                             .descriptorCount = 1,
                             .descriptorType = i < 2 ? vk::DescriptorType::eCombinedImageSampler
                                                     : vk::DescriptorType::eStorageImage,
                             .pImageInfo = &info[i]};
            }
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipe);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
            cmdbuf.dispatch(gx, gy, 1);
        });
    };
    const u32 groups_x = (scene_extent.width + 7) / 8;
    const u32 groups_y = (scene_extent.height + 7) / 8;
    const u32 tiles_x = (scene_extent.width + HistoryTileSize - 1) / HistoryTileSize;
    const u32 tiles_y = (scene_extent.height + HistoryTileSize - 1) / HistoryTileSize;
    if (cache) {
        auto out = mips;
        out[0] = views[0];
        out[1] = views[1];
        pass(*classify_depth[depth_samples != vk::SampleCountFlagBits::e1], depth_v, depth_v,
             out, tiles_x, tiles_y, false);
    }
    pass(prefilter, depth_v, depth_v, mips, tiles_x, tiles_y, false);
    auto outputs = mips;
    if (reuse) {
        outputs[0] = views[2];
        pass(*dilate_tiles, views[1], views[1], outputs, (tiles_x + 7) / 8, (tiles_y + 7) / 8);
    }
    outputs[0] = ao_v[0];
    const vk::ImageView mask = views[2];
    pass(reuse ? *cached_evaluate : evaluate, dist_view, reuse ? mask : dist_view,
         outputs, groups_x, groups_y);
    outputs[0] = ao_v[1];
    if (reuse) {
        outputs[1] = mask;
        pass(*cached_denoise_pipeline, ao_v[0], dist_view, outputs, groups_x, groups_y);
        outputs[1] = mips[1];
    } else {
        pass(*denoise_pipeline, ao_v[0], dist_view, outputs, groups_x, groups_y);
    }
    outputs[0] = target_v;
    pass(*store_pipeline, ao_v[1], vignette, outputs, groups_x, groups_y, true, vignette_layout);
    if (announced_quality != quality) {
        announced_quality = quality;
        std::printf("GTAO: active on Diablo III PS4 SSAO upscale output (%s)\n",
                    high ? "High" : "Low");
    }
    return true;
}
} // namespace Vulkan
