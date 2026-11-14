// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/ui_composition.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <vk_mem_alloc.h>

#include "sanctuary_settings.h"
#include "sanctuary_toggles.h"
#include "ffx_vk_portable.h"
#include "video_core/host_shaders/upscale_merge_comp.h"
#include "video_core/host_shaders/upscale_merge_rgb10a2_comp.h"
#include "video_core/host_shaders/upscale_reactive_comp.h"
#include "video_core/host_shaders/reactive_msaa_comp.h"
#include "video_core/host_shaders/taa_comp.h"
#include "video_core/host_shaders/taa_sharpen_comp.h"
#include "video_core/host_shaders/taa_sharpen_ldr_comp.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

FfxVkPortableImage Describe(vk::Image image, vk::Format format, u32 width, u32 height,
                            vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                            FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image;
    out.format = static_cast<VkFormat>(format);
    out.extent = {width, height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(usage);
    out.aspect = static_cast<VkImageAspectFlags>(aspect);
    out.state = state;
    return out;
}

/// sanctuary: SANCTUARY_DUMP_TRIGGER=<file> SANCTUARY_DUMP_DIR=<dir> (default out/dump): creating the file dumps
/// the upscaler's images of the next SANCTUARY_DUMP_FRAMES (8) frames as raw files
/// <dir>/fNNN_<name>_<w>x<h>_<format>.raw, for checking temporal stability offline.
/// Returns the frame number to dump, or -1.
int DumpFrame() {
    static const char* trigger = std::getenv("SANCTUARY_DUMP_TRIGGER");
    static int remaining = 0, index = 0, polls = 0;
    if (!trigger) {
        return -1;
    }
    if (remaining == 0) {
        if (++polls % 30 != 0 || std::remove(trigger) != 0) {
            return -1;
        }
        const char* frames = std::getenv("SANCTUARY_DUMP_FRAMES");
        remaining = frames ? std::max(1, std::atoi(frames)) : 8;
        index = 0;
    }
    --remaining;
    return index++;
}

struct DumpImage {
    vk::Image image; ///< in layout General
    u32 width, height, bytes_per_pixel;
    const char* name;
    const char* format;
    vk::ImageAspectFlagBits aspect = vk::ImageAspectFlagBits::eColor;
};

/// Copies `images` into host buffers after the commands recorded so far and writes them to
/// files once the GPU is done.
void DumpImages(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                int frame, std::initializer_list<DumpImage> images) {
    static const std::string dir = [] {
        const char* env = std::getenv("SANCTUARY_DUMP_DIR");
        return std::string{env && env[0] ? env : "out/dump"};
    }();
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    for (const auto& image : images) {
        const VkDeviceSize size = VkDeviceSize(image.width) * image.height * image.bytes_per_pixel;
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo alloc_ci{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        };
        VkBuffer buffer{};
        VmaAllocation allocation{};
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                            &info) != VK_SUCCESS) {
            std::printf("Dump: no host memory for %s\n", image.name);
            continue;
        }
        const vk::BufferImageCopy region{
            .imageSubresource = {image.aspect, 0, 0, 1},
            .imageExtent = {image.width, image.height, 1},
        };
        cmdbuf.copyImageToBuffer(image.image, vk::ImageLayout::eGeneral, buffer, region);
        char path[512];
        std::snprintf(path, sizeof(path), "%s/f%03d_%s_%ux%u_%s.raw", dir.c_str(), frame,
                      image.name, image.width, image.height, image.format);
        scheduler.DeferPriorityOperation(
            [allocator = instance.GetAllocator(), buffer, allocation, info, size,
             file = std::string{path}] {
                vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
                if (FILE* f = std::fopen(file.c_str(), "wb")) {
                    std::fwrite(info.pMappedData, 1, size, f);
                    std::fclose(f);
                }
                vmaDestroyBuffer(allocator, buffer, allocation);
            });
    }
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eNone,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    std::printf("Dump: frame %d -> %s\n", frame, dir.c_str());
}

void PrintIssues(const char* what, u64 issues) {
    std::printf("Upscaler: %s invalid:", what);
    for (u32 bit = 0; bit < 64; ++bit) {
        if (issues & (1ull << bit)) {
            std::printf(" %s", ffxVkPortableValidationIssueName(1ull << bit));
        }
    }
    std::printf("\n");
}

} // namespace

TemporalUpscaler::TemporalUpscaler(const Instance& instance_, Scheduler& scheduler_,
                                   VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                                   CameraMotion& camera_motion_, SceneTargets& scene_targets_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, camera_motion{camera_motion_}, scene_targets{scene_targets_} {
    // Reject unsupported shaders before allocating resources or recording a frame.
    SanctuarySettings::ConfigureUpscalerSupport(instance.IsFsr4Int8Supported(),
                                         instance.IsFsr411Supported());
    fsr4 = std::make_unique<Fsr4Upscaler>(instance, scheduler);
    {
        const Dlss* dlss = Dlss::Get();
        static std::string problem;
        problem = !dlss ? "sanctuary_dlss.dll and nvngx_dlss.dll are not installed"
                        : dlss->Problem();
        SanctuarySettings::ConfigureDlssSupport(dlss && dlss->Available(), problem.c_str());
    }
    // Available unless SANCTUARY_UPSCALER=none; on/off and the parameters are the menu's settings.
    const char* env = std::getenv("SANCTUARY_UPSCALER");
    enabled = !(env && std::strcmp(env, "none") == 0);
    if (const char* hash = std::getenv("SANCTUARY_UPSCALE_BEFORE_CS")) {
        trigger_hash = std::strtoull(hash, nullptr, 16);
    }
    if (const char* hash = std::getenv("SANCTUARY_UI_TRIGGER_VS")) {
        ui_trigger_vs = std::strtoull(hash, nullptr, 16);
    }
    if (const char* res = std::getenv("SANCTUARY_OUTPUT_RES")) {
        u32 w = 0, h = 0;
        if (std::sscanf(res, "%ux%u", &w, &h) == 2 && w && h) {
            target_width = w;
            target_height = h;
        }
    }
    // Explicit resolution overrides retain the old compatibility path. Normal presets
    // keep guest allocations/UI native and change only host raster targets at frame boundaries.
    scaled_session = std::getenv("SANCTUARY_RENDER_RES") && std::getenv("SANCTUARY_RENDER_RES")[0];
    render_width = 1920;
    render_height = 1080;
    if (const char* res = std::getenv("SANCTUARY_RENDER_RES")) {
        u32 w = 0, h = 0;
        if (std::sscanf(res, "%ux%u", &w, &h) == 2 && w && h) {
            render_width = w;
            render_height = h;
        }
    }
    // Scene depth copied into the output-size UI depth by a blit (depth aspect).
    const auto features = instance.GetPhysicalDevice()
                              .getFormatProperties(vk::Format::eD32SfloatS8Uint)
                              .optimalTilingFeatures;
    depth_blit = (features & vk::FormatFeatureFlagBits::eBlitSrc) &&
                 (features & vk::FormatFeatureFlagBits::eBlitDst);
    if (enabled && !instance.IsStorageImageWriteWithoutFormatEnabled()) {
        std::printf("Upscaler: shaderStorageImageWriteWithoutFormat unsupported, FSR 3 off\n");
        enabled = false;
    }
    if (enabled) {
        std::printf("Upscaler: FSR 3.1 available (%s) on scene color before compute shader "
                    "%016llx\n",
                    SanctuarySettings::Get().upscaler != SanctuarySettings::UpscalerOff ? "on" : "off",
                    static_cast<unsigned long long>(trigger_hash));
    }
}

TemporalUpscaler::~TemporalUpscaler() {
    if (resources_ready) scheduler.Finish();
    if (context) {
        scheduler.Finish();
        ffxVkPortableUpscaleContextDestroy(context);
    }
}

bool TemporalUpscaler::Active() const {
    // Toggle 1 << 24 switches it off at run time (A/B); history restarts after.
    return enabled && !failed &&
           (SanctuarySettings::Get().upscaler == SanctuarySettings::UpscalerFsr3 ||
            SanctuarySettings::IsFsr4(SanctuarySettings::Get().upscaler) ||
            SanctuarySettings::Get().upscaler == SanctuarySettings::UpscalerTaa ||
            SanctuarySettings::Get().upscaler == SanctuarySettings::UpscalerDlss) &&
           !SanctuaryToggle::Disabled(1u << 24);
}

bool TemporalUpscaler::ReactiveOn() const {
    // FSR 4 takes no reactive mask: the opaque snapshot and the mask pass would be wasted.
    return SanctuarySettings::Get().reactive && !SanctuaryToggle::Disabled(1u << 27) && !UseFsr4() && !UseDlss() &&
           SanctuarySettings::Get().upscaler != SanctuarySettings::UpscalerTaa;
}

void TemporalUpscaler::OnSceneColor(VideoCore::ImageId color) {
    scene_color = color;
}

namespace {
float Halton(u32 index, u32 base) {
    float f = 1.0f, result = 0.0f;
    for (u32 i = index; i > 0; i /= base) {
        f /= float(base);
        result += f * float(i % base);
    }
    return result;
}
} // namespace

bool TemporalUpscaler::ReducedScene(const VideoCore::Image& color) const {
    return !scaled_session && scene_targets.Reduced() && scene_targets.EligibleScene(color);
}

bool TemporalUpscaler::RasterScaling() const {
    return !scaled_session && scene_targets.Reduced() && !done_this_frame;
}

bool TemporalUpscaler::OnFrameStart() {
    // sanctuary: SANCTUARY_PRESET_FILE=<file> holding a preset number, read about once a second: switches
    // the preset like the menu does (scripted tests of live preset changes).
    static const char* preset_file = std::getenv("SANCTUARY_PRESET_FILE");
    if (preset_file && ++preset_file_frames % 64 == 0) {
        if (FILE* f = std::fopen(preset_file, "r")) {
            int value = 0;
            int output = -1;
            int provider = -1;
            const int fields = std::fscanf(f, "%d %d %d", &value, &output, &provider);
            if (fields >= 1 && value >= 0 &&
                value < SanctuarySettings::PresetCount) {
                SanctuarySettings::Get().preset.store(value);
            }
            if (fields >= 2 && output >= 0 && output < SanctuarySettings::OutputCount) {
                SanctuarySettings::Get().output_res.store(output);
            }
            if (fields == 3 && provider >= 0 && provider < SanctuarySettings::UpscalerCount) {
                SanctuarySettings::Get().upscaler.store(provider);
            }
            std::fclose(f);
        }
    }
    const auto& settings = SanctuarySettings::Get();
    // The guest's startup resolution patch remains in effect until restart. Keep the FSR
    // model at the applied preset while the menu saves the requested one for run.sh.
    const int preset = SanctuarySettings::RenderPreset();
    if (applied_preset != preset || settings.upscaler == SanctuarySettings::UpscalerOff) failed = false;
    const bool active = Active();
    const bool jitter_on = active && settings.jitter && !SanctuaryToggle::Disabled(1u << 25);
    const int upscaler = settings.upscaler.load();
    const int output = settings.output_res.load();
    const bool output_changed = !scaled_session && applied_output != output;
    if (output_changed) {
        target_width = SanctuarySettings::OutputWidths[output];
        target_height = SanctuarySettings::OutputHeights[output];
        std::printf("Output resolution: %ux%u (live)\n", target_width, target_height);
        failed = false;
        fsr4_failed = false;
    }
    const bool changed = output_changed || applied_preset != preset || active != last_active ||
                         jitter_on != last_jitter || applied_upscaler != upscaler;
    if (applied_upscaler != upscaler) {
        // A failed provider keeps a fatal flag internally; a user retry gets a fresh context.
        scheduler.Finish();
        fsr4 = std::make_unique<Fsr4Upscaler>(instance, scheduler);
        if (SanctuarySettings::IsFsr4(upscaler)) SanctuarySettings::Get().fsr4_problem = nullptr;
    }
    if (applied_upscaler != upscaler) fsr4_failed = false; // retry after a menu change
    // Dynamic scene resolution scaling (live preset switching) works on all GPUs.
    // On GPUs without D32S8 blit support, UI depth is cleared instead of copied from scene.
    if (!scaled_session) {
        scene_targets.SetSize(SceneResolution::ForPreset(active ? preset : 0,
                                                        {target_width, target_height}));
        render_width = scene_targets.Size().width;
        render_height = scene_targets.Size().height;
    }
    SanctuarySettings::Get().active_render_width = Scaled() ? render_width : scene_targets.Size().width;
    SanctuarySettings::Get().active_render_height = Scaled() ? render_height : scene_targets.Size().height;
    if (changed || !dispatched_last_frame) reset = true;
    if (changed) jitter_index = 0;
    applied_preset = preset;
    applied_output = output;
    applied_upscaler = upscaler;
    last_active = active;
    last_jitter = jitter_on;
    dispatched_last_frame = false;

    // The display pass of an upscaled frame reads the upscaled UI image.
    ++frame_count;
    display_redirect = ui_phase;
    ui_phase = false;
    viewport_redirect = viewport_phase;
    viewport_phase = false;
    viewport_sampled = false;
    ui_read_barrier = false;
    done_this_frame = false;
    ldr_target = {};
    scene_color = {};
    geometry_color = {};
    snapshot_taken = false;
    d3_after_depth_copy = false;
    opaque_valid = false;
    mask_ready = false;
    // Halton(2, 3); the menu or toggle 1 << 25 disables it.
    if (!jitter_on) {
        jitter = {};
        return changed;
    }
    // CameraMotion::OnDisplayPass has already cleared Depth(). Use the context's render
    // size, which survives the frame boundary (Performance 960 -> 1920 needs 32 phases).
    const u32 phases = Scaled() ? Motion::JitterPhases(render_width, target_width)
        : d3_jitter_sizes[0] ? Motion::JitterPhases(d3_jitter_sizes[0], d3_jitter_sizes[1])
        : Motion::JitterPhases(scene_targets.Size().width, 1920);
    jitter_index = jitter_index % phases + 1;
    jitter = {Halton(jitter_index, 2) - 0.5f, Halton(jitter_index, 3) - 0.5f};
    return changed;
}

void TemporalUpscaler::OnGeometryColor(VideoCore::ImageId color) {
    geometry_color = color;
}

void TemporalUpscaler::OnD3AdditiveDraw() {
    if (!d3_after_depth_copy || snapshot_taken || !geometry_color || failed || !Active() ||
        !ReactiveOn() ||
        SanctuarySettings::Get().upscaler != SanctuarySettings::UpscalerFsr3) {
        return;
    }
    snapshot_taken = true;
    opaque_valid = RecordD3Reactive(geometry_color, 0);
}

void TemporalUpscaler::OnD3Target(VideoCore::ImageId color) {
    // The first draw into another target ends the scene: the effects are in. (At the display
    // copy the 2-sample scene reads black: its memory is used again by then.)
    if (opaque_valid && !mask_ready && geometry_color && color != geometry_color) {
        mask_ready = RecordD3Reactive(geometry_color, 1);
        opaque_valid = mask_ready;
    }
}

bool TemporalUpscaler::RecordD3Reactive(VideoCore::ImageId color_id, u32 mode) {
    if (!color_id || !opaque_image || !reactive_image || !reactive_msaa_pipeline) {
        return false;
    }
    auto& color = texture_cache.GetImage(color_id);
    // The images of the last FSR dispatch (EnsureResources): the first frame has none yet.
    if (color.info.size.width != width || color.info.size.height != height ||
        color.info.num_samples < 2 || !(color.usage_flags & vk::ImageUsageFlagBits::eSampled)) {
        return false;
    }
    scheduler.EndRendering();
    const vk::ImageView scene =
        CachedView(color, color.info.pixel_format, vk::ImageAspectFlagBits::eColor);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    const vk::Image opaque = vk::Image(opaque_image), mask = vk::Image(reactive_image);
    const vk::ImageView opaque_v = *opaque_view, mask_v = *reactive_view;
    const vk::Pipeline pipeline = *reactive_msaa_pipeline;
    const vk::PipelineLayout layout = *reactive_msaa_pipeline_layout;
    const u32 w = width, h = height;
    const auto& settings = SanctuarySettings::Get();
    std::array<u32, 4> params{};
    const std::array<float, 3> values{settings.reactive_scale, settings.reactive_max,
                                      settings.reactive_threshold};
    std::memcpy(params.data(), values.data(), sizeof(values));
    params[3] = mode;
    // On the recording thread, in order with the scene's draws.
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const auto barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                                 vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                 vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
            const vk::ImageMemoryBarrier2 b{
                .srcStageMask = src_stage,
                .srcAccessMask = src_access,
                .dstStageMask = dst_stage,
                .dstAccessMask = dst_access,
                .oldLayout = old_layout,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = image,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        };
        const auto all = vk::PipelineStageFlagBits2::eAllCommands;
        const auto compute = vk::PipelineStageFlagBits2::eComputeShader;
        const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
        // Snapshot: the opaque copy is overwritten (last frame's mask pass read it). Mask: it
        // is read, and the mask image (last frame's FSR read it) is overwritten.
        barrier(opaque, mode == 0 ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral, all,
                vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite, compute, rw);
        if (mode == 1) {
            barrier(mask, vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eMemoryRead,
                    compute, vk::AccessFlagBits2::eShaderWrite);
        }
        const vk::DescriptorImageInfo opaque_info{.imageView = opaque_v,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo scene_info{.imageView = scene,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        // Mode 0 writes no mask; binding the mask image anyway keeps the set complete.
        const vk::DescriptorImageInfo mask_info{.imageView = mask_v,
                                                .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 3> writes = {{
            {.dstBinding = 0, .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &opaque_info},
            {.dstBinding = 1, .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &scene_info},
            {.dstBinding = 2, .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &mask_info},
        }};
        if (mode == 0) {
            barrier(mask, vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eMemoryRead,
                    compute, vk::AccessFlagBits2::eShaderWrite);
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
                             params.data());
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        barrier(mode == 0 ? opaque : mask, vk::ImageLayout::eGeneral, compute,
                vk::AccessFlagBits2::eShaderWrite, all, vk::AccessFlagBits2::eShaderRead);
    });
    return true;
}

TemporalUpscaler::ViewportTarget* TemporalUpscaler::EnsureViewportTarget(u32 width, u32 height,
                                                                         vk::Format format) {
    for (auto& target : viewport_targets) {
        if (target->width == width && target->height == height && target->image) {
            target->used = frame_count;
            return target.get();
        }
    }
    constexpr size_t MaxTargets = 8;
    if (viewport_targets.size() >= MaxTargets) {
        // The least recently used one goes once the GPU is past this point.
        auto oldest = std::min_element(viewport_targets.begin(), viewport_targets.end(),
                                       [](const auto& a, const auto& b) { return a->used < b->used; });
        std::shared_ptr<ViewportTarget> dropped{std::move(*oldest)};
        viewport_targets.erase(oldest);
        scheduler.DeferOperation([dropped] {});
    }
    auto target = std::make_unique<ViewportTarget>();
    target->width = width;
    target->height = height;
    target->used = frame_count;
    target->image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    target->image.Create(vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    target->storage = Check(instance.GetDevice().createImageViewUnique({
        .image = vk::Image(target->image),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    static int told = 0;
    if (told < 8) {
        ++told;
        std::printf("Upscaler: FSR 3 viewport target %ux%u\n", width, height);
    }
    viewport_targets.push_back(std::move(target));
    return viewport_targets.back().get();
}

bool TemporalUpscaler::DisplayUpscaleSize(u32 w, u32 h, u32& ow, u32& oh) const {
    const auto& settings = SanctuarySettings::Get();
    if (settings.upscaler != SanctuarySettings::UpscalerFsr3) {
        return false;
    }
    ow = u32(std::clamp(settings.window_width.load(), 0, 3840));
    oh = u32(std::clamp(settings.window_height.load(), 0, 2160));
    // Same shape only (a stretched window keeps the plain path).
    return ow > w && oh > h && std::abs(float(ow) / float(oh) - float(w) / float(h)) < 0.02f;
}

void TemporalUpscaler::RunDisplayUpscale(VideoCore::ImageId frame) {
    d3_display = false;
    if (!frame || !geometry_color || failed || !Active() || Scaled() || done_this_frame ||
        SanctuarySettings::Get().upscaler != SanctuarySettings::UpscalerFsr3) {
        return;
    }
    auto& color = texture_cache.GetImage(frame);
    const u32 w = color.info.size.width, h = color.info.size.height;
    if (color.info.pixel_format != vk::Format::eA2B10G10R10UnormPack32) {
        return;
    }
    // No mask from the finished frame (Run's RecordReactive): its post changes every pixel.
    if (!mask_ready) {
        opaque_valid = false;
    }
    u32 ow, oh;
    // The window is the render size. FSR 3 upscales the scene's viewport (dynamic resolution:
    // the top left part of the frame) to the frame size for the display copy; with
    // SANCTUARY_FSR_VIEWPORT=0 it runs on the whole frame as anti-aliasing and writes back into
    // it, and the display copy stretches the viewport part as the game does.
    static const bool viewport_off = [] {
        const char* env = std::getenv("SANCTUARY_FSR_VIEWPORT");
        return env && env[0] == '0';
    }();
    const bool viewport = !DisplayUpscaleSize(w, h, ow, oh) && !viewport_off;
    if (viewport) {
        ow = w;
        oh = h;
    } else if (!DisplayUpscaleSize(w, h, ow, oh)) {
        // FSR 3 as anti-aliasing, written back into the frame.
        if (camera_motion.Ready()) {
            done_this_frame = true;
            scene_color = frame;
            const bool was_reset = reset;
            dispatched_last_frame = false;
            Run();
            if (dispatched_last_frame) {
                NoteFrameGenInputs(w, h, 0.0f, was_reset);
            }
        } else {
            reset = true;
        }
        return;
    }
    if (!camera_motion.Ready()) {
        reset = true;
        return;
    }
    auto& depth = texture_cache.GetImage(camera_motion.Depth());
    if (depth.info.size.width != w || depth.info.size.height < h ||
        depth.info.size.height > h + 8) {
        return;
    }
    if (!EnsureResources(w, h, ow, oh, false)) {
        failed = true;
        return;
    }
    const auto rect = camera_motion.SceneRect(w, h);
    if (viewport) {
        // As much larger than the frame as the frame is than the viewport.
        viewport_target = EnsureViewportTarget((w * w + rect[0] / 2) / rect[0],
                                               (h * h + rect[1] / 2) / rect[1],
                                               color.info.pixel_format);
    } else {
        EnsureUiResources(ow, oh, color.info.pixel_format, depth.info.pixel_format);
    }
    const auto depth_format = depth.info.pixel_format;
    const auto depth_view = CachedView(depth, depth_format, vk::ImageAspectFlagBits::eDepth);
    // Barriers go outside a render pass: end the one the previous draw left open first.
    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eBlit,
                    vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto barrier = [&](vk::Image image, vk::ImageAspectFlags aspect,
                             vk::ImageLayout old_layout, vk::PipelineStageFlags2 src_stage,
                             vk::AccessFlags2 src_access, vk::ImageLayout new_layout,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    const auto blit = vk::PipelineStageFlagBits2::eBlit;
    // The HUD clears its depth itself; the old contents are not needed.
    if (!viewport) barrier(vk::Image(ui_depth_image),
            vk::ImageAspectFlagBits::eDepth |
                (depth_format == vk::Format::eD32SfloatS8Uint ? vk::ImageAspectFlagBits::eStencil
                                                             : vk::ImageAspectFlags{}),
            vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
            vk::ImageLayout::eGeneral, all,
            vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
    barrier(vk::Image(motion_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
            all, vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    camera_motion.RecordMotion(cmdbuf, depth_view, *motion_view, w, h, viewport);
    barrier(vk::Image(motion_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
            vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);
    // The 10-bit frame as 16-bit float (FSR takes no 10-bit input).
    barrier(vk::Image(input_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
            all, vk::AccessFlagBits2::eNone, vk::ImageLayout::eTransferDstOptimal, blit,
            vk::AccessFlagBits2::eTransferWrite);
    const auto full = [](u32 x, u32 y) {
        return std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(x), s32(y), 1}};
    };
    cmdbuf.blitImage(color.GetImage(), vk::ImageLayout::eGeneral, vk::Image(input_image),
                     vk::ImageLayout::eTransferDstOptimal,
                     vk::ImageBlit{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   .srcOffsets = full(w, h),
                                   .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   .dstOffsets = full(w, h)},
                     vk::Filter::eNearest);
    barrier(vk::Image(input_image), vk::ImageAspectFlagBits::eColor,
            vk::ImageLayout::eTransferDstOptimal, blit, vk::AccessFlagBits2::eTransferWrite,
            vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);
    barrier(vk::Image(output_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
            all, vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, all, rw);

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_frame).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    last_frame = now;
    const auto& settings = SanctuarySettings::Get();
    FfxVkPortableUpscaleDispatchInfo info{};
    info.structSize = sizeof(info);
    info.commandBuffer = cmdbuf;
    info.color = Describe(vk::Image(input_image), vk::Format::eR16G16B16A16Sfloat, w, h,
                          vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                              vk::ImageUsageFlagBits::eTransferDst,
                          vk::ImageAspectFlagBits::eColor,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.depth = Describe(depth.GetImage(), depth_format, w, h, depth.usage_flags,
                          vk::ImageAspectFlagBits::eDepth,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.motionVectors = Describe(vk::Image(motion_image), vk::Format::eR16G16Sfloat, w, h,
                                  vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                  vk::ImageAspectFlagBits::eColor,
                                  FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.output = Describe(vk::Image(output_image), vk::Format::eR16G16B16A16Sfloat, ow, oh,
                           vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                               vk::ImageUsageFlagBits::eTransferSrc,
                           vk::ImageAspectFlagBits::eColor,
                           FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    info.exposure.structSize = sizeof(info.exposure);
    info.reactiveMask.structSize = sizeof(info.reactiveMask);
    if (mask_ready) {
        info.reactiveMask = Describe(vk::Image(reactive_image), vk::Format::eR8Unorm, w, h,
                                     vk::ImageUsageFlagBits::eStorage |
                                         vk::ImageUsageFlagBits::eSampled,
                                     vk::ImageAspectFlagBits::eColor,
                                     FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    }
    info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
    const float sign = SanctuaryToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
    info.jitterOffset = {sign * jitter[0], sign * jitter[1]};
    info.motionVectorScale = {1.0f, 1.0f};
    info.renderSize = viewport ? FfxVkPortableExtent2D{rect[0], rect[1]} : FfxVkPortableExtent2D{w, h};
    info.outputSize = {ow, oh};
    info.frameTimeMilliseconds = frame_ms;
    info.preExposure = 1.0f;
    info.cameraNear = camera_motion.Near();
    info.cameraFar = camera_motion.Far();
    info.cameraVerticalFovRadians = camera_motion.VerticalFov();
    info.viewSpaceToMeters = 1.0f;
    info.sharpness = std::min(settings.sharpness.load(), 1.0f);
    info.enableSharpening = settings.sharpen ? VK_TRUE : VK_FALSE;
    info.reset = reset ? VK_TRUE : VK_FALSE;
    info.frameId = frame_id++;
    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.flags = FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {ow, oh};
    done_this_frame = true;
    if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &info)) {
        PrintIssues("display dispatch", issues);
        failed = true;
        return;
    }
    if (ffxVkPortableUpscaleContextRecordDispatch(context, &info) != FFX_VK_PORTABLE_OK) {
        std::printf("Upscaler: FSR 3 display dispatch failed\n");
        failed = true;
        return;
    }
    NoteFrameGenInputs(w, h, frame_ms, info.reset != VK_FALSE);
    reset = false;
    dispatched_last_frame = true;
    if (const int dump = DumpFrame(); dump >= 0) {
        barrier(vk::Image(output_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
                all, rw, vk::ImageLayout::eGeneral, all, rw);
        DumpImages(instance, scheduler, cmdbuf, dump,
                   {{vk::Image(input_image), w, h, 8, "input", "rgba16f"},
                    {vk::Image(motion_image), w, h, 4, "motion", "rg16f"},
                    {vk::Image(output_image), ow, oh, 8, "output", "rgba16f"}});
        DumpImages(instance, scheduler, cmdbuf, dump,
                   {{depth.GetImage(), w, h, 4, "depth", "f32", vk::ImageAspectFlagBits::eDepth}});
        if (mask_ready) {
            DumpImages(instance, scheduler, cmdbuf, dump,
                       {{vk::Image(reactive_image), w, h, 1, "reactive", "r8"},
                        {vk::Image(opaque_image), w, h, 8, "opaque", "rgba16f"}});
        }
    }
    if (viewport) {
        // The menu's sharpening above AMD's RCAS range, as the 1:1 path (Run).
        ExtraSharpen(cmdbuf, vk::Image(output_image), false, ow, oh);
    }
    // Into the output-size frame the display copy samples (16-bit float -> the frame's 10-bit
    // format), with the merge shader as the 1:1 path: it reads FSR's output as a storage image.
    const vk::Image merge_target = viewport ? vk::Image(viewport_target->image) : vk::Image(ui_image);
    barrier(vk::Image(output_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
            all, rw, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eShaderRead);
    if (viewport) {
        // The frame's alpha carries data the display copy reads: the viewport part of the frame,
        // enlarged, goes in first; the merge then replaces its RGB with FSR's.
        barrier(merge_target, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eTransferDstOptimal, blit,
                vk::AccessFlagBits2::eTransferWrite);
        cmdbuf.blitImage(color.GetImage(), vk::ImageLayout::eGeneral, merge_target,
                         vk::ImageLayout::eTransferDstOptimal,
                         vk::ImageBlit{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                       .srcOffsets = full(rect[0], rect[1]),
                                       .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                       .dstOffsets = full(w, h)},
                         vk::Filter::eLinear);
        barrier(merge_target, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferDstOptimal,
                blit, vk::AccessFlagBits2::eTransferWrite, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, rw);
    } else {
        barrier(merge_target, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, rw);
    }
    {
        const vk::DescriptorImageInfo out_info{.imageView = *output_view,
                                               .imageLayout = vk::ImageLayout::eGeneral};
        if (!viewport && (!display_frame_view || display_frame_image != vk::Image(ui_image))) {
            display_frame_image = vk::Image(ui_image);
            display_frame_view = Check(instance.GetDevice().createImageViewUnique({
                .image = display_frame_image,
                .viewType = vk::ImageViewType::e2D,
                .format = ui_format,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
        }
        const vk::DescriptorImageInfo ui_info{
            .imageView = viewport ? *viewport_target->storage : *display_frame_view,
            .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo mask_info{.imageView = *reactive_view,
                                                .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo motion_info{.imageView = *motion_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 5> writes = {{
            {.dstBinding = 0, .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out_info},
            {.dstBinding = 1, .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &ui_info},
            {.dstBinding = 2, .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &mask_info},
            {.dstBinding = 3, .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &motion_info},
            {.dstBinding = 4, .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &motion_info},
        }};
        const u32 mode = 0;
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *merge_rgb10a2_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *merge_pipeline_layout, 0,
                                    writes);
        cmdbuf.pushConstants(*merge_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(mode), &mode);
        cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
    }
    if (viewport) {
        // The display copy samples it next.
        barrier(merge_target, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);
        viewport_phase = true;
        ui_color = frame;
        d3_jitter_sizes = {rect[0], w};
        static int told = 0;
        if (told < 2 && std::getenv("SANCTUARY_CAMERA_TRACE")) {
            ++told;
            std::printf("Upscaler: viewport upscale %ux%u -> %ux%u\n", rect[0], rect[1], w, h);
        }
        return;
    }
    barrier(vk::Image(ui_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
            vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);
    ui_phase = true;
    ui_color = frame;
    ui_depth = {};
    d3_display = true;
    static int told = 0;
    if (told < 2 && std::getenv("SANCTUARY_CAMERA_TRACE")) {
        ++told;
        std::printf("Upscaler: display upscale %ux%u -> %ux%u, ui image %s\n", w, h, ow, oh,
                    vk::to_string(ui_format).c_str());
    }
    display_size = {w, h};
    d3_jitter_sizes = {w, ow};
}

void TemporalUpscaler::OnDispatch(u64 cs_hash) {
    if (cs_hash != trigger_hash || done_this_frame || failed || Scaled()) {
        return;
    }
    done_this_frame = true;
    if (!scene_color || !camera_motion.Ready() || !Active()) {
        reset = true;
        return;
    }
    Run();
}

bool TemporalUpscaler::EnsureResources(u32 w, u32 h, u32 ow, u32 oh, bool hdr) {
    // DLSS, like FSR 4, has its own context: no portable FSR 3 context is made.
    const bool use_fsr4 = UseFsr4() || UseDlss();
    const bool use_taa = SanctuarySettings::Get().upscaler == SanctuarySettings::UpscalerTaa;
    if (use_taa && (w != ow || h != oh)) {
        std::printf("TAA: remove SANCTUARY_RENDER_RES to use native-resolution TAA\n");
        return false;
    }
    if (resources_ready && w == width && h == height && ow == out_width && oh == out_height &&
        hdr == context_hdr && use_fsr4 == resources_fsr4 && use_taa == resources_taa) {
        return true;
    }
    const auto device = instance.GetDevice();
    // FSR 4 owns no portable FSR 3 context, but its images can still be in flight.
    scheduler.Finish();
    resources_ready = false;
    if (context) {
        ffxVkPortableUpscaleContextDestroy(context);
        context = nullptr;
    }
    width = w;
    height = h;
    out_width = ow;
    out_height = oh;
    context_hdr = hdr;

    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = instance.GetInstance();
    device_info.physicalDevice = instance.GetPhysicalDevice();
    device_info.device = device;
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = instance.GetGraphicsQueue();
    device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = instance.IsShaderFloat16Enabled();
    device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
    device_info.synchronization2Enabled = VK_TRUE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    // Native AA runs on the HDR scene color; scaled presets on the game's tonemapped frame.
    // Without an exposure image FSR needs its own exposure, HDR or not (with neither its
    // output was garbage for Diablo III's 10-bit frame).
    create_info.flags = hdr ? FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT |
                                  FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE
                            : FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {ow, oh};
    // FSR 4 has its own model context (vk_fsr4); the images below are shared.
    if (!use_fsr4 && !use_taa) {
        if (const u64 issues = ffxVkPortableValidateUpscaleCreateInfo(&create_info)) {
            PrintIssues("create info", issues);
            return false;
        }
        if (ffxVkPortableUpscaleContextCreate(&device_info, &create_info, &context) !=
            FFX_VK_PORTABLE_OK) {
            std::printf("Upscaler: FSR 3 context creation failed\n");
            context = nullptr;
            return false;
        }
    }

    const auto allocator = instance.GetAllocator();
    motion_view.reset();
    output_view.reset();
    motion_image = VideoCore::UniqueImage(device, allocator);
    motion_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        // Copied by frame generation and the frame dumps.
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    output_image = VideoCore::UniqueImage(device, allocator);
    output_image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {ow, oh, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    output_view = Check(device.createImageViewUnique({
        .image = vk::Image(output_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    motion_view = Check(device.createImageViewUnique({
        .image = vk::Image(motion_image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    const auto make_image = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                                vk::Format format, vk::ImageUsageFlags usage, u32 iw = 0,
                                u32 ih = 0, vk::ImageAspectFlags aspect =
                                                vk::ImageAspectFlagBits::eColor,
                                vk::ImageCreateFlags flags = {}) {
        view.reset();
        image = VideoCore::UniqueImage(device, allocator);
        image.Create(vk::ImageCreateInfo{
            .flags = flags,
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {iw ? iw : w, ih ? ih : h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        }));
    };
    make_image(opaque_image, opaque_view, vk::Format::eR16G16B16A16Sfloat,
               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst |
                   vk::ImageUsageFlagBits::eTransferSrc);
    make_image(input_image, input_view, vk::Format::eR16G16B16A16Sfloat,
               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                   vk::ImageUsageFlagBits::eTransferDst);
    make_image(reactive_image, reactive_view, vk::Format::eR8Unorm,
               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);
    for (u32 i = 0; i < taa_history.size(); ++i) {
        taa_history_views[i].reset();
        taa_history[i] = VideoCore::UniqueImage{};
        if (use_taa) {
            make_image(taa_history[i], taa_history_views[i], vk::Format::eR32G32B32A32Sfloat,
                       vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                           vk::ImageUsageFlagBits::eTransferSrc,
                       ow, oh);
        }
    }
    taa_next = 0;
    CreatePipelines();
    opaque_valid = false;
    reset = true;
    resources_ready = true;
    resources_fsr4 = use_fsr4;
    resources_taa = use_taa;
    if (use_taa) {
        std::printf("TAA: context %ux%u -> %ux%u (%s), no FSR model\n", w, h, ow, oh,
                    hdr ? "HDR scene color" : "tonemapped frame");
        return true;
    }
    if (use_fsr4) {
        std::printf("Upscaler: FSR 4 inputs %ux%u -> %ux%u\n", w, h, ow, oh);
        return true;
    }
    FfxVkPortableMemoryUsage usage{};
    usage.structSize = sizeof(usage);
    ffxVkPortableUpscaleContextGetMemoryUsage(context, &usage);
    std::printf("Upscaler: FSR 3 context %ux%u -> %ux%u (%s), %.1f MB\n", w, h, ow, oh,
                hdr ? "HDR scene color" : "tonemapped frame", usage.totalUsageInBytes / 1e6);
    return true;
}

void TemporalUpscaler::CreatePipelines() {
    if (merge_pipeline) {
        return;
    }
    const auto device = instance.GetDevice();
    const auto storage_layout = [&](u32 count, vk::UniqueDescriptorSetLayout& layout,
                                    vk::UniquePipelineLayout& pipeline_layout, u32 push_size) {
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < count; ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = vk::DescriptorType::eStorageImage,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = count,
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = push_size};
        pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
    };
    const auto compute = [&](const auto& code, vk::PipelineLayout layout) {
        const auto module = CompileSPV(code, device);
        auto pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = layout,
                }));
        device.destroyShaderModule(module);
        return pipeline;
    };
    {
        // Merge: three storage images, then the motion and object motion images (debug).
        std::array<vk::DescriptorSetLayoutBinding, 5> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = i < 3 ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        merge_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = sizeof(u32)};
        merge_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*merge_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
    }
    merge_pipeline = compute(UPSCALE_MERGE_COMP, *merge_pipeline_layout);
    merge_rgb10a2_pipeline = compute(UPSCALE_MERGE_RGB10A2_COMP, *merge_pipeline_layout);
    storage_layout(3, reactive_desc_layout, reactive_pipeline_layout, 3 * sizeof(float));
    reactive_pipeline = compute(UPSCALE_REACTIVE_COMP, *reactive_pipeline_layout);
    {
        // Diablo III's mask: opaque copy (storage), the 2-sample scene (sampled), the mask.
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                           .descriptorType = i == 1 ? vk::DescriptorType::eSampledImage
                                                    : vk::DescriptorType::eStorageImage,
                           .descriptorCount = 1,
                           .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        reactive_msaa_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                         .offset = 0,
                                         .size = 4 * sizeof(u32)};
        reactive_msaa_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*reactive_msaa_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
    }
    reactive_msaa_pipeline = compute(REACTIVE_MSAA_COMP, *reactive_msaa_pipeline_layout);
    storage_layout(2, taa_sharpen_desc_layout, taa_sharpen_pipeline_layout, sizeof(float));
    taa_sharpen_pipeline = compute(TAA_SHARPEN_COMP, *taa_sharpen_pipeline_layout);
    taa_sharpen_ldr_pipeline = compute(TAA_SHARPEN_LDR_COMP, *taa_sharpen_pipeline_layout);
    {
        std::array<vk::DescriptorSetLayoutBinding, 7> bindings{};
        for (u32 i = 0; i < bindings.size(); ++i) {
            bindings[i] = {.binding = i,
                .descriptorType = i < 4 ? vk::DescriptorType::eCombinedImageSampler
                                        : vk::DescriptorType::eStorageImage,
                .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute};
        }
        taa_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 64};
        taa_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1, .pSetLayouts = &*taa_desc_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push}));
        taa_sampler = Check(device.createSamplerUnique({
            .magFilter = vk::Filter::eNearest, .minFilter = vk::Filter::eNearest,
            .mipmapMode = vk::SamplerMipmapMode::eNearest,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge}));
        taa_pipeline = compute(TAA_COMP, *taa_pipeline_layout);
    }
}

void TemporalUpscaler::RecordTaa(vk::CommandBuffer cmdbuf, vk::ImageView color,
                                vk::ImageView depth) {
    // Make scene writes and the last storage-image history writes visible to sampled reads.
    const vk::MemoryBarrier2 inputs{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite |
                         vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead |
                         vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,.pMemoryBarriers = &inputs});
    // Histories alternate between reads and writes. Queue ordering plus this dependency
    // protects both the previous write and a reused destination's previous read.
    std::array<vk::ImageMemoryBarrier2, 2> barriers{};
    for (u32 i = 0; i < barriers.size(); ++i) {
        barriers[i] = {.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                             vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead |
                             vk::AccessFlagBits2::eShaderStorageWrite,
            .oldLayout = reset ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(taa_history[i]),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(barriers.size()),
                             .pImageMemoryBarriers = barriers.data()});
    const std::array<vk::ImageView, 7> views{
        color, depth, *motion_view, *taa_history_views[1 - taa_next],
        *output_view, *taa_history_views[taa_next], *opaque_view};
    std::array<vk::DescriptorImageInfo, 7> infos{};
    std::array<vk::WriteDescriptorSet, 7> writes{};
    for (u32 i = 0; i < infos.size(); ++i) {
        infos[i] = {.sampler = i < 4 ? *taa_sampler : vk::Sampler{},
                    .imageView = views[i], .imageLayout = vk::ImageLayout::eGeneral};
        writes[i] = {.dstBinding = i, .descriptorCount = 1,
            .descriptorType = i < 4 ? vk::DescriptorType::eCombinedImageSampler
                                    : vk::DescriptorType::eStorageImage,
            .pImageInfo = &infos[i]};
    }
    struct Params {
        std::array<float, 2> jitter;
        u32 reset, pad;
        std::array<std::array<float, 4>, 3> depth;
    } params{jitter, reset ? 1u : 0u, std::getenv("SANCTUARY_TAA_DIAGNOSTICS") ?
                 u32(std::clamp(std::atoi(std::getenv("SANCTUARY_TAA_DIAGNOSTICS")),1,3)) : 0u,
             camera_motion.TaaDepthParameters()};
    // Optional techniques for A/B in one run (SANCTUARY_TOGGLE_FILE bits 51-54): tonemapped blending,
    // YCoCg clipping, variance clipping, 3x3 reconstruction. Measured in game on 2026-10-02
    // (static and panning camera, interleaved captures): none improved stability beyond run
    // noise; YCoCg clipping and the reconstruction made it worse. Default: all off.
    params.pad |= (SanctuaryToggle::Disabled(SanctuaryToggle::TaaTonemapBlend) ? 1u << 8 : 0u) |
                  (SanctuaryToggle::Disabled(SanctuaryToggle::TaaClip) ? 2u << 8 : 0u) |
                  (SanctuaryToggle::Disabled(SanctuaryToggle::TaaVariance) ? 4u << 8 : 0u) |
                  (SanctuaryToggle::Disabled(SanctuaryToggle::TaaFilter) ? 8u << 8 : 0u) |
                  (SanctuaryToggle::Disabled(SanctuaryToggle::TaaKeepNearerHistory) ? 16u << 8 : 0u);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *taa_pipeline);
    if (params.pad & 0xffu) {
        static u32 diagnostic_frames = 0;
        if (diagnostic_frames++ < 16)
            std::printf("TAA diagnostic: reset=%u jitter=%.6f,%.6f mode=%u\n",
                        params.reset, jitter[0], jitter[1], params.pad);
    }
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *taa_pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*taa_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(params), &params);
    cmdbuf.dispatch((out_width + 7) / 8, (out_height + 7) / 8, 1);
    const auto& settings = SanctuarySettings::Get();
    const float strength = std::clamp(settings.sharpness.load(), 0.0f, 2.0f);
    if (settings.sharpen && strength > 0.0f && !(params.pad & 0xffu)) {
        const vk::MemoryBarrier2 resolved{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead |
                             vk::AccessFlagBits2::eShaderStorageWrite};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &resolved});
        const std::array<vk::DescriptorImageInfo, 2> sharpen_infos{{
            {.imageView = *taa_history_views[taa_next], .imageLayout = vk::ImageLayout::eGeneral},
            {.imageView = *output_view, .imageLayout = vk::ImageLayout::eGeneral}}};
        std::array<vk::WriteDescriptorSet, 2> sharpen_writes{};
        for (u32 i = 0; i < sharpen_writes.size(); ++i)
            sharpen_writes[i] = {.dstBinding = i, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &sharpen_infos[i]};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *taa_sharpen_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,
                                    *taa_sharpen_pipeline_layout, 0, sharpen_writes);
        cmdbuf.pushConstants(*taa_sharpen_pipeline_layout, vk::ShaderStageFlagBits::eCompute,
                             0, sizeof(strength), &strength);
        cmdbuf.dispatch((out_width + 7) / 8, (out_height + 7) / 8, 1);
    }
    taa_next = 1 - taa_next;
}

void TemporalUpscaler::ExtraSharpen(vk::CommandBuffer cmdbuf, vk::Image target, bool ldr, u32 w,
                                    u32 h) {
    const auto& settings = SanctuarySettings::Get();
    const float extra = std::clamp(settings.sharpness.load(), 0.0f, 2.0f) - 1.0f;
    if (!settings.sharpen || extra <= 0.0f) {
        return;
    }
    const auto device = instance.GetDevice();
    if (extra_sharpen_width != w || extra_sharpen_height != h) {
        // Retired command buffers may still read the old copy. `cmdbuf` stays open.
        scheduler.WaitSubmitted();
        extra_sharpen_view.reset();
        extra_sharpen_image = VideoCore::UniqueImage(device, instance.GetAllocator());
        extra_sharpen_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .extent = {w, h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        extra_sharpen_view = Check(device.createImageViewUnique({
            .image = vk::Image(extra_sharpen_image), .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        extra_sharpen_width = w;
        extra_sharpen_height = h;
    }
    vk::ImageView target_view = *output_view;
    if (ldr) {
        if (!ui_storage_view) {
            // The UI image's own format may be sRGB; FSR 3 writes it through a UNORM view too.
            ui_storage_view = Check(device.createImageViewUnique({
                .image = target, .viewType = vk::ImageViewType::e2D,
                .format = vk::Format::eR8G8B8A8Unorm,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
        }
        target_view = *ui_storage_view;
    }
    const auto image_barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                                   vk::PipelineStageFlags2 src, vk::AccessFlags2 src_access,
                                   vk::PipelineStageFlags2 dst, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src, .srcAccessMask = src_access,
            .dstStageMask = dst, .dstAccessMask = dst_access,
            .oldLayout = old_layout, .newLayout = vk::ImageLayout::eGeneral,
            .image = image, .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    constexpr auto all = vk::PipelineStageFlagBits2::eAllCommands;
    constexpr auto rw = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    image_barrier(target, vk::ImageLayout::eGeneral, all, rw, vk::PipelineStageFlagBits2::eBlit,
                  vk::AccessFlagBits2::eTransferRead);
    image_barrier(vk::Image(extra_sharpen_image), vk::ImageLayout::eUndefined, all, rw,
                  vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageBlit region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array{vk::Offset3D{}, vk::Offset3D{s32(w), s32(h), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array{vk::Offset3D{}, vk::Offset3D{s32(w), s32(h), 1}},
    };
    cmdbuf.blitImage(target, vk::ImageLayout::eGeneral, vk::Image(extra_sharpen_image),
                     vk::ImageLayout::eGeneral, region, vk::Filter::eNearest);
    image_barrier(vk::Image(extra_sharpen_image), vk::ImageLayout::eGeneral,
                  vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite,
                  vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageRead);
    image_barrier(target, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eBlit,
                  vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageWrite);
    const std::array<vk::DescriptorImageInfo, 2> infos{{
        {.imageView = *extra_sharpen_view, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = target_view, .imageLayout = vk::ImageLayout::eGeneral}}};
    std::array<vk::WriteDescriptorSet, 2> writes{};
    for (u32 i = 0; i < writes.size(); ++i)
        writes[i] = {.dstBinding = i, .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &infos[i]};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,
                        ldr ? *taa_sharpen_ldr_pipeline : *taa_sharpen_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *taa_sharpen_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*taa_sharpen_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(extra), &extra);
    cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    image_barrier(target, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderStorageWrite, all, rw);
}

void TemporalUpscaler::OnBlendedSceneDraw() {
    // The mask is opt-in (menu, SANCTUARY_REACTIVE=1): on thin mist it traded trails for
    // jitter shimmer, which looked worse. Toggle 1 << 27 switches it off.
    if (snapshot_taken || !scene_color || !Active() || !ReactiveOn()) {
        return;
    }
    snapshot_taken = true;
    auto& color = texture_cache.GetImage(scene_color);
    // With a live preset the scene is in the reduced proxy: the mask and the FSR context
    // must use that size, or each frame would recreate the context (native <-> reduced).
    const bool reduced = ReducedScene(color);
    const u32 ow = color.info.size.width, oh = color.info.size.height;
    // Scaled presets: the scene fills the top-left of its aligned targets (see RunScaled).
    const auto scene = Scaled() ? SceneSize(ow, oh) : std::array<u32, 2>{ow, oh};
    const u32 w = reduced ? scene_targets.Size().width : scene[0];
    const u32 h = reduced ? scene_targets.Size().height : scene[1];
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eTransferSrc)) {
        return;
    }
    if (!EnsureResources(w, h, Scaled() ? target_width : ow,
                         Scaled() ? target_height : oh, !Scaled())) {
        failed = true;
        return;
    }
    scheduler.EndRendering();
    vk::Image source = color.GetImage();
    vk::ImageLayout source_layout = vk::ImageLayout::eTransferSrcOptimal;
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        source = scene_targets.Read(scene_color, ci, vk::PipelineStageFlagBits2::eTransfer,
                                    vk::AccessFlagBits2::eTransferRead).image;
        source_layout = vk::ImageLayout::eGeneral;
    } else {
        runtime.Transit(&color, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
    }
    const vk::Image opaque = vk::Image(opaque_image);
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {w, h, 1},
    };
    // On the recording thread: direct recording here would move the rest of the scene there.
    scheduler.Record([opaque, source, source_layout, region](vk::CommandBuffer cmdbuf) {
        const auto to_general = [&](vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                    vk::ImageLayout old_layout, vk::PipelineStageFlags2 dst_stage,
                                    vk::AccessFlags2 dst_access) {
            const vk::ImageMemoryBarrier2 barrier{
                .srcStageMask = src_stage,
                .srcAccessMask = src_access,
                .dstStageMask = dst_stage,
                .dstAccessMask = dst_access,
                .oldLayout = old_layout,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = opaque,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        };
        // The previous frame's mask pass read it: wait for that before overwriting.
        to_general(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eNone,
                   vk::ImageLayout::eUndefined, vk::PipelineStageFlagBits2::eTransfer,
                   vk::AccessFlagBits2::eTransferWrite);
        cmdbuf.copyImage(source, source_layout, opaque, vk::ImageLayout::eGeneral, region);
        to_general(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                   vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                   vk::AccessFlagBits2::eShaderRead);
    });
    opaque_valid = true;
}

void TemporalUpscaler::OnSceneComposite() {
    if (!opaque_valid || mask_ready || failed || !ReactiveOn()) {
        return;
    }
    auto& color = texture_cache.GetImage(scene_color);
    const bool reduced = ReducedScene(color);
    const auto scene = Scaled() ? SceneSize(color.info.size.width, color.info.size.height)
                                : std::array<u32, 2>{color.info.size.width, color.info.size.height};
    const u32 w = reduced ? scene_targets.Size().width : scene[0];
    const u32 h = reduced ? scene_targets.Size().height : scene[1];
    if (w != width || h != height || !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        return;
    }
    scheduler.EndRendering();
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        const auto proxy = scene_targets.Read(scene_color, ci);
        mask_ready = RecordReactive(proxy.view);
        return;
    }
    const auto device = instance.GetDevice();
    const auto color_view = Check(device.createImageView({
        .image = vk::Image(color.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    mask_ready = RecordReactive(color_view);
    scheduler.DeferOperation([device, color_view] { device.destroyImageView(color_view); });
}

bool TemporalUpscaler::RecordReactive(vk::ImageView color_view) {
    if (!opaque_valid || !ReactiveOn()) {
        return false;
    }
    const vk::Image reactive = vk::Image(reactive_image);
    const vk::ImageView opaque = *opaque_view, mask = *reactive_view;
    const vk::Pipeline pipeline = *reactive_pipeline;
    const vk::PipelineLayout layout = *reactive_pipeline_layout;
    const u32 w = width, h = height;
    // Relative color change times the scale (default 1), zero below the threshold (0.2), at
    // most the maximum (0.9, never fully reactive) — the defaults of AMD's mask generator.
    const auto& settings = SanctuarySettings::Get();
    const std::array<float, 3> params{settings.reactive_scale, settings.reactive_max,
                                      settings.reactive_threshold};
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier2 to_write{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = reactive,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write});
        const vk::DescriptorImageInfo opaque_info{.imageView = opaque,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo scene_info{.imageView = color_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo mask_info{.imageView = mask,
                                                .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 3> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &opaque_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &scene_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &mask_info},
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0,
                                    writes);
        cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(params), params.data());
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = reactive,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    });
    return true;
}

void TemporalUpscaler::Run() {
    if (auto* profiler = GpuProfiler::Get()) {
        const char* label = SanctuarySettings::Get().upscaler == SanctuarySettings::UpscalerTaa
            ? "upscaler Run (TAA)" : "upscaler Run (FSR)";
        profiler->Mark(0xF5A0'0000ull ^ std::hash<std::string_view>{}(label),
                       [label] { return std::string{label}; });
    }
    auto& color = texture_cache.GetImage(scene_color);
    auto& depth = texture_cache.GetImage(camera_motion.Depth());
    const u32 ow = color.info.size.width, oh = color.info.size.height;
    const bool reduced = ReducedScene(color) && scene_targets.EligibleScene(depth);
    const u32 w = reduced ? scene_targets.Size().width : ow;
    const u32 h = reduced ? scene_targets.Size().height : oh;
    // Diablo III's scene is 10-bit unorm (no HDR): FSR reads a 16-bit float copy of it.
    const bool ten_bit = color.info.pixel_format == vk::Format::eA2B10G10R10UnormPack32;
    if ((color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat && !ten_bit) ||
        (ten_bit && reduced) || depth.info.size.width != ow || depth.info.size.height < oh ||
        depth.info.size.height > oh + 8 ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("Upscaler: scene %s %ux%u / depth %ux%u not usable\n",
                        vk::to_string(color.info.pixel_format).c_str(), ow, oh,
                        depth.info.size.width, depth.info.size.height);
        }
        return;
    }
    if (!EnsureResources(w, h, ow, oh, !ten_bit)) {
        failed = true;
        return;
    }
    const auto depth_format = depth.info.pixel_format;
    const auto depth_view =
        CachedView(depth, depth_format, vk::ImageAspectFlagBits::eDepth);
    const auto color_view =
        CachedView(color, color.info.pixel_format, vk::ImageAspectFlagBits::eColor);

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    vk::Image input_color = color.GetImage(), input_depth = depth.GetImage();
    vk::ImageView input_color_view = color_view, input_depth_view = depth_view;
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        const auto c = scene_targets.Read(scene_color, ci);
        ci.format = depth.info.pixel_format;
        const auto d = scene_targets.Read(camera_motion.Depth(), ci);
        input_color = c.image;
        input_depth = d.image;
        input_color_view = c.view;
        input_depth_view = d.view;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    vk::Format input_format = color.info.pixel_format;

    const auto own_barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                                 vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                 vk::ImageLayout new_layout, vk::PipelineStageFlags2 dst_stage,
                                 vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    if (ten_bit) {
        // 10-bit unorm -> 16-bit float, same size (a format-converting blit).
        own_barrier(vk::Image(input_image), vk::ImageLayout::eUndefined, all,
                    vk::AccessFlagBits2::eNone, vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite);
        const vk::ImageMemoryBarrier2 to_blit{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = color.GetImage(),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_blit});
        const vk::ImageBlit region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(w), s32(h), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(w), s32(h), 1}},
        };
        cmdbuf.blitImage(color.GetImage(), vk::ImageLayout::eGeneral, vk::Image(input_image),
                         vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
        own_barrier(vk::Image(input_image), vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite,
                    vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);
        input_color = vk::Image(input_image);
        input_color_view = *input_view;
        input_format = vk::Format::eR16G16B16A16Sfloat;
    }
    // Previous contents are not needed: the layouts start from undefined every frame.
    own_barrier(vk::Image(motion_image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    own_barrier(vk::Image(output_image), vk::ImageLayout::eUndefined, all,
                vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, all, rw);

    camera_motion.RecordMotion(cmdbuf, input_depth_view, *motion_view, w, h);
    own_barrier(vk::Image(motion_image), vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

    const bool has_reactive = mask_ready || RecordReactive(input_color_view);

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_frame).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    last_frame = now;

    bool dispatched = false;
    if (SanctuarySettings::Get().upscaler == SanctuarySettings::UpscalerTaa) {
        RecordTaa(cmdbuf, input_color_view, input_depth_view);
        dispatched = true;
    } else if (UseFsr4()) {
        dispatched = RecordFsr4(cmdbuf, {input_color, input_color_view, w, h},
                                {input_depth, input_depth_view, w, h}, w, h, ow, oh, frame_ms);
    } else if (UseDlss()) {
        dispatched = RecordDlss(
            cmdbuf,
            {input_color, input_color_view, input_format, vk::ImageAspectFlagBits::eColor, w, h},
            {input_depth, input_depth_view, depth_format, vk::ImageAspectFlagBits::eDepth, w, h}, w, h,
            ow, oh, frame_ms, !ten_bit);
    } else {
        FfxVkPortableUpscaleDispatchInfo info{};
        info.structSize = sizeof(info);
        info.commandBuffer = cmdbuf;
        info.color = Describe(input_color, input_format, w, h,
                              ten_bit ? vk::ImageUsageFlags(vk::ImageUsageFlagBits::eStorage |
                                                            vk::ImageUsageFlagBits::eSampled |
                                                            vk::ImageUsageFlagBits::eTransferDst)
                                      : color.usage_flags,
                              vk::ImageAspectFlagBits::eColor,
                              FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.depth = Describe(input_depth, depth_format, w, h, depth.usage_flags,
                              vk::ImageAspectFlagBits::eDepth,
                              FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.motionVectors = Describe(vk::Image(motion_image), vk::Format::eR16G16Sfloat, w, h,
                                      vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                      vk::ImageAspectFlagBits::eColor,
                                      FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        info.output = Describe(vk::Image(output_image), vk::Format::eR16G16B16A16Sfloat, ow, oh,
                               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                                   vk::ImageUsageFlagBits::eTransferSrc,
                               vk::ImageAspectFlagBits::eColor,
                               FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
        // Optional inputs, absent: a described but null image.
        info.exposure.structSize = sizeof(info.exposure);
        info.reactiveMask.structSize = sizeof(info.reactiveMask);
        if (has_reactive) {
            info.reactiveMask = Describe(vk::Image(reactive_image), vk::Format::eR8Unorm, w, h,
                                         vk::ImageUsageFlagBits::eStorage |
                                             vk::ImageUsageFlagBits::eSampled,
                                         vk::ImageAspectFlagBits::eColor,
                                         FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
        }
        info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
        // Toggle 1 << 26 (tests): the opposite sign convention for FSR.
        const float sign = SanctuaryToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
        info.jitterOffset = {sign * jitter[0], sign * jitter[1]};
        info.motionVectorScale = {1.0f, 1.0f};
        info.renderSize = {w, h};
        info.outputSize = {ow, oh};
        info.frameTimeMilliseconds = frame_ms;
        info.preExposure = 1.0f;
        info.cameraNear = camera_motion.Near();
        info.cameraFar = camera_motion.Far();
        info.cameraVerticalFovRadians = camera_motion.VerticalFov();
        info.viewSpaceToMeters = 1.0f;
        // RCAS strength 0..1 (menu, SANCTUARY_FSR_SHARPNESS); jitter at 1:1 softens the image slightly.
        // AMD's RCAS ends at 1; ExtraSharpen adds the rest of the menu's 0..2.
        info.sharpness = std::min(SanctuarySettings::Get().sharpness.load(), 1.0f);
        info.enableSharpening = SanctuarySettings::Get().sharpen ? VK_TRUE : VK_FALSE;
        info.reset = reset ? VK_TRUE : VK_FALSE;
        info.frameId = frame_id++;
    
        FfxVkPortableUpscaleCreateInfo create_info{};
        create_info.structSize = sizeof(create_info);
        create_info.flags = ten_bit ? u32(FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE)
                                    : FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT |
                                          FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
        create_info.maxRenderSize = {w, h};
        create_info.maxOutputSize = {ow, oh};
        if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &info)) {
            static bool printed = false;
            if (!printed) {
                printed = true;
                PrintIssues("dispatch", issues);
            }
            failed = true;
        } else if (ffxVkPortableUpscaleContextRecordDispatch(context, &info) !=
                   FFX_VK_PORTABLE_OK) {
            std::printf("Upscaler: FSR 3 dispatch failed\n");
            failed = true;
        } else {
            dispatched = true;
        }
    }
    if (dispatched) {
        reset = false;
        dispatched_last_frame = true;
        if (const int dump = DumpFrame(); dump >= 0) {
            DumpImages(instance, scheduler, cmdbuf, dump,
                       {{input_color, w, h, 8, "input", "rgba16f"},
                        {vk::Image(motion_image), w, h, 4, "motion", "rg16f"},
                        {vk::Image(output_image), ow, oh, 8, "output", "rgba16f"}});
            if (has_reactive) {
                DumpImages(instance, scheduler, cmdbuf, dump,
                           {{vk::Image(reactive_image), w, h, 1, "reactive", "r8"},
                            {vk::Image(opaque_image), w, h, 8, "opaque", "rgba16f"}});
            }
        }
        if (SanctuarySettings::Get().upscaler != SanctuarySettings::UpscalerTaa) {
            ExtraSharpen(cmdbuf, vk::Image(output_image), false, ow, oh);
        }
        // The result replaces the scene color's RGB (its alpha carries data for the post).
        own_barrier(vk::Image(output_image), vk::ImageLayout::eGeneral, all, rw,
                    vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
        runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        runtime.FlushBarriers();
        const vk::DescriptorImageInfo out_info{.imageView = *output_view,
                                               .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo scene_info{.imageView = color_view,
                                                 .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo mask_info{.imageView = *reactive_view,
                                                .imageLayout = vk::ImageLayout::eGeneral};
        // Sampled for the debug view only; the object image falls back to the motion image.
        const vk::ImageView objects = camera_motion.ObjectMotionView();
        const vk::DescriptorImageInfo motion_info{.imageView = *motion_view,
                                                  .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo objects_info{
            .imageView = objects ? objects : *motion_view,
            .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 5> writes = {{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &out_info},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &scene_info},
            {.dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &mask_info},
            {.dstBinding = 3,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &motion_info},
            {.dstBinding = 4,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &objects_info},
        }};
        // Debug (menu or toggle 1 << 28): the reactive mask in red over a darkened frame;
        // or the motion vectors (menu).
        const int debug_view = SanctuarySettings::Get().debug_view;
        const u32 mode = has_reactive && (debug_view == SanctuarySettings::DebugReactive ||
                                          SanctuaryToggle::Disabled(1u << 28))
                             ? 1
                         : debug_view == SanctuarySettings::DebugMotion ? 2
                                                                 : 0;
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,
                            ten_bit ? *merge_rgb10a2_pipeline : *merge_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *merge_pipeline_layout, 0,
                                    writes);
        cmdbuf.pushConstants(*merge_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(mode), &mode);
        cmdbuf.dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
    }
}

vk::ImageView TemporalUpscaler::CachedView(const VideoCore::Image& image, vk::Format format,
                                           vk::ImageAspectFlags aspect) {
    const vk::Image handle = image.GetImage();
    ViewEntry* oldest = &view_cache[0];
    for (auto& entry : view_cache) {
        if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
            entry.format == format && entry.aspect == aspect) {
            entry.last_use = ++view_uses;
            return entry.view;
        }
        if (entry.last_use < oldest->last_use) {
            oldest = &entry;
        }
    }
    const auto device = instance.GetDevice();
    if (oldest->view) {
        scheduler.DeferOperation([device, view = oldest->view] { device.destroyImageView(view); });
    }
    *oldest = {
        .image = handle,
        .uid = image.image_uid,
        .format = format,
        .aspect = aspect,
        .view = Check(device.createImageView({
            .image = handle,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        })),
        .last_use = ++view_uses,
    };
    return oldest->view;
}

} // namespace Vulkan

namespace Vulkan {

std::array<u32, 2> TemporalUpscaler::SceneSize(u32 w, u32 h) const {
    if (!scaled_session) return {render_width, render_height};
    // The game's own render size (scene constants); SANCTUARY_RENDER_RES until the first camera.
    auto size = camera_motion.RenderSize();
    if (size[0] == 0 || size[1] == 0) {
        size = {render_width, render_height};
    }
    return {std::min(size[0], w), std::min(size[1], h)};
}

float TemporalUpscaler::SceneMipBias() const {
    if (!Active() || SanctuarySettings::Get().upscaler == SanctuarySettings::UpscalerTaa) return 0.0f;
    const float render = float(SanctuarySettings::Get().active_render_width.load());
    const float output = float(Scaled() ? target_width : 1920u);
    return render > 0.0f && render < output ? std::log2(render / output) : 0.0f;
}

bool TemporalUpscaler::Scaled() const {
    return scaled_session || target_width != 1920 || target_height != 1080;
}

void TemporalUpscaler::OnColorTarget(VideoCore::ImageId color) {
    if (ui_phase || !camera_motion.Depth()) {
        return;
    }
    const auto& image = texture_cache.GetImage(color);
    const auto& depth = texture_cache.GetImage(camera_motion.Depth());
    if (image.info.pixel_format == vk::Format::eR8G8B8A8Unorm &&
        image.info.size.width == depth.info.size.width &&
        image.info.size.height == depth.info.size.height) {
        ldr_target = color;
    }
}

void TemporalUpscaler::OnDraw(u64 vs_hash, VideoCore::ImageId color,
                              VideoCore::ImageId depth, bool native_viewport) {
    if (!Scaled() && vs_hash == ui_trigger_vs) done_this_frame = true;
    if (!Scaled() || !color) {
        return;
    }
    if (ui_phase) {
        // A UI movie can start without stencil and enable it for later text/masks.
        if (color == ui_color && depth && depth != ui_depth) {
            EnsureUiResources(ui_width, ui_height, ui_format,
                              texture_cache.GetImage(depth).info.pixel_format);
            PrepareUiDepth(depth);
            ui_depth = depth;
        }
        return;
    }
    const auto& image = texture_cache.GetImage(color);
    const bool movie = vs_hash == ui_trigger_vs || UiComposition::MovieShader(vs_hash) ||
                       (scaled_session && native_viewport);
    const bool ui_draw = movie &&
        (image.info.pixel_format == vk::Format::eR8G8B8A8Unorm ||
         image.info.pixel_format == vk::Format::eR8G8B8A8Srgb) &&
        RenderTarget(image.info.size.width, image.info.size.height) &&
        !FrameCapture::IsDisplayBuffer(image.info.guest_address);
    switch (UiComposition::Choose(Scaled(), ui_draw,
                                  camera_motion.Ready() && ldr_target == color,
                                  Active() && !failed)) {
    case UiComposition::Background::None:
        return;
    case UiComposition::Background::Temporal:
        RunScaled();
        if (ui_phase) {
            return;
        }
        // A failed FSR dispatch must not force UI text back to the scene resolution.
        [[fallthrough]];
    case UiComposition::Background::Copy:
        RunUiOnly(color, depth);
        return;
    }
}

void TemporalUpscaler::EnsureUiResources(u32 w, u32 h, vk::Format color, vk::Format depth) {
    const bool resized = w != ui_width || h != ui_height;
    const bool new_color = !ui_image || resized || color != ui_format;
    const bool new_depth = !ui_depth_image || resized || depth != ui_depth_format;
    if (!new_color && !new_depth) {
        return;
    }
    scheduler.Finish();
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    ui_width = w;
    ui_height = h;
    if (new_color) {
        ui_views.clear();
        ui_view.reset();
        ui_storage_view.reset();
        ui_image = VideoCore::UniqueImage(device, allocator);
        ui_image.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage,
            .imageType = vk::ImageType::e2D,
            .format = color,
            .extent = {w, h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                     vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        ui_format = color;
    }
    if (new_depth) {
        ui_depth_view.reset();
        ui_depth_image = VideoCore::UniqueImage(device, allocator);
        ui_depth_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = depth,
            .extent = {w, h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                     vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        const auto aspect = vk::ImageAspectFlagBits::eDepth |
            (depth == vk::Format::eD32SfloatS8Uint ? vk::ImageAspectFlagBits::eStencil
                                                : vk::ImageAspectFlags{});
        ui_depth_view = Check(device.createImageViewUnique({
            .image = vk::Image(ui_depth_image), .viewType = vk::ImageViewType::e2D,
            .format = depth, .subresourceRange = {aspect, 0, 1, 0, 1},
        }));
        ui_depth_format = depth;
    }
    std::printf("UI: native composition %ux%u (scene %ux%u), independent of FSR history\n",
                w, h, render_width, render_height);
}

void TemporalUpscaler::PrepareUiDepth(VideoCore::ImageId depth_id) {
    scheduler.EndRendering();
    const auto aspect = vk::ImageAspectFlagBits::eDepth |
        (ui_depth_format == vk::Format::eD32SfloatS8Uint ? vk::ImageAspectFlagBits::eStencil
                                                      : vk::ImageAspectFlags{});
    // Copy depth if blit is supported; otherwise just clear it (UI depth test still works).
    const bool copy = depth_id && depth_blit &&
        texture_cache.GetImage(depth_id).info.pixel_format == ui_depth_format;
    if (copy) {
        runtime.Transit(&texture_cache.GetImage(depth_id), vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
    }
    const auto cmd = scheduler.CommandBuffer();
    const vk::ImageSubresourceRange range{aspect, 0, 1, 0, 1};
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = vk::Image(ui_depth_image), .subresourceRange = range,
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    cmd.clearDepthStencilImage(vk::Image(ui_depth_image), vk::ImageLayout::eTransferDstOptimal,
                               {.depth = 1.0f, .stencil = 0}, range);
    if (copy) {
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        const auto& source = texture_cache.GetImage(depth_id);
        const vk::ImageBlit region{
            .srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                vk::Offset3D{s32(source.info.size.width), s32(source.info.size.height), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                vk::Offset3D{s32(ui_width), s32(ui_height), 1}},
        };
        cmd.blitImage(vk::Image(source.backing->image), vk::ImageLayout::eTransferSrcOptimal,
                      vk::Image(ui_depth_image), vk::ImageLayout::eTransferDstOptimal,
                      region, vk::Filter::eNearest);
    }
    // Without blit support, UI depth is cleared but not copied from the scene.
    // UI elements will still depth-test correctly against the cleared buffer.
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                           vk::PipelineStageFlagBits2::eLateFragmentTests;
    barrier.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                            vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eGeneral;
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

void TemporalUpscaler::RunUiOnly(VideoCore::ImageId color_id, VideoCore::ImageId depth_id) {
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xF5A1'0000ull, [] { return std::string{"upscaler RunUiOnly"}; });
    }
    const auto& color = texture_cache.GetImage(color_id);
    const auto depth_format = depth_id ? texture_cache.GetImage(depth_id).info.pixel_format
                                      : vk::Format::eD32SfloatS8Uint;
    EnsureUiResources(target_width, target_height, color.info.pixel_format, depth_format);
    PrepareUiDepth(depth_id);
    // Copy only the background already drawn. The subsequent menu/HUD/text draws execute
    // directly at output resolution, including frames with no scene or camera at all.
    vk::Image source = color.GetImage();
    auto source_layout = vk::ImageLayout::eTransferSrcOptimal;
    u32 source_width = color.info.size.width, source_height = color.info.size.height;
    if (!scaled_session && camera_motion.Depth() && scene_targets.Reduced() &&
        scene_targets.EligibleScene(color)) {
        VideoCore::ImageViewInfo view;
        view.format = color.info.pixel_format;
        const auto proxy = scene_targets.Read(color_id, view,
                                              vk::PipelineStageFlagBits2::eTransfer,
                                              vk::AccessFlagBits2::eTransferRead);
        source = proxy.image;
        source_layout = proxy.layout;
        source_width = render_width;
        source_height = render_height;
    } else {
        runtime.Transit(&texture_cache.GetImage(color_id), vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
    }
    const auto cmd = scheduler.CommandBuffer();
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = vk::Image(ui_image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    const vk::ImageBlit region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
            vk::Offset3D{s32(source_width), s32(source_height), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
            vk::Offset3D{s32(ui_width), s32(ui_height), 1}},
    };
    cmd.blitImage(source, source_layout,
                  vk::Image(ui_image), vk::ImageLayout::eTransferDstOptimal, region,
                  vk::Filter::eLinear);
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    barrier.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                            vk::AccessFlagBits2::eColorAttachmentWrite;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eGeneral;
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    reset = true; // returning to 3D must not reuse history from before a menu/loading screen
    done_this_frame = ui_phase = true;
    ui_color = color_id;
    ui_depth = depth_id;
}

void TemporalUpscaler::RunScaled() {
    if (auto* profiler = GpuProfiler::Get()) {
        const char* label = SanctuarySettings::Get().upscaler == SanctuarySettings::UpscalerTaa
            ? "upscaler RunScaled (TAA)" : "upscaler RunScaled (FSR)";
        profiler->Mark(0xF5A0'0000ull ^ std::hash<std::string_view>{}(label),
                       [label] { return std::string{label}; });
    }
    auto& color = texture_cache.GetImage(ldr_target);
    auto& depth = texture_cache.GetImage(camera_motion.Depth());
    // The scene fills the top-left w x h of its targets (iw x ih, aligned by the game).
    const u32 iw = color.info.size.width, ih = color.info.size.height;
    const auto [w, h] = SceneSize(iw, ih);
    const u32 ow = target_width, oh = target_height;
    if (depth.info.size.width != iw || depth.info.size.height != ih || w > ow || h > oh) {
        return;
    }
    if (!EnsureResources(w, h, ow, oh, false)) {
        failed = true;
        return;
    }
    EnsureUiResources(ow, oh, color.info.pixel_format, depth.info.pixel_format);
    PrepareUiDepth(camera_motion.Depth());
    const auto depth_format = depth.info.pixel_format;
    // Views kept across frames: FSR 4 registers images by view in a registry of eight.
    vk::Image depth_image = depth.GetImage(), color_image = color.GetImage();
    vk::ImageView depth_view{}, color_view{};
    u32 source_width = iw, source_height = ih;
    if (!scaled_session) {
        VideoCore::ImageViewInfo ci, di;
        ci.format = color.info.pixel_format;
        di.format = depth_format;
        const auto cp = scene_targets.Read(ldr_target, ci);
        const auto dp = scene_targets.Read(camera_motion.Depth(), di);
        color_image = cp.image;
        color_view = cp.view;
        depth_image = dp.image;
        depth_view = dp.view;
        source_width = w;
        source_height = h;
    } else {
        depth_view = CachedView(depth, depth_format, vk::ImageAspectFlagBits::eDepth);
        color_view = CachedView(color, color.info.pixel_format, vk::ImageAspectFlagBits::eColor);
        runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }

    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto barrier = [&](vk::Image image, vk::ImageAspectFlags aspect,
                             vk::ImageLayout old_layout, vk::PipelineStageFlags2 src_stage,
                             vk::AccessFlags2 src_access, vk::ImageLayout new_layout,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto color_access = vk::AccessFlagBits2::eColorAttachmentRead |
                              vk::AccessFlagBits2::eColorAttachmentWrite;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    // The previous frame's display pass read it: all commands before.
    barrier(vk::Image(ui_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
            all, vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral, all, rw);
    barrier(vk::Image(motion_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
            all, vk::AccessFlagBits2::eNone, vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
    camera_motion.RecordMotion(cmdbuf, depth_view, *motion_view, w, h);
    barrier(vk::Image(motion_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
            vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
            vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eShaderRead);

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_frame).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    last_frame = now;

    // sanctuary: FSR 4 writes its HDR-format output, copied into the output-size UI image.
    if (UseFsr4() || UseDlss() || SanctuarySettings::Get().upscaler == SanctuarySettings::UpscalerTaa) {
        barrier(vk::Image(output_image), vk::ImageAspectFlagBits::eColor,
                vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
                vk::ImageLayout::eGeneral, all, rw);
        bool ok4 = true;
        if (SanctuarySettings::Get().upscaler == SanctuarySettings::UpscalerTaa) {
            RecordTaa(cmdbuf, color_view, depth_view);
        } else if (UseDlss()) {
            ok4 = RecordDlss(cmdbuf,
                             {color_image, color_view, color.info.pixel_format,
                              vk::ImageAspectFlagBits::eColor, source_width, source_height},
                             {depth_image, depth_view, depth_format, vk::ImageAspectFlagBits::eDepth,
                              source_width, source_height},
                             w, h, ow, oh, frame_ms, false);
        } else {
            ok4 = RecordFsr4(cmdbuf, {color_image, color_view, source_width, source_height},
                            {depth_image, depth_view, source_width, source_height}, w, h, ow,
                            oh, frame_ms);
        }
        if (ok4) {
            if (SanctuarySettings::Get().upscaler != SanctuarySettings::UpscalerTaa) {
                ExtraSharpen(cmdbuf, vk::Image(output_image), false, ow, oh);
            }
            barrier(vk::Image(output_image), vk::ImageAspectFlagBits::eColor,
                    vk::ImageLayout::eGeneral, all, rw, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead);
            barrier(vk::Image(ui_image), vk::ImageAspectFlagBits::eColor,
                    vk::ImageLayout::eGeneral, all, rw, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite);
            const vk::ImageBlit region{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                         vk::Offset3D{s32(ow), s32(oh), 1}},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                         vk::Offset3D{s32(ow), s32(oh), 1}},
            };
            cmdbuf.blitImage(vk::Image(output_image), vk::ImageLayout::eGeneral,
                             vk::Image(ui_image), vk::ImageLayout::eGeneral, region,
                             vk::Filter::eNearest);
            barrier(vk::Image(ui_image), vk::ImageAspectFlagBits::eColor,
                    vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eBlit,
                    vk::AccessFlagBits2::eTransferWrite, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eColorAttachmentOutput, color_access);
            reset = false;
            dispatched_last_frame = true;
            if (const int dump = DumpFrame(); dump >= 0) {
                DumpImages(instance, scheduler, cmdbuf, dump,
                           {{color_image, source_width, source_height, 4, "input", "rgba"},
                            {vk::Image(motion_image), w, h, 4, "motion", "rg16f"},
                            {vk::Image(output_image), ow, oh, 8, "output", "rgba16f"}});
                DumpImages(instance, scheduler, cmdbuf, dump,
                           {{depth_image, source_width, source_height, 4, "depth", "f32",
                             vk::ImageAspectFlagBits::eDepth}});
                if (SanctuarySettings::Get().upscaler == SanctuarySettings::UpscalerTaa &&
                    std::getenv("SANCTUARY_TAA_DIAGNOSTICS")) {
                    DumpImages(instance, scheduler, cmdbuf, dump,
                               {{vk::Image(taa_history[taa_next]),ow,oh,16,
                                 "previous_history","rgba32f"},
                                {vk::Image(opaque_image),ow,oh,8,"history_sample","rgba16f"}});
                }
                // ObjectMotion's attachment can be in a different resolution from the FSR
                // inputs: only dump it when it matches, to avoid an out-of-bounds GPU copy.
                if (const auto object = camera_motion.ObjectMotionImage(w, h); object) {
                    DumpImages(instance, scheduler, cmdbuf, dump,
                               {{object, w, h, 16, "objects", "rgba32f"}});
                }
            }
        }
        done_this_frame = true;
        if (ok4) {
            ui_phase = true;
            ui_color = ldr_target;
            ui_depth = camera_motion.Depth();
        }
        return;
    }

    const auto& settings = SanctuarySettings::Get();
    FfxVkPortableUpscaleDispatchInfo info{};
    info.structSize = sizeof(info);
    info.commandBuffer = cmdbuf;
    info.color = Describe(color_image, color.info.pixel_format, source_width, source_height,
                          color.usage_flags, vk::ImageAspectFlagBits::eColor,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.depth = Describe(depth_image, depth_format, source_width, source_height, depth.usage_flags,
                          vk::ImageAspectFlagBits::eDepth,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.motionVectors = Describe(vk::Image(motion_image), vk::Format::eR16G16Sfloat, w, h,
                                  vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                  vk::ImageAspectFlagBits::eColor,
                                  FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.output = Describe(vk::Image(ui_image), vk::Format::eR8G8B8A8Unorm, ow, oh,
                           vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                               vk::ImageUsageFlagBits::eColorAttachment,
                           vk::ImageAspectFlagBits::eColor,
                           FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    info.exposure.structSize = sizeof(info.exposure);
    info.reactiveMask.structSize = sizeof(info.reactiveMask);
    if (mask_ready && ReactiveOn()) {
        info.reactiveMask = Describe(vk::Image(reactive_image), vk::Format::eR8Unorm, w, h,
            vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
            vk::ImageAspectFlagBits::eColor, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    }

    info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
    const float sign = SanctuaryToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
    info.jitterOffset = {sign * jitter[0], sign * jitter[1]};
    info.motionVectorScale = {1.0f, 1.0f};
    info.renderSize = {w, h};
    info.outputSize = {ow, oh};
    info.frameTimeMilliseconds = frame_ms;
    info.preExposure = 1.0f;
    info.cameraNear = camera_motion.Near();
    info.cameraFar = 3000.0f;
    info.cameraVerticalFovRadians = camera_motion.VerticalFov();
    info.viewSpaceToMeters = 1.0f;
    info.sharpness = std::min(settings.sharpness.load(), 1.0f);
    info.enableSharpening = settings.sharpen ? VK_TRUE : VK_FALSE;
    info.reset = reset ? VK_TRUE : VK_FALSE;
    info.frameId = frame_id++;

    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {ow, oh};
    bool ok = false;
    if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &info)) {
        PrintIssues("scaled dispatch", issues);
        failed = true;
    } else if (ffxVkPortableUpscaleContextRecordDispatch(context, &info) != FFX_VK_PORTABLE_OK) {
        std::printf("Upscaler: FSR 3 scaled dispatch failed\n");
        failed = true;
    } else {
        ok = true;
        reset = false;
        dispatched_last_frame = true;
        ExtraSharpen(cmdbuf, vk::Image(ui_image), true, ow, oh);
    }
    barrier(vk::Image(ui_image), vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral, all,
            rw, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            color_access);

    done_this_frame = true; // the UI is not jittered
    if (ok) {
        ui_phase = true;
        ui_color = ldr_target;
        ui_depth = camera_motion.Depth();
    }
}

vk::ImageView TemporalUpscaler::Mirror(vk::Image image, std::vector<MirrorView>& views,
                                       vk::Format format, vk::ComponentMapping mapping) {
    for (const auto& entry : views) {
        if (entry.format == format && entry.mapping == mapping) {
            return *entry.view;
        }
    }
    const vk::ImageViewUsageCreateInfo usage{
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment,
    };
    auto view = Check(instance.GetDevice().createImageViewUnique({
        .pNext = &usage,
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .components = mapping,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    const vk::ImageView handle = *view;
    views.push_back({format, mapping, std::move(view)});
    return handle;
}

bool TemporalUpscaler::RedirectColor(VideoCore::ImageId color,
                                     const VideoCore::ImageViewInfo& view_info, Target& target) {
    // The viewport upscale is for the display copy only: once a draw goes anywhere but a display
    // buffer (the next frame's scene reuses the frame image), the frame is read as it is again.
    if (viewport_redirect && viewport_sampled &&
        !FrameCapture::IsDisplayBuffer(texture_cache.GetImage(color).info.guest_address)) {
        viewport_redirect = false;
    }
    if (ui_phase && color == ui_color) {
        target = {Mirror(vk::Image(ui_image), ui_views, view_info.format, {}),
                  vk::ImageLayout::eGeneral, ui_width, ui_height, true};
        return true;
    }
    const auto& image = texture_cache.GetImage(color);
    const VAddr address = image.info.guest_address;
    if (!FrameCapture::IsDisplayBuffer(address)) {
        display_redirect = false;
        return false;
    }
    std::scoped_lock lock{display_mutex};
    auto& display = displays[address];
    if (!display_redirect) {
        display.valid = false; // not upscaled: the presenter shows the guest buffer
        return false;
    }
    if (!display.image || display.format != image.info.pixel_format ||
        display.width != ui_width || display.height != ui_height) {
        const auto device = instance.GetDevice();
        scheduler.Finish();
        display.views.clear();
        std::printf("Display: host buffer %ux%u (live)\n", ui_width, ui_height);
        display.format = image.info.pixel_format;
        display.width = ui_width;
        display.height = ui_height;
        display.image = VideoCore::UniqueImage(device, instance.GetAllocator());
        display.image.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = display.format,
            .extent = {ui_width, ui_height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                     vk::ImageUsageFlagBits::eTransferSrc,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        display.views.clear();
    }
    scheduler.EndRendering();
    // The first pass of a frame starts it afresh; later ones (Diablo III's HUD after its
    // display copy) draw over what is there.
    const bool first_pass = display.drawn_frame != frame_count;
    display.drawn_frame = frame_count;
    const vk::ImageMemoryBarrier2 b{
        .srcStageMask = first_pass ? vk::PipelineStageFlagBits2::eAllCommands
                                   : vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = first_pass ? vk::AccessFlagBits2::eNone
                                    : vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eColorAttachmentRead,
        .oldLayout = first_pass ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(display.image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    scheduler.Record([b](vk::CommandBuffer cmd) {
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
    });
    display.valid = true;
    target = {Mirror(vk::Image(display.image), display.views, view_info.format, {}),
              vk::ImageLayout::eGeneral, ui_width, ui_height};
    return true;
}

bool TemporalUpscaler::RedirectDepth(VideoCore::ImageId depth, Target& target) {
    // Diablo III: the HUD's own depth, drawn with the redirected display buffer.
    if (display_redirect && d3_display && depth && depth != camera_motion.GeometryDepth() &&
        depth != camera_motion.Depth()) {
        const auto& image = texture_cache.GetImage(depth);
        static int told = 0;
        if (told < 3 && std::getenv("SANCTUARY_CAMERA_TRACE")) {
            ++told;
            std::printf("Upscaler: HUD depth %ux%u samples %u %s (display %ux%u, ui %s)\n",
                        image.info.size.width, image.info.size.height, image.info.guest_samples,
                        vk::to_string(image.info.pixel_format).c_str(), display_size[0],
                        display_size[1], vk::to_string(ui_depth_format).c_str());
        }
        if (image.info.guest_samples == 1 && image.info.size.width == display_size[0] &&
            image.info.size.height == display_size[1] &&
            image.info.pixel_format == ui_depth_format) {
            target = {*ui_depth_view, vk::ImageLayout::eGeneral, ui_width, ui_height, false};
            return true;
        }
    }
    if (!ui_phase || depth != ui_depth) {
        return false;
    }
    target = {*ui_depth_view, vk::ImageLayout::eGeneral, ui_width, ui_height, true};
    return true;
}

bool TemporalUpscaler::RedirectSampled(VideoCore::ImageId image,
                                       const VideoCore::ImageViewInfo& info, vk::ImageView& view,
                                       vk::ImageLayout& layout) {
    if (viewport_redirect && image == ui_color && viewport_target) {
        // Written by the merge pass, with a barrier to shader reads (RunDisplayUpscale).
        view = Mirror(vk::Image(viewport_target->image), viewport_target->views, info.format,
                      info.mapping);
        layout = vk::ImageLayout::eGeneral;
        viewport_sampled = true;
        return true;
    }
    if (!display_redirect || image != ui_color) {
        return false;
    }
    if (!ui_read_barrier) {
        ui_read_barrier = true;
        scheduler.EndRendering();
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = vk::Image(ui_image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        scheduler.Record([b](vk::CommandBuffer cmd) {
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        });
    }
    view = Mirror(vk::Image(ui_image), ui_views, info.format, info.mapping);
    static int told = 0;
    if (told < 2 && std::getenv("SANCTUARY_CAMERA_TRACE")) {
        ++told;
        std::printf("Upscaler: display copy reads the upscaled frame as %s\n",
                    vk::to_string(info.format).c_str());
    }
    layout = vk::ImageLayout::eGeneral;
    return true;
}

void TemporalUpscaler::NoteFrameGenInputs(u32 w, u32 h, float frame_ms, bool was_reset) {
    const float sign = SanctuaryToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
    // Dynamic resolution: depth and vectors cover the scene's viewport; the display copy
    // stretches that part over the whole frame.
    const auto rect = camera_motion.SceneRect(w, h);
    frame_gen_inputs = {
        .valid = true,
        .depth = camera_motion.Depth(),
        .motion = vk::Image(motion_image),
        .width = rect[0],
        .height = rect[1],
        .target_width = w,
        .target_height = h,
        .jitter = {sign * jitter[0], sign * jitter[1]},
        .near_plane = camera_motion.Near(),
        .far_plane = camera_motion.Far(),
        .fov = camera_motion.VerticalFov(),
        .frame_ms = frame_ms > 0.0f ? frame_ms : 16.6f,
        .reset = was_reset,
    };
}

bool TemporalUpscaler::DisplayHostImage(VAddr address, Display& display) {
    std::scoped_lock lock{display_mutex};
    const auto it = displays.find(address);
    if (it == displays.end() || !it->second.valid || it->second.drawn_frame != frame_count) {
        return false;
    }
    display = {vk::Image(it->second.image), it->second.format, it->second.width, it->second.height};
    return true;
}

bool TemporalUpscaler::DisplayOverride(VAddr address, Display& display) {
    std::scoped_lock lock{display_mutex};
    const auto it = displays.find(address);
    if (it == displays.end() || !it->second.valid) {
        return false;
    }
    display = {vk::Image(it->second.image), it->second.format, it->second.width, it->second.height};
    // Diagnostic capture of the actual completed display buffer, including native UI.
    // SANCTUARY_PRESENT_DUMP_COUNT=N: the trigger dumps N consecutive frames (temporal stability).
    static const char* dump = std::getenv("SANCTUARY_PRESENT_DUMP_TRIGGER");
    static const int dump_count = [] {
        const char* env = std::getenv("SANCTUARY_PRESENT_DUMP_COUNT");
        return env ? std::max(1, std::atoi(env)) : 1;
    }();
    static int dump_index = 0;
    static int dump_remaining = 0;
    if (dump && dump_remaining == 0 && std::remove(dump) == 0) {
        dump_remaining = dump_count;
    }
    if (dump_remaining > 0) {
        --dump_remaining;
        scheduler.EndRendering();
        const bool bgra = display.format == vk::Format::eB8G8R8A8Unorm ||
                          display.format == vk::Format::eB8G8R8A8Srgb;
        DumpImages(instance, scheduler, scheduler.CommandBuffer(), dump_index++,
                   {{display.image, display.width, display.height, 4, "present",
                     bgra ? "bgra" : "rgba"}});
    }
    return true;
}

} // namespace Vulkan

namespace Vulkan {

bool TemporalUpscaler::UseFsr4() const {
    const int selected = SanctuarySettings::Get().upscaler;
    const bool supported = selected == SanctuarySettings::UpscalerFsr411
                               ? instance.IsFsr411Supported()
                               : instance.IsFsr4Int8Supported();
    return SanctuarySettings::IsFsr4(selected) && supported && !fsr4_failed;
}

bool TemporalUpscaler::RecordFsr4(vk::CommandBuffer cmdbuf, Fsr4Upscaler::Image color,
                                  Fsr4Upscaler::Image depth, u32 w, u32 h, u32 ow, u32 oh,
                                  float frame_ms) {
    const auto& settings = SanctuarySettings::Get();
    // Same jitter convention as FSR 3; the menu (or toggle 1 << 26) flips it for tests.
    const float sign =
        SanctuaryToggle::Disabled(1u << 26) != settings.fsr4_invert_jitter.load() ? -1.0f : 1.0f;
    const bool ok = fsr4->Record({
        .cmdbuf = cmdbuf,
        .color = color,
        .depth = depth,
        .motion = {vk::Image(motion_image), *motion_view, w, h},
        .output = {vk::Image(output_image), *output_view, ow, oh},
        .render_width = w,
        .render_height = h,
        .preset = applied_preset,
        .jitter = {sign * jitter[0], sign * jitter[1]},
        .frame_ms = frame_ms,
        .near_plane = camera_motion.Near(),
        .far_plane = 3000.0f,
        .vertical_fov = camera_motion.VerticalFov(),
        .sharpness = std::min(settings.sharpness.load(), 1.0f),
        .sharpen = settings.sharpen,
        .reset = reset,
        .auto_exposure = settings.fsr4_auto_exposure,
    });
    // The menu shows the reason; it outlives this frame (FSR 4 keeps its last message).
    static std::string shown;
    const char* problem = fsr4->Problem();
    if (!problem) {
        SanctuarySettings::Get().fsr4_problem = nullptr;
    } else if (shown != problem) {
        shown = problem;
        static std::array<std::string, 8> kept;
        static u32 next = 0;
        kept[next] = shown;
        SanctuarySettings::Get().fsr4_problem = kept[next].c_str();
        next = (next + 1) % kept.size();
    }
    if (!ok && fsr4->Fatal()) {
        std::printf("Upscaler: falling back to FSR 3.1\n");
        SanctuarySettings::Get().upscaler = SanctuarySettings::UpscalerFsr3;
        fsr4_failed = true; // EnsureResources creates the FSR 3 context next frame
        reset = true;
    }
    return ok;
}

bool TemporalUpscaler::UseDlss() const {
    const Dlss* dlss = Dlss::Get();
    return SanctuarySettings::Get().upscaler == SanctuarySettings::UpscalerDlss && dlss && dlss->Available() &&
           !dlss_failed;
}

bool TemporalUpscaler::RecordDlss(vk::CommandBuffer cmdbuf, const Dlss::Resource& color,
                                  const Dlss::Resource& depth, u32 w, u32 h, u32 ow, u32 oh,
                                  float frame_ms, bool hdr) {
    Dlss* dlss = Dlss::Get();
    const Dlss::FeatureDesc desc{w, h, ow, oh, Dlss::QualityForScale(float(ow) / float(w)), hdr};
    bool ok = true;
    if (!dlss->HasFeature(desc)) {
        // The previous feature may still be in use by submitted work; `cmdbuf` stays open.
        scheduler.WaitSubmitted();
        dlss->ReleaseFeature();
        ok = dlss->CreateFeature(cmdbuf, desc);
        reset = true;
    }
    if (ok) {
        // The jitter and motion vectors FSR 3 gets: render pixels, current to previous.
        const float sign = SanctuaryToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
        const auto& settings = SanctuarySettings::Get();
        ok = dlss->Evaluate(cmdbuf, {
            .color = color,
            .depth = depth,
            .motion = {vk::Image(motion_image), *motion_view, vk::Format::eR16G16Sfloat,
                       vk::ImageAspectFlagBits::eColor, w, h},
            .output = {vk::Image(output_image), *output_view, vk::Format::eR16G16B16A16Sfloat,
                       vk::ImageAspectFlagBits::eColor, ow, oh},
            .jitter_x = sign * jitter[0],
            .jitter_y = sign * jitter[1],
            .reset = reset,
            .frame_ms = frame_ms,
            .sharpness = settings.sharpen ? std::min(settings.sharpness.load(), 1.0f) : 0.0f,
        });
    }
    if (!ok) {
        std::printf("Upscaler: DLSS failed; falling back to FSR 3.1\n");
        SanctuarySettings::Get().upscaler = SanctuarySettings::UpscalerFsr3;
        dlss_failed = true; // EnsureResources creates the FSR 3 context next frame
        reset = true;
    }
    return ok;
}

} // namespace Vulkan
