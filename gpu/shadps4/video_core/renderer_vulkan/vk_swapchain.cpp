// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "sanctuary_graphics.h"
#include "sanctuary_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

namespace Vulkan {

// sanctuary: VkSurfaceFullScreenExclusiveInfoEXT and VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT. The
// Vulkan headers declare them only with VK_USE_PLATFORM_WIN32_KHR (which would pull in windows.h
// here); the layout and values are fixed by the VK_EXT_full_screen_exclusive spec.
struct FullScreenExclusiveInfo {
    VkStructureType sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
    void* pNext = nullptr;
    int32_t fullScreenExclusive = 2; // VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT
};

// sanctuary: Borderless must stay a window on Windows. NVIDIA's driver promotes a Vulkan window
// that covers the monitor to exclusive fullscreen on its own (slow Alt+Tab, the screen flashes:
// GitHub #109); the swapchain now forbids that. Windowed asks the same (a maximized window can
// cover the screen too); Fullscreen leaves the driver's default, which is the exclusive mode.
static bool RefusesExclusiveFullscreen(const Instance& instance) {
    return instance.IsFullScreenExclusiveSupported() &&
           SanctuarySettings::Get().display_mode.load() != SanctuaryGraphics::Fullscreen;
}

static constexpr vk::SurfaceFormatKHR SURFACE_FORMAT_HDR = {
    .format = vk::Format::eA2B10G10R10UnormPack32,
    .colorSpace = vk::ColorSpaceKHR::eHdr10St2084EXT,
};

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window_)
    : instance{instance_}, window{window_}, surface{CreateSurface(instance.GetInstance(), window)} {
    FindPresentFormat();
    FindPresentMode();

    Create(window.GetWidth(), window.GetHeight());
    if (instance.IsFullScreenExclusiveSupported()) {
        std::printf("Swapchain: exclusive fullscreen %s\n", borderless ? "refused" : "allowed");
    }
    ImGui::Core::Initialize(instance, window, image_count, surface_format.format);
}

Swapchain::~Swapchain() {
    Destroy();
    instance.GetInstance().destroySurfaceKHR(surface);
}

void Swapchain::Create(u32 width_, u32 height_) {
    width = width_;
    height = height_;
    needs_recreation = false;

    Destroy();

    SetSurfaceProperties();

    const std::array queue_family_indices = {
        instance.GetGraphicsQueueFamilyIndex(),
        instance.GetPresentQueueFamilyIndex(),
    };

    const bool exclusive = queue_family_indices[0] == queue_family_indices[1];
    const u32 queue_family_indices_count = exclusive ? 1u : 2u;
    const vk::SharingMode sharing_mode =
        exclusive ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent;
    const auto format = needs_hdr ? SURFACE_FORMAT_HDR : surface_format;
    FullScreenExclusiveInfo no_exclusive{};
    borderless = RefusesExclusiveFullscreen(instance);
    const vk::SwapchainCreateInfoKHR swapchain_info = {
        .pNext = borderless ? &no_exclusive : nullptr,
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = sharing_mode,
        .queueFamilyIndexCount = queue_family_indices_count,
        .pQueueFamilyIndices = queue_family_indices.data(),
        .preTransform = transform,
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = true,
        .oldSwapchain = nullptr,
    };

    auto [swapchain_result, chain] = instance.GetDevice().createSwapchainKHR(swapchain_info);
    ASSERT_MSG(swapchain_result == vk::Result::eSuccess, "Failed to create swapchain: {}",
               vk::to_string(swapchain_result));
    swapchain = chain;

    SetupImages();
    RefreshSemaphores();
}

void Swapchain::Recreate(u32 width_, u32 height_) {
    LOG_DEBUG(Render_Vulkan, "Recreate the swapchain: width={} height={} HDR={}", width_, height_,
              needs_hdr);
    Create(width_, height_);
}

void Swapchain::SetHDR(bool hdr) {
    if (needs_hdr == hdr) {
        return;
    }

    auto result = instance.GetDevice().waitIdle();
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(ImGui, "Failed to wait for Vulkan device idle on mode change: {}",
                    vk::to_string(result));
    }

    needs_hdr = hdr;
    Recreate(width, height);
    ImGui::Core::OnSurfaceFormatChange(needs_hdr ? SURFACE_FORMAT_HDR.format
                                                 : surface_format.format);
}

namespace {
// SANCTUARY_ACQUIRE_FENCE=0: the acquire semaphore alone, as in 0.7.4 (diagnosis only: not safe on
// AMD's Windows driver, GitHub #110); =1: 0.7.5's order also without V-Sync (acquire when the frame
// is ready, then wait); otherwise without V-Sync the next image is acquired right after a present.
int ForcedAcquireFence() {
    static const int forced = [] {
        const char* env = std::getenv("SANCTUARY_ACQUIRE_FENCE");
        return env && (env[0] == '0' || env[0] == '1') ? env[0] - '0' : -1;
    }();
    return forced;
}

bool Acquired(vk::Result result) {
    return result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR;
}
} // namespace

vk::Result Swapchain::StartAcquire() {
    // sanctuary: the acquire also signals a fence, and this (present) thread waits for it on the
    // CPU before the blit that waits on image_acquired is submitted (GitHub #103, #110).
    // #103: everything runs on one queue, and a driver that signals the semaphore only when the
    // display lets go of the image (V-Sync) stalled the game's own GPU work behind the blit's wait.
    // #110: AMD's driver (PAL: SwapChain::AcquireNextImage, PresentScheduler::SignalOnAcquire,
    // Queue::WaitQueueSemaphoreInternal) signals the semaphore from an internal queue only after the
    // image's previous present is done. A queue that waits on a semaphore whose signal is not
    // submitted yet is "stalled": the driver keeps every later submission of that queue (all of the
    // game's GPU work here) in a software batch that another driver thread submits later. 0.7.6
    // skipped the fence wait without V-Sync, so at 200 FPS (Immediate) the graphics queue went
    // through that path while the pipeline warm-up ran, and amdvlk64.dll crashed. The fence is
    // signaled after the semaphore on that same internal queue: once it is, the semaphore's signal
    // has been submitted and the blit's wait never stalls the queue.
    // The semaphore must have no wait pending (VUID-vkAcquireNextImageKHR-semaphore-01779). The
    // frame fences used to guarantee that; an acquire made ahead comes one present earlier.
    if (const u64 tick = acquire_wait_ticks[frame_index];
        tick != 0 && acquire_wait_scheduler && !acquire_wait_scheduler->IsFree(tick)) {
        acquire_wait_scheduler->Wait(tick);
    }
    const vk::Device device = instance.GetDevice();
    const bool use_fence = ForcedAcquireFence() != 0;
    const vk::Fence fence = use_fence ? image_acquired_fences[frame_index] : vk::Fence{};
    return device.acquireNextImageKHR(swapchain, std::numeric_limits<u64>::max(),
                                      image_acquired[frame_index], fence, &image_index);
}

void Swapchain::FinishAcquire(vk::Result result) {
    if (ForcedAcquireFence() == 0 || !Acquired(result)) {
        return; // no fence, or nothing acquired: the fence is not signaled
    }
    const vk::Device device = instance.GetDevice();
    const vk::Fence fence = image_acquired_fences[frame_index];
    const auto wait_result = device.waitForFences(fence, true, std::numeric_limits<u64>::max());
    if (wait_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for an acquired swapchain image: {}",
                    vk::to_string(wait_result));
    }
    (void)device.resetFences(fence);
}

void Swapchain::AcquireAhead() {
    // Without V-Sync only (Mailbox, Immediate): with V-Sync the acquire can block until the next
    // refresh, and #103's frame pacing was measured with the acquire at the frame. Without it, the
    // CPU wait for the image right before the blit gave the present thread 26-42 ms stalls
    // (GitHub #106: earlier GPU or compositor work); acquired one frame earlier, that wait has the
    // whole frame interval to finish.
    if (acquired_ahead || needs_recreation || ForcedAcquireFence() >= 0 ||
        present_mode == vk::PresentModeKHR::eFifo ||
        present_mode == vk::PresentModeKHR::eFifoRelaxed) {
        return;
    }
    ahead_result = StartAcquire();
    acquired_ahead = true;
}

bool Swapchain::AcquireNextImage() {
    vk::Result result;
    if (acquired_ahead) {
        acquired_ahead = false;
        result = ahead_result;
    } else {
        result = StartAcquire();
    }
    FinishAcquire(result);

    switch (result) {
    case vk::Result::eSuccess:
        break;
    case vk::Result::eSuboptimalKHR:
    case vk::Result::eErrorSurfaceLostKHR:
    case vk::Result::eErrorOutOfDateKHR:
    case vk::Result::eErrorUnknown:
        needs_recreation = true;
        break;
    default:
        LOG_CRITICAL(Render_Vulkan, "Swapchain acquire returned unknown result {}",
                     vk::to_string(result));
        UNREACHABLE();
        break;
    }

    return !needs_recreation;
}

bool Swapchain::Present() {
    const vk::PresentInfoKHR present_info = {
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };

    auto result = instance.GetPresentQueue().presentKHR(present_info);
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        needs_recreation = true;
    } else {
        ASSERT_MSG(result == vk::Result::eSuccess, "Swapchain presentation failed: {}",
                   vk::to_string(result));
    }

    // sanctuary: the blit that waited on this frame's acquire semaphore was the last submission.
    if (acquire_wait_scheduler) {
        acquire_wait_ticks[frame_index] = acquire_wait_scheduler->CurrentTick() - 1;
    }
    frame_index = (frame_index + 1) % image_count;

    return !needs_recreation;
}

void Swapchain::FindPresentFormat() {
    const auto [formats_result, formats] =
        instance.GetPhysicalDevice().getSurfaceFormatsKHR(surface);
    ASSERT_MSG(formats_result == vk::Result::eSuccess, "Failed to query surface formats: {}",
               vk::to_string(formats_result));

    // Check if the device supports HDR formats. Here we care of Rec.2020 PQ only as it is expected
    // game output. Other variants as e.g. linear Rec.2020 will require additional color space
    // rotation
    supports_hdr =
        std::find_if(formats.begin(), formats.end(), [](const vk::SurfaceFormatKHR& format) {
            return format == SURFACE_FORMAT_HDR;
        }) != formats.end();
    // Also make sure that user allowed us to use HDR
    supports_hdr &= EmulatorSettings.IsHdrAllowed();

    // If there is a single undefined surface format, the device doesn't care, so we'll just use
    // RGBA sRGB.
    if (formats[0].format == vk::Format::eUndefined) {
        surface_format.format = vk::Format::eR8G8B8A8Unorm;
        surface_format.colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
        return;
    }

    // Try to find a suitable format.
    for (const vk::SurfaceFormatKHR& sformat : formats) {
        vk::Format format = sformat.format;
        if (format != vk::Format::eR8G8B8A8Unorm && format != vk::Format::eB8G8R8A8Unorm) {
            continue;
        }

        surface_format.format = format;
        surface_format.colorSpace = sformat.colorSpace;
        return;
    }

    UNREACHABLE_MSG("Unable to find required swapchain format!");
}

void Swapchain::FindPresentMode() {
    const auto [modes_result, modes] =
        instance.GetPhysicalDevice().getSurfacePresentModesKHR(surface);
    if (modes_result != vk::Result::eSuccess) {
        LOG_ERROR(Render, "Failed to query available present modes, falling back to Fifo as "
                          "guaranteed supported option.");
        present_mode = vk::PresentModeKHR::eFifo;
        return;
    }

    // sanctuary: VSync on is Fifo (no tearing, waits for the display); off is Mailbox when the
    // driver has it, else Immediate (tearing allowed), else Fifo.
    vsync = SanctuarySettings::Get().vsync;
    const auto has = [&](vk::PresentModeKHR mode) { return std::ranges::find(modes, mode) != modes.cend(); };
    present_mode = vsync ? vk::PresentModeKHR::eFifo
                   : has(vk::PresentModeKHR::eMailbox) ? vk::PresentModeKHR::eMailbox
                   : has(vk::PresentModeKHR::eImmediate) ? vk::PresentModeKHR::eImmediate
                                                         : vk::PresentModeKHR::eFifo;
    std::printf("Swapchain: VSync %s (%s)\n", vsync ? "on" : "off", vk::to_string(present_mode).c_str());
}

void Swapchain::UpdateVsync() {
    // sanctuary: a Display mode change to or from Fullscreen also rebuilds the swapchain, so the
    // exclusive-fullscreen permission follows it even when the window size stays the same.
    const bool vsync_changed = SanctuarySettings::Get().vsync != vsync;
    if (!vsync_changed && RefusesExclusiveFullscreen(instance) == borderless) {
        return;
    }
    if (const auto result = instance.GetDevice().waitIdle(); result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for the device before a VSync change: {}",
                    vk::to_string(result));
    }
    if (vsync_changed) {
        FindPresentMode();
    }
    Recreate(width, height);
    if (instance.IsFullScreenExclusiveSupported()) {
        std::printf("Swapchain: exclusive fullscreen %s\n", borderless ? "refused" : "allowed");
    }
}

void Swapchain::SetSurfaceProperties() {
    const auto [capabilities_result, capabilities] =
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    ASSERT_MSG(capabilities_result == vk::Result::eSuccess,
               "Failed to query surface capabilities: {}", vk::to_string(capabilities_result));

    extent = capabilities.currentExtent;
    if (capabilities.currentExtent.width == std::numeric_limits<u32>::max()) {
        extent.width = std::max(capabilities.minImageExtent.width,
                                std::min(capabilities.maxImageExtent.width, width));
        extent.height = std::max(capabilities.minImageExtent.height,
                                 std::min(capabilities.maxImageExtent.height, height));
    }

    // Select number of images in swap chain, we prefer one buffer in the background to work on
    image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    // Prefer identity transform if possible
    transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    if (!(capabilities.supportedTransforms & transform)) {
        transform = capabilities.currentTransform;
    }

    // Opaque is not supported everywhere.
    composite_alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    if (!(capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)) {
        composite_alpha = vk::CompositeAlphaFlagBitsKHR::eInherit;
    }
}

void Swapchain::Destroy() {
    vk::Device device = instance.GetDevice();
    if (acquired_ahead) {
        // sanctuary: the image stays acquired (destroying the swapchain releases it); its fence
        // must be signaled before the fence and semaphore are destroyed.
        acquired_ahead = false;
        FinishAcquire(ahead_result);
    }
    const auto wait_result = device.waitIdle();
    if (wait_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for device to become idle: {}",
                    vk::to_string(wait_result));
    }

    for (auto& image_view : images_view) {
        device.destroyImageView(image_view);
    }
    images_view.clear();

    if (swapchain) {
        device.destroySwapchainKHR(swapchain);
    }

    for (const auto& sem : image_acquired) {
        device.destroySemaphore(sem);
    }
    for (const auto& sem : present_ready) {
        device.destroySemaphore(sem);
    }

    for (const auto& fence : image_acquired_fences) {
        device.destroyFence(fence);
    }

    image_acquired.clear();
    present_ready.clear();
    image_acquired_fences.clear();
}

void Swapchain::RefreshSemaphores() {
    const vk::Device device = instance.GetDevice();
    image_acquired.resize(image_count);
    present_ready.resize(image_count);

    for (vk::Semaphore& semaphore : image_acquired) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create image acquired semaphore: {}",
                   vk::to_string(semaphore_result));
        semaphore = sem;
    }
    for (vk::Semaphore& semaphore : present_ready) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create present ready semaphore: {}", vk::to_string(semaphore_result));
        semaphore = sem;
    }

    // sanctuary: the image count can change with the swapchain; frame_index indexes these.
    frame_index = 0;
    acquire_wait_ticks.assign(image_count, 0);
    image_acquired_fences.resize(image_count);
    for (vk::Fence& fence : image_acquired_fences) {
        auto [fence_result, created] = device.createFence({});
        ASSERT_MSG(fence_result == vk::Result::eSuccess, "Failed to create image acquired fence: {}",
                   vk::to_string(fence_result));
        fence = created;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, image_acquired[i], "Swapchain Semaphore: image_acquired {}", i);
        SetObjectName(device, present_ready[i], "Swapchain Semaphore: present_ready {}", i);
    }
}

void Swapchain::SetupImages() {
    vk::Device device = instance.GetDevice();
    auto [images_result, imgs] = device.getSwapchainImagesKHR(swapchain);
    ASSERT_MSG(images_result == vk::Result::eSuccess, "Failed to create swapchain images: {}",
               vk::to_string(images_result));
    images = std::move(imgs);
    image_count = static_cast<u32>(images.size());
    images_view.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        if (images_view[i]) {
            device.destroyImageView(images_view[i]);
        }
        auto [im_view_result, im_view] = device.createImageView(vk::ImageViewCreateInfo{
            .image = images[i],
            .viewType = vk::ImageViewType::e2D,
            .format = needs_hdr ? SURFACE_FORMAT_HDR.format : surface_format.format,
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        });
        ASSERT_MSG(im_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
                   vk::to_string(im_view_result));
        images_view[i] = im_view;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, images[i], "Swapchain Image {}", i);
        SetObjectName(device, images_view[i], "Swapchain ImageView {}", i);
    }
}

} // namespace Vulkan
