// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_fxaa.h"

#include <array>
#include <cstdio>

#include "sanctuary_settings.h"
#include "video_core/host_shaders/fxaa_comp.h"
#include "video_core/host_shaders/fxaa_store_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

Fxaa::Fxaa(const Instance& instance_, Scheduler& scheduler_,
           VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_} {}

Fxaa::~Fxaa() = default;

void Fxaa::CreatePipelines() {
    const auto device = instance.GetDevice();
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = vk::DescriptorType::eStorageImage,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
    }));
    const auto compute = [&](const auto& code) {
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
    smooth_pipeline = compute(FXAA_COMP);
    store_pipeline = compute(FXAA_STORE_COMP);
}

void Fxaa::Run(VideoCore::ImageId frame) {
    const auto& settings = SanctuarySettings::Get();
    if (!frame || !settings.fxaa || settings.upscaler == SanctuarySettings::UpscalerFsr3) {
        return;
    }
    auto& image = texture_cache.GetImage(frame);
    if (image.info.pixel_format != vk::Format::eA2B10G10R10UnormPack32 ||
        !(image.usage_flags & vk::ImageUsageFlagBits::eStorage)) {
        if (!logged) {
            logged = true;
            std::printf("FXAA: skips frames in %s or without storage use (menus)\n",
                        vk::to_string(image.info.pixel_format).c_str());
        }
        return;
    }
    if (!smooth_pipeline) {
        CreatePipelines();
    }
    const auto device = instance.GetDevice();
    const u32 w = image.info.size.width, h = image.info.size.height;
    if (w != width || h != height) {
        scheduler.Finish(); // the old copy may still be in use; sizes change rarely
        smoothed_view.reset();
        smoothed = VideoCore::UniqueImage(device, instance.GetAllocator());
        smoothed.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eStorage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        smoothed_view = Check(device.createImageViewUnique({
            .image = vk::Image(smoothed),
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR16G16B16A16Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        width = w, height = h;
        std::printf("FXAA: on, %ux%u\n", w, h);
    }
    if (!frame_view || frame_image != image.GetImage() || frame_uid != image.image_uid) {
        if (frame_view) {
            scheduler.DeferOperation(
                [device, view = frame_view.release()] { device.destroyImageView(view); });
        }
        frame_image = image.GetImage();
        frame_uid = image.image_uid;
        frame_view = Check(device.createImageViewUnique({
            .image = frame_image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eA2B10G10R10UnormPack32,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    }
    scheduler.EndRendering();
    runtime.Transit(&image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    runtime.FlushBarriers();
    const vk::Image copy = vk::Image(smoothed);
    const vk::ImageView copy_view = *smoothed_view, frame_v = *frame_view;
    const vk::Pipeline smooth = *smooth_pipeline, store = *store_pipeline;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        const auto compute = vk::PipelineStageFlagBits2::eComputeShader;
        const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
        const vk::ImageMemoryBarrier2 to_general{
            .srcStageMask = compute,
            .srcAccessMask = rw,
            .dstStageMask = compute,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = copy,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_general});
        const auto pass = [&](vk::Pipeline pipeline, vk::ImageView first, vk::ImageView second) {
            const vk::DescriptorImageInfo a{.imageView = first, .imageLayout = vk::ImageLayout::eGeneral};
            const vk::DescriptorImageInfo b{.imageView = second, .imageLayout = vk::ImageLayout::eGeneral};
            const std::array<vk::WriteDescriptorSet, 2> writes{{
                {.dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &a},
                {.dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &b},
            }};
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, writes);
            cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        };
        // The frame into the smoothed copy, then the copy back into the frame.
        pass(smooth, frame_v, copy_view);
        const vk::MemoryBarrier2 between{
            .srcStageMask = compute, .srcAccessMask = rw, .dstStageMask = compute, .dstAccessMask = rw};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &between});
        pass(store, copy_view, frame_v);
    });
}

} // namespace Vulkan
