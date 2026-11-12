// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_generation.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <vk_mem_alloc.h>
#include "ffx_vk_portable.h"
#include "sanctuary_settings.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/host_shaders/depth_copy_comp.h"
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

void Barrier(vk::CommandBuffer cmdbuf, vk::Image image, vk::ImageLayout old_layout,
             vk::ImageLayout new_layout, vk::PipelineStageFlags2 src_stage,
             vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
             vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 b{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
}

// Frame interpolation writes its output as a storage image: an 8-bit RGBA target is supported
// everywhere (BGRA storage is not); the presenter's blit converts it.
constexpr vk::Format GeneratedFormat = vk::Format::eR8G8B8A8Unorm;
constexpr vk::ImageUsageFlags FrameUsage =
    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
    vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
constexpr vk::ImageUsageFlags GeneratedUsage = vk::ImageUsageFlagBits::eStorage |
                                               vk::ImageUsageFlagBits::eSampled |
                                               vk::ImageUsageFlagBits::eTransferSrc;

} // namespace

FrameGeneration::FrameGeneration(const Instance& instance_, Scheduler& scheduler_,
                                 VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_}, runtime{runtime_} {
    const auto& settings = SanctuarySettings::Get();
    if (!settings.frame_generation) {
        return;
    }
    if (settings.upscaler != SanctuarySettings::UpscalerFsr3) {
        std::printf("Frame generation: needs temporal_aa=fsr3, off\n");
        return;
    }
    FfxVkPortableDeviceCapabilities caps{};
    caps.structSize = sizeof(caps);
    if (ffxVkPortableQueryDeviceCapabilities(instance.GetPhysicalDevice(), &caps) !=
            FFX_VK_PORTABLE_OK ||
        !caps.fsr3FrameGenerationPrerequisites) {
        std::printf("Frame generation: not supported by this GPU, off\n");
        return;
    }
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {.binding = 0, .descriptorType = vk::DescriptorType::eSampledImage, .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1, .descriptorType = vk::DescriptorType::eStorageImage, .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    copy_desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    copy_pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*copy_desc_layout,
    }));
    const auto module = CompileSPV(DEPTH_COPY_COMP, device);
    copy_pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *copy_pipeline_layout,
            }));
    device.destroyShaderModule(module);
    enabled = true;
    std::printf("Frame generation: on (experimental)\n");
}

FrameGeneration::~FrameGeneration() {
    scheduler.Finish();
    DestroyContext();
    for (auto& frame : generated) {
        if (frame) {
            FreeFrame(*frame);
        }
    }
    if (hudless_frame) {
        FreeFrame(*hudless_frame);
    }
}

void FrameGeneration::DestroyContext() {
    if (context) {
        ffxVkPortableFrameGenerationContextDestroy(context);
        context = nullptr;
    }
    history = false;
}

void FrameGeneration::MakeFrame(Frame& frame, u32 width, u32 height, vk::Format format,
                                vk::ImageUsageFlags usage) {
    const auto device = instance.GetDevice();
    const VkImageCreateInfo image_info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = usage,
    });
    const VmaAllocationCreateInfo alloc_info{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage image{};
    if (vmaCreateImage(instance.GetAllocator(), &image_info, &alloc_info, &image,
                       &frame.allocation, nullptr) != VK_SUCCESS) {
        failed = true;
        std::printf("Frame generation: image allocation failed, off\n");
        return;
    }
    frame.image = vk::Image(image);
    frame.image_view = Check(device.createImageView({
        .image = frame.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    if (!frame.present_done) {
        frame.present_done = Check(device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
    }
    frame.width = width;
    frame.height = height;
    frame.format = format;
}

void FrameGeneration::FreeFrame(Frame& frame) {
    const auto device = instance.GetDevice();
    if (frame.image_view) {
        device.destroyImageView(frame.image_view);
        frame.image_view = nullptr;
    }
    if (frame.image) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        frame.image = nullptr;
    }
    if (frame.present_done) {
        device.destroyFence(frame.present_done);
        frame.present_done = nullptr;
    }
}

FrameGeneration::Slot* FrameGeneration::FindSlot(VAddr address) {
    for (auto& slot : slots) {
        if (slot.address == address) {
            return &slot;
        }
    }
    return nullptr;
}

namespace {

void MakeImage(const Instance& instance, VideoCore::UniqueImage& image, vk::Format format,
               u32 width, u32 height, vk::ImageUsageFlags usage) {
    image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    image.Create(vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
}

} // namespace

void FrameGeneration::BeginFrame(VAddr address, const TemporalUpscaler::FrameGenInputs& inputs) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{slot_mutex};
    Slot* slot = FindSlot(address);
    if (!slot) {
        slot = &slots[0];
        for (auto& candidate : slots) {
            if (candidate.stamp < slot->stamp) {
                slot = &candidate;
            }
        }
    }
    slot->address = address;
    slot->stamp = ++slot_stamp;
    slot->has_inputs = false;
    slot->has_hudless = false;
    if (!inputs.valid || !inputs.depth || !inputs.motion) {
        return;
    }
    auto& depth = texture_cache.GetImage(inputs.depth);
    const vk::Format depth_format = depth.info.pixel_format;
    // Sized for the scene's targets: dynamic resolution changes only the part in use.
    const u32 w = inputs.target_width, h = inputs.target_height;
    if (!w || !h || inputs.width > w || inputs.height > h) {
        return;
    }
    if (depth.info.size.width < w || depth.info.size.height < h) {
        return;
    }
    if (!slot->depth || slot->depth_w != w || slot->depth_h != h) {
        scheduler.Finish();
        slot->depth_view.reset();
        MakeImage(instance, slot->depth, vk::Format::eR32Sfloat, w, h,
                  vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);
        MakeImage(instance, slot->motion, vk::Format::eR16G16Sfloat, w, h,
                  vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled |
                      vk::ImageUsageFlagBits::eStorage);
        slot->depth_format = vk::Format::eR32Sfloat;
        slot->depth_w = w;
        slot->depth_h = h;
    }
    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    const auto device = instance.GetDevice();
    const vk::ImageView depth_view = Check(device.createImageView({
        .image = depth.GetImage(),
        .viewType = vk::ImageViewType::e2D,
        .format = depth_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));
    if (!slot->depth_view) {
        slot->depth_view = Check(device.createImageViewUnique({
            .image = vk::Image(slot->depth),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR32Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    }
    const vk::ImageView depth_out = *slot->depth_view;
    const vk::Pipeline pipeline = *copy_pipeline;
    const vk::PipelineLayout layout = *copy_pipeline_layout;
    const vk::Image motion_src = inputs.motion;
    const vk::Image depth_dst = vk::Image(slot->depth), motion_dst = vk::Image(slot->motion);
    // On the recording thread, right after the FSR dispatch that read them.
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const auto all = vk::PipelineStageFlagBits2::eAllCommands;
        const auto transfer = vk::PipelineStageFlagBits2::eTransfer;
        const auto barrier = [&](vk::Image image, vk::ImageAspectFlags aspect,
                                 vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                                 vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
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
        // The motion vectors: written by the camera pass and read by FSR (General).
        const vk::MemoryBarrier2 written{
            .srcStageMask = all,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = transfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});
        // The copies: their last flip's frame interpolation read them.
        barrier(depth_dst, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
                vk::ImageLayout::eGeneral, all, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);
        barrier(motion_dst, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
                vk::ImageLayout::eTransferDstOptimal, all, vk::AccessFlagBits2::eNone, transfer,
                vk::AccessFlagBits2::eTransferWrite);
        const vk::DescriptorImageInfo in_info{.imageView = depth_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
        const vk::DescriptorImageInfo out_info{.imageView = depth_out,
                                               .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 2> writes{{
            {.dstBinding = 0, .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage, .pImageInfo = &in_info},
            {.dstBinding = 1, .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &out_info},
        }};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        cmdbuf.copyImage(motion_src, vk::ImageLayout::eGeneral, motion_dst,
                         vk::ImageLayout::eTransferDstOptimal,
                         vk::ImageCopy{
                             .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                             .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                             .extent = {w, h, 1}});
        barrier(depth_dst, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
                vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                vk::AccessFlagBits2::eShaderWrite, all, vk::AccessFlagBits2::eShaderRead);
        barrier(motion_dst, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eGeneral, transfer, vk::AccessFlagBits2::eTransferWrite, all,
                vk::AccessFlagBits2::eShaderRead);
        // The camera pass of the next frame writes the motion image again.
        const vk::MemoryBarrier2 read{
            .srcStageMask = transfer | vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = all,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &read});
    });
    scheduler.DeferOperation([device, depth_view] { device.destroyImageView(depth_view); });
    slot->inputs = inputs;
    slot->has_inputs = true;
}

void FrameGeneration::CaptureHudless(VAddr address, vk::Image image, vk::ImageLayout layout,
                                     vk::Format format, u32 width, u32 height) {
    if (!Enabled()) {
        return;
    }
    std::scoped_lock lock{slot_mutex};
    Slot* slot = FindSlot(address);
    if (!slot || !slot->has_inputs) {
        return;
    }
    if (!slot->hudless || slot->hudless_w != width || slot->hudless_h != height ||
        slot->hudless_format != format) {
        scheduler.Finish();
        slot->hudless_view.reset();
        MakeImage(instance, slot->hudless, format, width, height,
                  vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled);
        slot->hudless_w = width;
        slot->hudless_h = height;
        slot->hudless_format = format;
    }
    scheduler.EndRendering();
    const vk::Image target = vk::Image(slot->hudless);
    // On the recording thread, in order with the display copy before it.
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const auto all = vk::PipelineStageFlagBits2::eAllCommands;
        const auto transfer = vk::PipelineStageFlagBits2::eTransfer;
        if (layout == vk::ImageLayout::eGeneral) {
            // The host display buffer: wait for the display copy's writes.
            const vk::MemoryBarrier2 written{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .dstStageMask = transfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});
        }
        // The last flip of this display buffer read it.
        Barrier(cmdbuf, target, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                all, vk::AccessFlagBits2::eNone, transfer, vk::AccessFlagBits2::eTransferWrite);
        cmdbuf.copyImage(image, layout, target, vk::ImageLayout::eTransferDstOptimal,
                         vk::ImageCopy{
                             .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                             .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                             .extent = {width, height, 1},
                         });
        Barrier(cmdbuf, target, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal, transfer,
                vk::AccessFlagBits2::eTransferWrite, all, vk::AccessFlagBits2::eShaderRead);
        if (layout == vk::ImageLayout::eGeneral) {
            // The HUD then draws over the host display buffer.
            const vk::MemoryBarrier2 read{
                .srcStageMask = transfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                                 vk::AccessFlagBits2::eColorAttachmentRead,
            };
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &read});
        }
    });
    slot->has_hudless = true;
}

bool FrameGeneration::HudlessSource(VAddr address, vk::Format view_format, vk::ImageView& view,
                                    vk::Extent2D& size) {
    if (!Enabled()) {
        return false;
    }
    std::scoped_lock lock{slot_mutex};
    Slot* slot = FindSlot(address);
    if (!slot || !slot->has_inputs || !slot->has_hudless) {
        ++skipped[!slot ? 5 : !slot->has_inputs ? 6 : 7];
        return false;
    }
    if (!slot->hudless_view || slot->hudless_view_format != view_format) {
        slot->hudless_view = Check(instance.GetDevice().createImageViewUnique({
            .image = vk::Image(slot->hudless),
            .viewType = vk::ImageViewType::e2D,
            .format = view_format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        slot->hudless_view_format = view_format;
    }
    view = *slot->hudless_view;
    size = vk::Extent2D{slot->hudless_w, slot->hudless_h};
    return true;
}

void FrameGeneration::Skip(VAddr address) {
    // A flip without this frame's data (the guest flipped a buffer again, or before its HUD):
    // shown as it is. The interpolation history stays: the next frame interpolates from the
    // last one it saw.
    std::scoped_lock lock{slot_mutex};
    if (Slot* slot = FindSlot(address)) {
        slot->has_inputs = slot->has_hudless = false;
    }
}

Frame* FrameGeneration::HudlessFrame(u32 width, u32 height, vk::Format format) {
    if (!hudless_frame) {
        hudless_frame = std::make_unique<Frame>();
    }
    auto& frame = *hudless_frame;
    if (!frame.image || frame.width != width || frame.height != height || frame.format != format) {
        // The presenter is recording into the scheduler's command buffer: wait for the GPU
        // without submitting it.
        (void)instance.GetDevice().waitIdle();
        if (frame.image) {
            FreeFrame(frame);
        }
        MakeFrame(frame, width, height, format, FrameUsage);
    }
    return failed ? nullptr : &frame;
}

bool FrameGeneration::EnsureContext(u32 render_w, u32 render_h, u32 display_w, u32 display_h,
                                    vk::Format source_format) {
    const std::array<u32, 4> size{render_w, render_h, display_w, display_h};
    if (context && size == context_size && source_format == context_format) {
        return true;
    }
    (void)instance.GetDevice().waitIdle(); // as in HudlessFrame: no submission mid-recording
    DestroyContext();
    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = instance.GetInstance();
    device_info.physicalDevice = instance.GetPhysicalDevice();
    device_info.device = instance.GetDevice();
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = instance.GetGraphicsQueue();
    device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = instance.IsShaderFloat16Enabled();
    device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
    device_info.synchronization2Enabled = VK_TRUE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;
    FfxVkPortableFrameGenerationCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.flags = 0; // render-size, unjittered vectors; depth 0 near .. 1 far
    create_info.maxRenderSize = {render_w, render_h};
    create_info.displaySize = {display_w, display_h};
    create_info.interpolationSourceFormat = static_cast<VkFormat>(source_format);
    create_info.outputFormat = static_cast<VkFormat>(GeneratedFormat);
    const auto result =
        ffxVkPortableFrameGenerationContextCreate(&device_info, &create_info, &context);
    if (result != FFX_VK_PORTABLE_OK) {
        std::printf("Frame generation: context creation failed (%d), off\n", int(result));
        context = nullptr;
        failed = true;
        return false;
    }
    context_size = size;
    context_format = source_format;
    std::printf("Frame generation: %ux%u -> %ux%u\n", render_w, render_h, display_w, display_h);
    for (auto& frame : generated) {
        if (!frame) {
            frame = std::make_unique<Frame>();
        }
        if (!frame->image || frame->width != display_w || frame->height != display_h) {
            if (frame->image) {
                FreeFrame(*frame);
            }
            MakeFrame(*frame, display_w, display_h, GeneratedFormat, GeneratedUsage);
        }
    }
    return !failed;
}

Frame* FrameGeneration::Generate(vk::CommandBuffer cmdbuf, const Frame& real, VAddr address) {
    std::unique_lock slot_lock{slot_mutex};
    Slot* slot = FindSlot(address);
    const bool have_inputs = slot && slot->has_inputs;
    const bool have_hudless = have_inputs && slot->has_hudless;
    if (slot) {
        slot->has_inputs = slot->has_hudless = false; // used once
    }
    if (!Enabled() || !have_hudless || !hudless_frame ||
        hudless_frame->width != real.width || hudless_frame->height != real.height) {
        ++skipped[!have_inputs ? 1 : !have_hudless ? 0 : 2];
        history = false;
        return nullptr;
    }
    const auto inputs = slot->inputs;
    const vk::Image depth_image = vk::Image(slot->depth), motion_image = vk::Image(slot->motion);
    const vk::Format depth_format = slot->depth_format;
    slot_lock.unlock();
    if (!EnsureContext(inputs.target_width, inputs.target_height, real.width, real.height,
                       real.format)) {
        return nullptr;
    }
    // A generated frame is written again only once the presenter showed it.
    const size_t index = next_generated;
    next_generated = (next_generated + 1) % GeneratedFrames;
    {
        std::unique_lock lock{pending_mutex};
        pending_cv.wait(lock, [&] { return !pending[index]; });
    }
    Frame& out = *generated[index];
    (void)instance.GetDevice().waitForFences(out.present_done, VK_TRUE, UINT64_MAX);

    const bool reset = inputs.reset || !history;
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    Barrier(cmdbuf, out.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, all,
            vk::AccessFlagBits2::eNone, all, rw);
    // The presenter's passes wrote the real and HUD-less frames.
    const vk::MemoryBarrier2 written{
        .srcStageMask = all,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = all,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});

    FfxVkPortableFrameGenerationPrepareInfo prepare{};
    prepare.structSize = sizeof(prepare);
    prepare.commandBuffer = cmdbuf;
    prepare.depth = Describe(depth_image, depth_format, inputs.target_width, inputs.target_height,
                             vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                             vk::ImageAspectFlagBits::eColor,
                             FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    prepare.motionVectors = Describe(motion_image, vk::Format::eR16G16Sfloat,
                                     inputs.target_width, inputs.target_height,
                                     vk::ImageUsageFlagBits::eStorage |
                                         vk::ImageUsageFlagBits::eSampled |
                                         vk::ImageUsageFlagBits::eTransferDst,
                                     vk::ImageAspectFlagBits::eColor,
                                     FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    prepare.renderSize = {inputs.width, inputs.height};
    prepare.jitterOffset = {inputs.jitter[0], inputs.jitter[1]};
    prepare.motionVectorScale = {1.0f, 1.0f};
    prepare.frameTimeMilliseconds = inputs.frame_ms;
    prepare.cameraNear = inputs.near_plane;
    prepare.cameraFar = inputs.far_plane;
    prepare.cameraVerticalFovRadians = inputs.fov;
    prepare.viewSpaceToMeters = 1.0f;
    prepare.minLuminance = 0.0f;
    prepare.maxLuminance = 1.0f;
    prepare.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    prepare.cameraUp = {0.0f, 1.0f, 0.0f};
    prepare.cameraRight = {1.0f, 0.0f, 0.0f};
    prepare.cameraForward = {0.0f, 0.0f, 1.0f};
    prepare.reset = reset ? VK_TRUE : VK_FALSE;
    prepare.frameId = frame_id;
    const auto hudless_image =
        Describe(hudless_frame->image, real.format, real.width, real.height, FrameUsage,
                 vk::ImageAspectFlagBits::eColor, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    if (ffxVkPortableFrameGenerationContextPrepare(context, &prepare, &hudless_image) !=
        FFX_VK_PORTABLE_OK) {
        std::printf("Frame generation: prepare failed, off\n");
        failed = true;
        return nullptr;
    }
    FfxVkPortableFrameGenerationDispatchInfo dispatch{};
    dispatch.structSize = sizeof(dispatch);
    dispatch.commandBuffer = cmdbuf;
    dispatch.currentColor =
        Describe(real.image, real.format, real.width, real.height, FrameUsage,
                 vk::ImageAspectFlagBits::eColor, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    dispatch.hudlessColor = hudless_image;
    dispatch.distortionField.structSize = sizeof(dispatch.distortionField);
    dispatch.output = Describe(out.image, GeneratedFormat, real.width, real.height, GeneratedUsage,
                               vk::ImageAspectFlagBits::eColor,
                               FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.displaySize = {real.width, real.height};
    dispatch.interpolationRect = {0, 0, real.width, real.height};
    dispatch.frameTimeMilliseconds = inputs.frame_ms;
    dispatch.cameraNear = inputs.near_plane;
    dispatch.cameraFar = inputs.far_plane;
    dispatch.cameraVerticalFovRadians = inputs.fov;
    dispatch.viewSpaceToMeters = 1.0f;
    dispatch.minLuminance = 0.0f;
    dispatch.maxLuminance = 1.0f;
    dispatch.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    dispatch.reset = reset ? VK_TRUE : VK_FALSE;
    dispatch.frameId = frame_id;
    if (ffxVkPortableFrameGenerationContextRecordDispatch(context, &dispatch) !=
        FFX_VK_PORTABLE_OK) {
        std::printf("Frame generation: dispatch failed, off\n");
        failed = true;
        return nullptr;
    }
    ++frame_id;
    history = true;
    // Back to General for the presenter's blit (it reads frames in General).
    const vk::MemoryBarrier2 done{
        .srcStageMask = all,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = all,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
    if (reset) {
        // The first frame of a history has nothing to interpolate from.
        ++skipped[inputs.reset ? 3 : 4];
        return nullptr;
    }
    {
        std::scoped_lock lock{pending_mutex};
        pending[index] = true;
    }
    out.is_hdr = false;
    return &out;
}

void FrameGeneration::OnPresented(Frame* frame) {
    // SANCTUARY_FRAME_STATS=1: generated frames shown per second, every 5 seconds.
    static const bool stats = std::getenv("SANCTUARY_FRAME_STATS") != nullptr;
    if (stats) {
        using Clock = std::chrono::steady_clock;
        static auto start = Clock::now();
        static u32 count = 0;
        ++count;
        const double s = std::chrono::duration<double>(Clock::now() - start).count();
        if (s >= 5.0) {
            std::printf("Frame generation: %.1f generated frames/s; not generated: %u no HUD-less, "
                        "%u no FSR, %u other, %u FSR reset, %u new history; at the flip: %u no slot, "
                        "%u no inputs, %u no HUD-less\n",
                        count / s, skipped[0], skipped[1], skipped[2], skipped[3], skipped[4],
                        skipped[5], skipped[6], skipped[7]);
            skipped = {};
            count = 0;
            start = Clock::now();
        }
    }
    std::scoped_lock lock{pending_mutex};
    for (size_t i = 0; i < GeneratedFrames; ++i) {
        if (generated[i].get() == frame) {
            pending[i] = false;
        }
    }
    pending_cv.notify_all();
}

} // namespace Vulkan
