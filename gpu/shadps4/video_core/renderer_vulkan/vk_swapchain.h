// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Frontend {
class WindowSDL;
}

namespace Vulkan {

class Instance;
class Scheduler;

class Swapchain {
public:
    explicit Swapchain(const Instance& instance, const Frontend::WindowSDL& window);
    ~Swapchain();

    /// Creates (or recreates) the swapchain with a given size.
    void Create(u32 width, u32 height);

    /// Recreates the swapchain with a given size and current surface.
    void Recreate(u32 width, u32 height);

    /// Acquires the next image in the swapchain (or takes the one AcquireAhead acquired) and waits on
    /// the CPU until the image is available.
    bool AcquireNextImage();

    /// sanctuary: without V-Sync, acquires the image for the next frame right after a present, so
    /// that AcquireNextImage's CPU wait on it overlaps the time until that frame (GitHub #110).
    void AcquireAhead();

    /// sanctuary: the scheduler whose submissions wait on GetImageAcquiredSemaphore() (the blit);
    /// an acquire semaphore is passed to the acquire again only once that wait has completed.
    void SetAcquireWaitScheduler(Scheduler* scheduler) {
        acquire_wait_scheduler = scheduler;
    }

    /// Presents the current image and move to the next one
    bool Present();

    vk::SurfaceKHR GetSurface() const {
        return surface;
    }

    vk::Image Image() const {
        return images[image_index];
    }

    vk::ImageView ImageView() const {
        return images_view[image_index];
    }

    vk::SurfaceFormatKHR GetSurfaceFormat() const {
        return surface_format;
    }

    vk::SwapchainKHR GetHandle() const {
        return swapchain;
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    u32 GetImageCount() const {
        return image_count;
    }

    u32 GetFrameIndex() const {
        return frame_index;
    }

    vk::Extent2D GetExtent() const {
        return extent;
    }

    [[nodiscard]] vk::Semaphore GetImageAcquiredSemaphore() const {
        return image_acquired[frame_index];
    }

    [[nodiscard]] vk::Semaphore GetPresentReadySemaphore() const {
        return present_ready[image_index];
    }

    bool HasHDR() const {
        return supports_hdr;
    }

    void SetHDR(bool hdr);

    /// sanctuary: VSync from the in-game menu (SanctuarySettings::vsync); recreates the swapchain when it
    /// changed. Present thread only.
    void UpdateVsync();

    bool GetHDR() const {
        return needs_hdr;
    }

private:
    /// Selects the best available swapchain image format
    void FindPresentFormat();

    /// Selects the best available present mode
    void FindPresentMode();

    /// Sets the surface properties according to device capabilities
    void SetSurfaceProperties();

    /// Destroys current swapchain resources
    void Destroy();

    /// Performs creation of image views and framebuffers from the swapchain images
    void SetupImages();

    /// Creates the image acquired and present ready semaphores
    void RefreshSemaphores();

    /// sanctuary: the acquire with semaphore and fence; FinishAcquire waits for the fence.
    vk::Result StartAcquire();
    void FinishAcquire(vk::Result result);

private:
    const Instance& instance;
    const Frontend::WindowSDL& window;
    vk::SwapchainKHR swapchain{};
    vk::SurfaceKHR surface{};
    vk::SurfaceFormatKHR surface_format;
    vk::Format view_format;
    vk::PresentModeKHR present_mode;
    bool vsync = true; ///< the setting present_mode was chosen for
    bool borderless = false; ///< sanctuary: created refusing exclusive fullscreen (GitHub #109)
    vk::Extent2D extent;
    vk::SurfaceTransformFlagBitsKHR transform;
    vk::CompositeAlphaFlagBitsKHR composite_alpha;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> images_view;
    std::vector<vk::Semaphore> image_acquired;
    std::vector<vk::Semaphore> present_ready;
    /// sanctuary: signaled with image_acquired; the present thread waits for it on the CPU before
    /// anything waits on image_acquired on the GPU (GitHub #103, #110).
    std::vector<vk::Fence> image_acquired_fences;
    /// sanctuary: AcquireAhead's acquire, not taken by AcquireNextImage yet, and its result.
    bool acquired_ahead = false;
    vk::Result ahead_result = vk::Result::eSuccess;
    /// sanctuary: per image_acquired semaphore, the tick of acquire_wait_scheduler's submission that
    /// waited on it (0: none).
    Scheduler* acquire_wait_scheduler = nullptr;
    std::vector<u64> acquire_wait_ticks;
    u32 width = 0;
    u32 height = 0;
    u32 image_count = 0;
    u32 image_index = 0;
    u32 frame_index = 0;
    bool needs_recreation = true;
    bool needs_hdr = false;    // The game requested HDR swapchain
    bool supports_hdr = false; // SC supports HDR output
};

} // namespace Vulkan
