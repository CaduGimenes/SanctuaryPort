// SPDX-License-Identifier: GPL-2.0-or-later
// sanctuary: temporal upscaling of the HDR scene color (docs/upscaler.md). Off for Diablo III
// until its scene color, depth and camera constants are found (run.py). SANCTUARY_UPSCALER=fsr3
// runs FSR 3.1 (FireBurn/FSR-Vulkan, native Vulkan) on the scene color right before the
// post-processing combine pass, with the scene depth and camera motion vectors, and writes the
// result back so the game's own post, tonemap and UI continue unchanged (Native AA preset).
//
// Guest targets stay at 1920x1080. Host scene targets use output/preset dimensions,
// resized at frame boundaries. At 1080p HDR reconstruction precedes post; other outputs
// reconstruct tonemapped scene proxies before output-size UI/display composition.
// SANCTUARY_RENDER_RES retains the older startup-patched guest-resolution path.

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_fsr4.h"
#include "video_core/texture_cache/image.h"

struct FfxVkPortableUpscaleContext;

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class CameraMotion;
class SceneTargets;

class TemporalUpscaler {
public:
    TemporalUpscaler(const Instance& instance, Scheduler& scheduler,
                     VideoCore::TextureCache& texture_cache, Runtime& runtime,
                     CameraMotion& camera_motion, SceneTargets& scene_targets);
    ~TemporalUpscaler();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    /// A draw into a full-size RGBA16F target with the scene depth: the scene color.
    void OnSceneColor(VideoCore::ImageId color);

    /// A blended (transparent) draw into the scene color that is not a full-screen pass: the
    /// first one of a frame snapshots the opaque scene for the reactive mask.
    void OnBlendedSceneDraw();

    /// A full-screen pass into the scene color: after a snapshot, the reactive mask is taken
    /// before it (the fog composite rewrites every pixel).
    void OnSceneComposite();

    /// Diablo III: a draw into the 10-bit scene color with the scene depth.
    void OnGeometryColor(VideoCore::ImageId color);
    /// Diablo III copies its scene depth after the surfaces (blended ground details included);
    /// after it come the darkness and fog layers, then the effects.
    void OnD3SceneDepthCopy() {
        d3_after_depth_copy = true;
    }
    /// Diablo III: an additive draw into the scene color (fire, glows, spells). The first one
    /// after the depth copy keeps the scene for the reactive mask (the darkness and fog layers,
    /// which change every pixel, are drawn by then).
    void OnD3AdditiveDraw();
    /// Diablo III: every draw's first color target (null without one). After the snapshot,
    /// the first one that is not the scene color takes the reactive mask.
    void OnD3Target(VideoCore::ImageId color);
    /// Diablo III, at the copy of the finished frame to the display buffer: FSR 3 on that
    /// frame (after the game's post and outline). With a window larger than the render size it
    /// upscales to the window and the display copy and HUD are drawn at that size
    /// (RedirectColor/RedirectDepth/RedirectSampled, DisplayOverride); else it runs 1:1 as
    /// anti-aliasing and writes the frame back.
    void RunDisplayUpscale(VideoCore::ImageId frame);

    /// Before a compute dispatch: the post-processing combine shader triggers the upscale.
    void OnDispatch(u64 cs_hash);

    /// Start of a frame in the command stream (display pass).
    bool OnFrameStart();
    bool RasterScaling() const;
    /// sanctuary: LOD bias for scene materials, log2(render / output) while a reduced scene is
    /// upscaled (0 at native size, for TAA and without an upscaler).
    [[nodiscard]] float SceneMipBias() const;
    /// sanctuary: the state RedirectColor/RedirectDepth/RasterScaling depend on, for memoizing a
    /// draw's render state (Rasterizer::BeginRendering).
    [[nodiscard]] u64 RedirectState() const noexcept {
        return u64(ui_phase) | u64(display_redirect) << 1 | u64(done_this_frame) << 2 |
               u64(viewport_redirect) << 3 |
               u64(ui_color.index) << 8 | u64(ui_depth.index) << 36;
    }


    /// This frame's sub-pixel jitter in pixels (screen x right, y down); zero when off and after
    /// the upscale (post and UI are not jittered).
    [[nodiscard]] std::array<float, 2> Jitter() const noexcept {
        return done_this_frame ? std::array<float, 2>{} : jitter;
    }

    // Scaled presets.

    /// After identifying this draw's targets, before drawing any UI pixels. Menus use
    /// spatial background copy and native UI, without needing scene depth or FSR history.
    void OnDraw(u64 vs_hash, VideoCore::ImageId color, VideoCore::ImageId depth,
                bool native_viewport);

    /// A pass's first color target: the last render-size RGBA8 target before the UI is the
    /// game's finished frame.
    void OnColorTarget(VideoCore::ImageId color);

    struct Target {
        vk::ImageView view;
        vk::ImageLayout layout;
        u32 width, height;
        bool native_ui = false;
    };
    /// Render target redirection (UI passes, display pass): false when not redirected.
    /// `view` is the game's view of the image: redirected views mirror its format (sRGB) and,
    /// for sampling, its channel swizzle.
    bool RedirectColor(VideoCore::ImageId color, const VideoCore::ImageViewInfo& view,
                       Target& target);
    bool RedirectDepth(VideoCore::ImageId depth, Target& target);
    /// A sampled image replaced by the upscaled frame (the display pass).
    bool RedirectSampled(VideoCore::ImageId image, const VideoCore::ImageViewInfo& info,
                         vk::ImageView& view, vk::ImageLayout& layout);
    /// Whether RedirectSampled would replace `image` (it may record a barrier then).
    [[nodiscard]] bool RedirectsSampled(VideoCore::ImageId image) const {
        return (display_redirect || viewport_redirect) && image == ui_color;
    }

    /// Presenter: the output-size display buffer standing in for the guest one at `address`.
    struct Display {
        vk::Image image;
        vk::Format format;
        u32 width, height;
    };
    bool DisplayOverride(VAddr address, Display& display);
    /// Diablo III: the host display buffer drawn this frame at `address` (render size below the
    /// window), in layout General; false when the guest buffer is used.
    bool DisplayHostImage(VAddr address, Display& display);

    /// sanctuary: what frame generation needs from this frame's FSR 3 dispatch (Diablo III's
    /// display path), valid until the next frame starts. Depth and motion are in General.
    struct FrameGenInputs {
        bool valid = false;
        VideoCore::ImageId depth{};
        vk::Image motion{};
        u32 width = 0, height = 0;               ///< the scene's viewport (dynamic resolution)
        u32 target_width = 0, target_height = 0; ///< its targets (the largest viewport)
        std::array<float, 2> jitter{};
        float near_plane = 0, far_plane = 0, fov = 0, frame_ms = 16.6f;
        bool reset = false;
    };
    FrameGenInputs TakeFrameGenInputs() {
        return std::exchange(frame_gen_inputs, {});
    }

private:
    /// sanctuary: views of guest images the upscaler reads, kept across frames: FSR 4 registers
    /// images by view in a registry of eight (a new view per frame filled it at Native AA).
    vk::ImageView CachedView(const VideoCore::Image& image, vk::Format format,
                             vk::ImageAspectFlags aspect);
    struct ViewEntry {
        vk::Image image;
        u64 uid = 0;
        vk::Format format{};
        vk::ImageAspectFlags aspect{};
        vk::ImageView view;
        u64 last_use = 0;
    };
    std::array<ViewEntry, 6> view_cache{};
    u64 view_uses = 0;

    void Run();
    void RunScaled();
    void RunUiOnly(VideoCore::ImageId color, VideoCore::ImageId depth);
    void EnsureUiResources(u32 width, u32 height, vk::Format color, vk::Format depth);
    void PrepareUiDepth(VideoCore::ImageId depth);
    /// Render size below the scaled-preset output size (the resolution patch is on).
    [[nodiscard]] bool Scaled() const;
    /// A target of the patched render size: the game allocates it with aligned dimensions
    /// (a 1916x1078 scene in 1916x1080 targets).
    [[nodiscard]] bool RenderTarget(u32 w, u32 h) const {
        if (!scaled_session) return w == 1920 && h == 1080;
        return w >= render_width && h >= render_height && w < render_width + 8 &&
               h < render_height + 8;
    }
    /// The scene's size inside a `w` x `h` render target: the game's viewport.
    [[nodiscard]] std::array<u32, 2> SceneSize(u32 w, u32 h) const;
    // The scene color is drawn into a reduced SceneTargets proxy (live presets).
    [[nodiscard]] bool ReducedScene(const VideoCore::Image& color) const;
    /// Available and switched on (menu setting, toggle 1 << 24).
    [[nodiscard]] bool Active() const;
    [[nodiscard]] bool ReactiveOn() const;
    bool EnsureResources(u32 width, u32 height, u32 out_width, u32 out_height, bool hdr);
    void CreatePipelines();
    /// Records the reactive mask pass; false when there is no snapshot this frame.
    bool RecordReactive(vk::ImageView color_view);
    /// Diablo III: the 2-sample scene into opaque_image (mode 0) or, compared with it, into the
    /// reactive mask (mode 1). Outside a render pass; false when the sizes do not match.
    bool RecordD3Reactive(VideoCore::ImageId color, u32 mode);
    /// FSR 4 is selected, possible in this session (not SANCTUARY_RENDER_RES) and has not failed.
    [[nodiscard]] bool UseFsr4() const;
    /// sanctuary: DLSS selected and the bridge is ready (NVIDIA RTX, sanctuary_dlss.dll).
    [[nodiscard]] bool UseDlss() const;
    /// Records DLSS into `cmdbuf` (output in General). `hdr`: linear scene color input.
    bool RecordDlss(vk::CommandBuffer cmdbuf, const Dlss::Resource& color,
                    const Dlss::Resource& depth, u32 w, u32 h, u32 ow, u32 oh, float frame_ms,
                    bool hdr);
    /// Records FSR 4 into output_image; on a permanent failure FSR 3 takes over.
    bool RecordFsr4(vk::CommandBuffer cmdbuf, Fsr4Upscaler::Image color, Fsr4Upscaler::Image depth,
                    u32 w, u32 h, u32 ow, u32 oh, float frame_ms);
    void RecordTaa(vk::CommandBuffer cmdbuf, vk::ImageView color, vk::ImageView depth);
    /// Sharpness above 1 for FSR 3/4 (their RCAS stops at 1): one more RCAS pass over the target
    /// (output_image, or the 8-bit UI image with ldr) in General layout after the upscaler.
    void ExtraSharpen(vk::CommandBuffer cmdbuf, vk::Image target, bool ldr, u32 w, u32 h);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    CameraMotion& camera_motion;
    SceneTargets& scene_targets;
    int applied_preset = -1;
    int applied_upscaler = -1;
    bool dispatched_last_frame = false;
    bool last_active = false, last_jitter = false;


    bool enabled = false;
    bool failed = false;
    u64 trigger_hash = 0x9a9cf8a9;
    VideoCore::ImageId scene_color{};
    VideoCore::ImageId geometry_color{}; ///< Diablo III: this frame's scene color (MSAA)
    /// Diablo III: the display buffer of this frame is drawn at the window size (its HUD depth
    /// of `display_size` is redirected too).
    bool d3_display = false;
    vk::Image display_frame_image{};          ///< ui_image, for display_frame_view
    vk::UniqueImageView display_frame_view;   ///< ui_image as a storage image (its own format)
    std::array<u32, 2> display_size{};
    std::array<u32, 2> d3_jitter_sizes{}; ///< render and output width of the last upscale
    /// The window size when it is larger than `w` x `h` (Diablo III's upscale target).
    bool DisplayUpscaleSize(u32 w, u32 h, u32& ow, u32& oh) const;
    bool done_this_frame = false;
    u32 preset_file_frames = 0; ///< SANCTUARY_PRESET_FILE polling
    int applied_output = -1;
    bool snapshot_taken = false;
    bool d3_after_depth_copy = false;
    bool opaque_valid = false;
    bool mask_ready = false;
    std::array<float, 2> jitter{};
    u32 jitter_index = 0;
    bool reset = true;
    u64 frame_id = 0;
    std::chrono::steady_clock::time_point last_frame{};

    u32 width = 0, height = 0;             ///< render size
    u32 out_width = 0, out_height = 0;     ///< output size of the context
    bool context_hdr = true;
    u32 target_width = 1920, target_height = 1080; ///< output size of scaled presets
    u64 ui_trigger_vs = 0x34e8a281;
    VideoCore::ImageId ldr_target{};
    VideoCore::ImageId ui_color{}, ui_depth{};
    bool ui_phase = false;         ///< from the upscale to the next display pass
    bool display_redirect = false; ///< the display pass of an upscaled frame
    bool ui_read_barrier = false;
    VideoCore::UniqueImage ui_image;
    VideoCore::UniqueImage ui_depth_image;
    vk::UniqueImageView ui_view;
    vk::UniqueImageView ui_depth_view;
    bool depth_blit = false;
    bool scaled_session = false;
    u32 ui_width = 0, ui_height = 0;
    u32 render_width = 0, render_height = 0;
    vk::Format ui_format = vk::Format::eUndefined;
    vk::Format ui_depth_format = vk::Format::eUndefined;
    /// Views of the port's images in the formats/swizzles the game's views use.
    struct MirrorView {
        vk::Format format;
        vk::ComponentMapping mapping;
        vk::UniqueImageView view;
    };
    vk::ImageView Mirror(vk::Image image, std::vector<MirrorView>& views, vk::Format format,
                         vk::ComponentMapping mapping);
    std::vector<MirrorView> ui_views;
    /// sanctuary: Diablo III at the window size with dynamic resolution: FSR 3 upscales the
    /// scene's viewport (top left of the frame) to the frame size, into an image as much larger
    /// than the frame as the frame is than the viewport, so the display copy, which stretches the
    /// viewport's part of the frame, reads FSR's output pixel for pixel. One per viewport size
    /// (the game changes it about twice a second): no reallocation, no wait.
    struct ViewportTarget {
        VideoCore::UniqueImage image;
        std::vector<MirrorView> views;
        vk::UniqueImageView storage;
        u32 width = 0, height = 0;
        u64 used = 0;
    };
    std::vector<std::unique_ptr<ViewportTarget>> viewport_targets;
    ViewportTarget* viewport_target = nullptr;
    bool viewport_phase = false;    ///< this frame's display copy reads viewport_target
    bool viewport_redirect = false; ///< the display pass of such a frame
    bool viewport_sampled = false;  ///< the display copy has read viewport_target
    ViewportTarget* EnsureViewportTarget(u32 width, u32 height, vk::Format format);
    struct DisplayImage {
        VideoCore::UniqueImage image;
        std::vector<MirrorView> views;
        vk::Format format{};
        u32 width = 0, height = 0;
        bool valid = false;
        u64 drawn_frame = ~0ull; ///< frame_count of its first pass (its old contents dropped)
    };
    std::mutex display_mutex;
    u64 frame_count = 0; ///< OnFrameStart calls
    FrameGenInputs frame_gen_inputs;
    void NoteFrameGenInputs(u32 w, u32 h, float frame_ms, bool was_reset);
    std::unordered_map<VAddr, DisplayImage> displays;
    FfxVkPortableUpscaleContext* context = nullptr;
    bool resources_ready = false; ///< images below match width/height/out size
    bool resources_fsr4 = false;  ///< made for FSR 4 (no FSR 3 context)
    bool resources_taa = false;
    std::unique_ptr<Fsr4Upscaler> fsr4;
    bool fsr4_failed = false;
    bool dlss_failed = false;
    VideoCore::UniqueImage motion_image;
    VideoCore::UniqueImage output_image;
    vk::UniqueImageView motion_view;
    vk::UniqueImageView output_view;
    VideoCore::UniqueImage opaque_image;   ///< scene color before the blended draws
    VideoCore::UniqueImage reactive_image; ///< R8 reactive mask
    vk::UniqueImageView opaque_view;
    vk::UniqueImageView reactive_view;
    vk::UniqueDescriptorSetLayout reactive_desc_layout;
    vk::UniquePipelineLayout reactive_pipeline_layout;
    vk::UniquePipeline reactive_pipeline;
    vk::UniqueDescriptorSetLayout reactive_msaa_desc_layout;
    vk::UniquePipelineLayout reactive_msaa_pipeline_layout;
    vk::UniquePipeline reactive_msaa_pipeline; ///< Diablo III (reactive_msaa.comp)
    vk::UniqueDescriptorSetLayout merge_desc_layout;
    vk::UniquePipelineLayout merge_pipeline_layout;
    vk::UniquePipeline merge_pipeline;
    vk::UniquePipeline merge_rgb10a2_pipeline; ///< Diablo III: 10-bit scene color
    /// Diablo III: the 10-bit scene converted to 16-bit float for FSR (it takes no 10-bit input).
    VideoCore::UniqueImage input_image;
    vk::UniqueImageView input_view;

    std::array<VideoCore::UniqueImage, 2> taa_history;
    std::array<vk::UniqueImageView, 2> taa_history_views;
    u32 taa_next = 0;
    vk::UniqueSampler taa_sampler;
    vk::UniqueDescriptorSetLayout taa_desc_layout;
    vk::UniquePipelineLayout taa_pipeline_layout;
    vk::UniquePipeline taa_pipeline;
    vk::UniqueDescriptorSetLayout taa_sharpen_desc_layout;
    vk::UniquePipelineLayout taa_sharpen_pipeline_layout;
    vk::UniquePipeline taa_sharpen_pipeline;
    vk::UniquePipeline taa_sharpen_ldr_pipeline;
    // ExtraSharpen: a copy of the upscaled frame (RCAS reads neighbours) and the target views.
    VideoCore::UniqueImage extra_sharpen_image;
    vk::UniqueImageView extra_sharpen_view;
    u32 extra_sharpen_width = 0, extra_sharpen_height = 0;
    vk::UniqueImageView ui_storage_view;
};

} // namespace Vulkan
