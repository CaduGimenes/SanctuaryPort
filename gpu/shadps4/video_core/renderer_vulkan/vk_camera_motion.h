// SPDX-License-Identifier: GPL-2.0-or-later
// sanctuary: camera motion vectors for temporal upscaling (docs/upscaler.md). The scene constants
// give the camera of each frame; the previous one is kept here. SANCTUARY_DEBUG_MOTION=1 blends the
// motion vectors as colors into the frame before it is copied to the display.

#pragma once

#include <array>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class ObjectMotion;

class CameraMotion {
public:
    CameraMotion(const Instance& instance, Scheduler& scheduler,
                 VideoCore::TextureCache& texture_cache, Runtime& runtime);
    ~CameraMotion();
    void InvalidateHistory() { current.valid = previous.valid = false; }
    void SetObjectMotion(ObjectMotion* motion) noexcept {
        object_motion = motion;
    }
    /// This frame's object motion image (after RecordMotion), for the debug view; or null.
    vk::ImageView ObjectMotionView() const noexcept;
    vk::Image ObjectMotionImage(u32 width, u32 height) const noexcept;

    void SetJitter(std::array<float, 2> value) noexcept {
        previous_jitter = jitter;
        jitter = value;
    }

    [[nodiscard]] bool Enabled() const noexcept {
        return debug_overlay || for_upscaler;
    }

    /// Both cameras and the scene depth of the current frame are known.
    [[nodiscard]] bool Ready() const noexcept {
        return current.valid && previous.valid && depth_id;
    }
    [[nodiscard]] VideoCore::ImageId Depth() const noexcept {
        return depth_id;
    }
    /// The render size in the scene constants (the game's viewport; its targets may be
    /// allocated larger, e.g. 1916x1080 for a 1916x1078 scene), or zero before the first camera.
    [[nodiscard]] std::array<u32, 2> RenderSize() const noexcept {
        return render_size;
    }
    /// Vertical field of view and near/far planes of the current camera.
    [[nodiscard]] float VerticalFov() const noexcept;
    [[nodiscard]] float Near() const noexcept;
    [[nodiscard]] float Far() const noexcept;
    // Current projection, previous projection, and row Z of previous-view * inverse-view.
    [[nodiscard]] std::array<std::array<float, 4>, 3> TaaDepthParameters() const noexcept;

    /// Diablo III: the scene's viewport (dynamic resolution: the top-left part of its targets).
    void SetSceneViewport(u32 width, u32 height) noexcept {
        scene_viewport = {width, height};
    }
    /// The part of a `width` x `height` target the scene covers.
    [[nodiscard]] std::array<u32, 2> SceneRect(u32 width, u32 height) const noexcept {
        if (scene_viewport[0] < 16 || scene_viewport[1] < 16 || scene_viewport[0] > width ||
            scene_viewport[1] > height) {
            return {width, height};
        }
        return scene_viewport;
    }
    /// Records the motion vector pass: `depth_view` (depth aspect, General layout) into
    /// `motion_view` (RG16F storage, General), pixels, previous minus current.
    void RecordMotion(vk::CommandBuffer cmdbuf, vk::ImageView depth_view, vk::ImageView motion_view,
                      u32 width, u32 height);

    /// A bound constant buffer of 864 bytes: checks the scene constant signature.
    void OnConstants(const float* data);

    /// The G-buffer pass (5+ color targets): its depth is the scene depth.
    void OnGBufferPass(VideoCore::ImageId depth);

    /// Diablo III: a vertex shader constant block of `ObjectBlockSize` bytes. Floats [56..71]
    /// are the draw's object-to-clip matrix (rows), [416..417] the render size; the camera of
    /// the frame comes from these (ResolveObjectCamera). `shader` is the vertex shader hash.
    static constexpr u32 ObjectBlockSize = 2384;
    /// Returns true when the block holds this frame's scene camera (a perspective matrix).
    bool OnObjectBlock(const float* block, u64 shader);
    /// Diablo III: the depth target of draws with the scene camera (the 2-sample scene depth):
    /// geometry drawn with it is jittered.
    void NoteGeometryDepth(VideoCore::ImageId depth) noexcept {
        geometry_depth = depth;
    }
    [[nodiscard]] VideoCore::ImageId GeometryDepth() const noexcept {
        return geometry_depth;
    }
    /// Diablo III: the single-sample copy of the scene depth (render size, depth only).
    void OnSceneDepthCopy(VideoCore::ImageId depth, u32 width, u32 height);

    /// The pass copying the finished frame (`frame`, the last target drawn) to the display: the
    /// frame's depth and camera are complete. Records the debug overlay, starts a new frame.
    void OnDisplayPass(VideoCore::ImageId frame);

private:
    struct Camera {
        std::array<float, 12> view{};     ///< world to view, 3x4 rows
        std::array<float, 12> inv_view{}; ///< view to world, 3x4 rows
        std::array<float, 4> proj{};      ///< x scale, y scale, z scale, z offset
        bool valid = false;
    };

    void Overlay(VideoCore::ImageId frame);
    /// Diablo III: matches this frame's object matrices with the previous frame's; the camera
    /// displacement most of them agree on (static scenery) moves the tracked camera.
    void ResolveObjectCamera();

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    bool debug_overlay = false;
    bool for_upscaler = false;
    vk::UniqueDescriptorSetLayout motion_desc_layout;
    vk::UniquePipelineLayout motion_pipeline_layout;
    vk::UniquePipeline motion_pipeline;

    ObjectMotion* object_motion = nullptr;
    std::array<u32, 2> scene_viewport{};
    Camera current, previous;
    bool frame_has_camera = false;
    std::array<float, 2> jitter{}, previous_jitter{};
    std::array<u32, 2> render_size{};
    VideoCore::ImageId depth_id{};
    VideoCore::ImageId geometry_depth{};

    // Diablo III object-to-clip matrices of this frame and the previous one, by object key.
    struct ObjectMatrix {
        std::array<double, 16> m{};
        bool ambiguous = false; ///< the key was drawn with different matrices (copies of a mesh)
    };
    std::unordered_map<u64, ObjectMatrix> object_matrices, previous_object_matrices;
    std::array<double, 3> camera_position{}; ///< tracked camera, in the first frame's space
    std::array<u32, 2> frame_size{}; ///< largest render size in this frame's blocks
    bool camera_resolved = false;    ///< this frame's camera is known (scene depth copy)
    bool trace_objects = false;
    u32 trace_frames = 0;

    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline overlay_pipeline;
    vk::UniquePipeline overlay_rgb10a2_pipeline; ///< Diablo III's 10-bit frame
    /// Debug: the last two finished frames (RGBA8 packed), for the reprojection check.
    std::unique_ptr<VideoCore::Buffer> frames[2];
    u32 frame_index = 0;
};

} // namespace Vulkan
