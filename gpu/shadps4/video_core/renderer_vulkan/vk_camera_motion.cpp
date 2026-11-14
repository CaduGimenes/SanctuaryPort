// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_camera_motion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sanctuary_toggles.h"

#include "video_core/host_shaders/camera_motion_comp.h"
#include "video_core/host_shaders/camera_motion_debug_comp.h"
#include "video_core/host_shaders/camera_motion_debug_rgb10a2_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

struct PushConstants {
    std::array<float, 12> reproject;
    std::array<float, 4> proj;
    std::array<float, 4> prev_proj;
    std::array<float, 2> size;
    std::array<float, 2> jitter;
    std::array<float, 2> previous_jitter;
    u32 mode;
    u32 pad;
    std::array<float, 2> prev_size; ///< the previous frame's scene viewport (dynamic resolution)
};

/// a * b for 3x4 affine matrices (rows [R | t]).
std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = c == 3 ? a[r * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

/// Diablo III: checks that `m` (object-to-clip, rows) is a symmetric perspective projection
/// with a unit view direction in row 3, as the scene draws use.
bool IsPerspective(const std::array<double, 16>& m) {
    auto dot = [&](int a, int b) {
        return m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1] + m[a * 4 + 2] * m[b * 4 + 2];
    };
    const double w = dot(3, 3), x = dot(0, 0), y = dot(1, 1), z = dot(2, 2);
    if (std::abs(w - 1.0) > 0.02 || x < 0.04 || y < 0.04 || z < 0.5) {
        return false;
    }
    if (std::abs(dot(0, 3)) > 1e-3 * std::sqrt(x) || std::abs(dot(1, 3)) > 1e-3 * std::sqrt(y) ||
        std::abs(dot(0, 1)) > 1e-3 * std::sqrt(x * y)) {
        return false;
    }
    // Row 2 = a * row 3 (depth = a + b / w).
    const double a = dot(2, 3) / w;
    return a > 0.5 && a < 2.0 && std::abs(z - a * a * w) < 1e-3 * z;
}

/// The camera position in the matrix's object space: where clip x, y and w are zero.
bool SolveCamera(const std::array<double, 16>& m, std::array<double, 3>& c) {
    const int rows[3] = {0, 1, 3};
    double a[3][3], b[3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            a[i][j] = m[rows[i] * 4 + j];
        }
        b[i] = -m[rows[i] * 4 + 3];
    }
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                       a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (std::abs(det) < 1e-9) {
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        double t[3][3];
        std::memcpy(t, a, sizeof(t));
        for (int i = 0; i < 3; ++i) {
            t[i][k] = b[i];
        }
        c[k] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) -
                t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
                t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) / det;
    }
    return true;
}

u64 HashFloats(u64 h, const float* data, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        u32 bits;
        std::memcpy(&bits, data + i, 4);
        h = (h ^ bits) * 0x100000001b3ULL;
    }
    return h;
}

} // namespace

CameraMotion::CameraMotion(const Instance& instance_, Scheduler& scheduler_,
                           VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_}, runtime{runtime_} {
    const char* env = std::getenv("SANCTUARY_DEBUG_MOTION");
    debug_overlay = env && env[0] == '1';
    const char* trace = std::getenv("SANCTUARY_CAMERA_TRACE");
    trace_objects = trace && trace[0] == '1';
    const char* upscaler = std::getenv("SANCTUARY_UPSCALER");
    // The upscaler can be switched on from the menu at any time: the camera is always tracked
    // unless SANCTUARY_UPSCALER=none.
    for_upscaler = !(upscaler && std::strcmp(upscaler, "none") == 0);
    const auto device = instance.GetDevice();
    if (for_upscaler) {
        const std::array<vk::DescriptorSetLayoutBinding, 3> motion_bindings = {{
            {.binding = 0,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 2,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        motion_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(motion_bindings.size()),
            .pBindings = motion_bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(PushConstants)};
        motion_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*motion_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto motion_module = CompileSPV(CAMERA_MOTION_COMP, device);
        motion_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = motion_module,
                              .pName = "main"},
                    .layout = *motion_pipeline_layout,
                }));
        device.destroyShaderModule(motion_module);
    }
    if (!debug_overlay) {
        return;
    }
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 3,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push_range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    }));
    auto make_overlay = [&](const auto& code) {
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
    overlay_pipeline = make_overlay(CAMERA_MOTION_DEBUG_COMP);
    overlay_rgb10a2_pipeline = make_overlay(CAMERA_MOTION_DEBUG_RGB10A2_COMP);
    for (auto& frame : frames) {
        frame = std::make_unique<VideoCore::Buffer>(instance, 0, 3840ull * 2160 * 4,
                                                    VideoCore::MemoryType::DeviceLocal);
    }
    std::printf("GPU: camera motion debug overlay on\n");
}

CameraMotion::~CameraMotion() = default;

float CameraMotion::VerticalFov() const noexcept {
    return 2.0f * std::atan(1.0f / current.proj[1]);
}

float CameraMotion::Near() const noexcept {
    // depth = zs + zo / z is 0 at the near plane.
    return -current.proj[3] / current.proj[2];
}

float CameraMotion::Far() const noexcept {
    // depth = zs + zo / z is 1 at the far plane.
    const float zs = current.proj[2], zo = current.proj[3];
    const float far = std::abs(1.0f - zs) > 1e-7f ? zo / (1.0f - zs) : 0.0f;
    return far > Near() && far < 1e7f ? far : 3000.0f;
}

std::array<std::array<float, 4>, 3> CameraMotion::TaaDepthParameters() const noexcept {
    const auto transform = Multiply(previous.view, current.inv_view);
    return {current.proj, previous.proj,
            {transform[8], transform[9], transform[10], transform[11]}};
}

vk::ImageView CameraMotion::ObjectMotionView() const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->View() : vk::ImageView{};
}

vk::Image CameraMotion::ObjectMotionImage(u32 width, u32 height) const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->Image(width, height)
                                                      : vk::Image{};
}

void CameraMotion::RecordMotion(vk::CommandBuffer cmdbuf, vk::ImageView depth_view,
                                vk::ImageView motion_view, u32 width, u32 height,
                                bool same_scale) {
    bool object_valid = false;
    const vk::ImageView object_view = object_motion && object_motion->Enabled()
        ? object_motion->PrepareRead(cmdbuf, width, height, object_valid) : depth_view;
    // Dynamic resolution: the scene's projection covers only its viewport, and the previous
    // frame's covered its own: a vector points to where the pixel was in that frame's viewport
    // (the game changes the size about twice a second under FSR 3 on a small GPU; with the
    // current size for both frames each change smeared the whole picture for a frame).
    const auto rect = SceneRect(width, height);
    const auto prev_rect = previous_rect[0] && !same_scale ? previous_rect : rect;
    previous_rect = rect;
    width = rect[0];
    height = rect[1];
    const PushConstants push{
        .reproject = Multiply(previous.view, current.inv_view),
        .proj = current.proj,
        .prev_proj = previous.proj,
        .size = {float(width), float(height)},
        .jitter = jitter,
        .previous_jitter = previous_jitter,
        .mode = object_valid ? 1u : 0u,
        .prev_size = {float(prev_rect[0]), float(prev_rect[1])},
    };
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo motion_info{.imageView = motion_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo object_info{.imageView = object_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 3> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &motion_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &object_info},
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *motion_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *motion_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*motion_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), &push);
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

void CameraMotion::OnConstants(const float* data) {
    // Scene constants: far plane 3000, 1/far, and the render size.
    if (data[0] != 3000.0f || data[4] < 64.0f || data[5] < 64.0f ||
        std::abs(data[1] * data[0] - 1.0f) > 1e-3f) {
        return;
    }
    if (frame_has_camera) {
        return; // the first one of a frame is the main camera
    }
    previous = current;
    std::memcpy(current.view.data(), data + 8, 12 * sizeof(float));
    std::memcpy(current.inv_view.data(), data + 180, 12 * sizeof(float));
    current.proj = {data[52], data[57], data[62], data[63]};
    current.valid = current.proj[0] != 0.0f && current.proj[1] != 0.0f;
    const std::array<u32, 2> size{u32(data[4]), u32(data[5])};
    if (size != render_size) {
        std::printf("Camera motion: scene render size %ux%u\n", size[0], size[1]);
        render_size = size;
    }
    frame_has_camera = true;
}

void CameraMotion::OnGBufferPass(VideoCore::ImageId depth) {
    depth_id = depth;
}

bool CameraMotion::OnObjectBlock(const float* block, u64 shader) {
    ObjectMatrix entry;
    for (int i = 0; i < 16; ++i) {
        entry.m[i] = block[56 + i];
    }
    if (!IsPerspective(entry.m)) {
        return false;
    }
    // The object: its shader and its position decoding (scale/offset, [448..455]). Copies of
    // one mesh share this and are dropped as ambiguous. Not [40..55]: for most draws that is a
    // matrix of the hero's light, which moves with the hero, so a running hero left no object
    // matched between frames and the camera (and FSR) was lost on ~40% of the frames.
    if (camera_resolved) {
        return true; // after the scene depth copy: the camera of this frame is set
    }
    const u64 key = HashFloats(0xcbf29ce484222325ULL ^ shader, block + 448, 8);
    const auto [it, inserted] = object_matrices.try_emplace(key, entry);
    if (!inserted && !it->second.ambiguous && it->second.m != entry.m) {
        it->second.ambiguous = true;
    }
    // Post passes (bloom at a quarter size) bind copies of the block: the scene is the largest.
    const u32 w = u32(block[416]), h = u32(block[417]);
    if (w <= 8192 && h <= 8192 && w * h > frame_size[0] * frame_size[1]) {
        frame_size = {w, h};
    }
    return true;
}

void CameraMotion::OnSceneDepthCopy(VideoCore::ImageId depth, u32 width, u32 height) {
    if (geometry_depth_candidate && geometry_depth_candidate != geometry_depth) {
        geometry_depth = geometry_depth_candidate;
        static u32 changes = 0;
        if (++changes <= 16) {
            std::printf("Camera motion: scene geometry depth is image %u\n", geometry_depth.index);
        }
    }
    // The opaque pass is done: every scene camera block of this frame has been seen (the
    // upscaler runs before the frame ends, so the camera is resolved here).
    if (!camera_resolved && !object_matrices.empty()) {
        ResolveObjectCamera();
        camera_resolved = true;
    }
    if (render_size[0] && width == render_size[0] && height >= render_size[1] &&
        height <= render_size[1] + 8) {
        depth_id = depth;
    }
}

void CameraMotion::ResolveObjectCamera() {
    if (frame_size[0] >= 320 && frame_size[1] >= 180 && frame_size != render_size) {
        std::printf("Camera motion: scene render size %ux%u\n", frame_size[0], frame_size[1]);
        render_size = frame_size;
    }
    frame_size = {};
    if (object_matrices.empty()) {
        return;
    }
    // Camera displacement voted by every object drawn in both frames.
    struct Vote {
        std::array<double, 3> delta;
        const ObjectMatrix* current;
        std::array<double, 3> camera; ///< in the object's space, this frame
        u64 key;
    };
    std::vector<Vote> votes;
    votes.reserve(object_matrices.size());
    for (const auto& [key, entry] : object_matrices) {
        if (entry.ambiguous) {
            continue;
        }
        const auto prev = previous_object_matrices.find(key);
        if (prev == previous_object_matrices.end() || prev->second.ambiguous) {
            continue;
        }
        std::array<double, 3> c, p;
        if (!SolveCamera(entry.m, c) || !SolveCamera(prev->second.m, p)) {
            continue;
        }
        votes.push_back({{c[0] - p[0], c[1] - p[1], c[2] - p[2]}, &entry, c, key});
    }
    size_t best = votes.size(), best_count = 0;
    for (size_t i = 0; i < votes.size(); ++i) {
        size_t count = 0;
        for (const auto& v : votes) {
            const double dx = v.delta[0] - votes[i].delta[0], dy = v.delta[1] - votes[i].delta[1],
                         dz = v.delta[2] - votes[i].delta[2];
            count += dx * dx + dy * dy + dz * dz < 1e-4;
        }
        if (count > best_count) {
            best_count = count;
            best = i;
        }
    }
    const ObjectMatrix* reference = nullptr;
    std::array<double, 3> reference_camera{};
    const bool tracked = best < votes.size() && best_count >= 4 && best_count * 4 >= votes.size();
    static const bool frozen = [] {
        const char* env = std::getenv("SANCTUARY_CAMERA_FREEZE"); // debugging: no camera motion
        return env && env[0] == '1';
    }();
    if (tracked) {
        // The objects that agree on the displacement still differ a little in orientation (up to
        // a degree or two passes the vote), and the camera's axes and projection come from the
        // one used as reference: a different one every frame rolled the camera by about half a
        // degree back and forth, and those frames' motion vectors were off by 10-30 pixels
        // (FSR 3 ghosting). Keep the same reference while it agrees; else take the one whose
        // axes are closest to the last camera's.
        const auto agrees = [&](const Vote& v) {
            const double dx = v.delta[0] - votes[best].delta[0], dy = v.delta[1] - votes[best].delta[1],
                         dz = v.delta[2] - votes[best].delta[2];
            return dx * dx + dy * dy + dz * dz < 1e-4;
        };
        const auto axis_distance = [&](const Vote& v) {
            if (!current.valid) {
                return 0.0;
            }
            const auto& m = v.current->m;
            double d = 0.0;
            const int rows[3] = {0, 1, 3};
            for (int r = 0; r < 3; ++r) {
                const double len = std::sqrt(m[rows[r] * 4] * m[rows[r] * 4] +
                                             m[rows[r] * 4 + 1] * m[rows[r] * 4 + 1] +
                                             m[rows[r] * 4 + 2] * m[rows[r] * 4 + 2]);
                for (int k = 0; k < 3; ++k) {
                    d += std::abs(m[rows[r] * 4 + k] / len - current.view[r * 4 + k]);
                }
            }
            return d;
        };
        size_t chosen = best;
        double chosen_distance = 1e30;
        for (size_t i = 0; i < votes.size(); ++i) {
            if (!agrees(votes[i])) {
                continue;
            }
            if (votes[i].key == reference_key) {
                chosen = i;
                break;
            }
            if (const double d = axis_distance(votes[i]); d < chosen_distance) {
                chosen_distance = d;
                chosen = i;
            }
        }
        for (int k = 0; k < 3 && !frozen; ++k) {
            camera_position[k] += votes[chosen].delta[k];
        }
        reference = votes[chosen].current;
        reference_camera = votes[chosen].camera;
        reference_key = votes[chosen].key;
    } else {
        // No history (first frame, a load, a cut): restart from any object of this frame.
        for (const auto& [key, entry] : object_matrices) {
            if (!entry.ambiguous && SolveCamera(entry.m, reference_camera)) {
                reference = &entry;
                reference_key = key;
                camera_position = reference_camera;
                break;
            }
        }
    }
    if (trace_objects && tracked && best < votes.size() &&
        std::abs(votes[best].delta[0]) + std::abs(votes[best].delta[1]) > 1e-3) {
        std::printf("Camera motion: moved %.4f %.4f %.4f (%zu of %zu agree)\n",
                    votes[best].delta[0], votes[best].delta[1], votes[best].delta[2], best_count,
                    votes.size());
    }
    if (trace_objects && (trace_frames++ % 30) == 0) {
        std::printf("Camera motion: %zu objects, %zu matched, %zu agree, camera %.3f %.3f %.3f%s, "
                    "scene depth %s\n",
                    object_matrices.size(), votes.size(), best_count, camera_position[0],
                    camera_position[1], camera_position[2], tracked ? "" : " (restart)",
                    depth_id ? "found" : "missing");
    }
    previous_object_matrices.swap(object_matrices);
    object_matrices.clear();
    if (!reference) {
        return;
    }
    // View = rows right, up, forward (unit) of the matrix; projection x/y scale, depth a + b / w.
    const auto& m = reference->m;
    auto row_length = [&](int r) {
        return std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] +
                         m[r * 4 + 2] * m[r * 4 + 2]);
    };
    const double sx = row_length(0), sy = row_length(1), sw = row_length(3);
    const double a = (m[8] * m[12] + m[9] * m[13] + m[10] * m[14]) / (sw * sw);
    // At the camera w = 0, so clip z there is the depth offset b.
    const double b = m[8] * reference_camera[0] + m[9] * reference_camera[1] +
                     m[10] * reference_camera[2] + m[11];
    const double scale[3] = {sx, sy, sw};
    const int rows[3] = {0, 1, 3};
    Camera camera;
    for (int r = 0; r < 3; ++r) {
        double t = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double v = m[rows[r] * 4 + k] / scale[r];
            camera.view[r * 4 + k] = float(v);
            camera.inv_view[k * 4 + r] = float(v);
            t -= v * camera_position[k];
        }
        camera.view[r * 4 + 3] = float(t);
    }
    for (int k = 0; k < 3; ++k) {
        camera.inv_view[k * 4 + 3] = float(camera_position[k]);
    }
    camera.proj = {float(sx), float(sy), float(a), float(b)};
    camera.valid = true;
    previous = tracked ? current : Camera{};
    current = camera;
    frame_has_camera = true;
}

void CameraMotion::OnDisplayPass(VideoCore::ImageId frame) {
    if (!camera_resolved) {
        ResolveObjectCamera();
    }
    camera_resolved = false;
    if (trace_objects && (trace_frames % 30) == 1) {
        std::printf("Camera motion: display pass, frame %s, depth %s, cameras %d/%d\n",
                    frame ? "yes" : "no", depth_id ? "yes" : "no", current.valid, previous.valid);
    }
    if (debug_overlay && frame && depth_id && current.valid && previous.valid) {
        Overlay(frame);
    }
    if (!frame_has_camera) InvalidateHistory();
    frame_has_camera = false;
    depth_id = {};
}

void CameraMotion::Overlay(VideoCore::ImageId frame) {
    auto& depth = texture_cache.GetImage(depth_id);
    auto& color = texture_cache.GetImage(frame);
    const auto depth_format = depth.info.pixel_format;
    if ((depth_format != vk::Format::eD32Sfloat && depth_format != vk::Format::eD32SfloatS8Uint) ||
        (color.info.pixel_format != vk::Format::eR8G8B8A8Unorm &&
         color.info.pixel_format != vk::Format::eA2B10G10R10UnormPack32) ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
        color.info.size.width != depth.info.size.width ||
        color.info.size.height != depth.info.size.height) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("Camera motion: overlay skipped (depth %s, frame %s %ux%u)\n",
                        vk::to_string(depth_format).c_str(),
                        vk::to_string(color.info.pixel_format).c_str(), color.info.size.width,
                        color.info.size.height);
        }
        return;
    }
    const auto device = instance.GetDevice();
    const auto depth_view = Check(device.createImageView({
        .image = vk::Image(depth.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = depth_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));
    const auto color_view = Check(device.createImageView({
        .image = vk::Image(color.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = color.info.pixel_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    runtime.FlushBarriers();

    const PushConstants push{
        .reproject = Multiply(previous.view, current.inv_view),
        .proj = current.proj,
        .prev_proj = previous.proj,
        .size = {float(color.info.size.width), float(color.info.size.height)},
        .jitter = jitter,
        .previous_jitter = previous_jitter,
        .mode = SanctuaryToggle::Disabled(1u << 20)   ? 1u
                : SanctuaryToggle::Disabled(1u << 21) ? 2u
                : SanctuaryToggle::Disabled(1u << 22) ? 3u
                : SanctuaryToggle::Disabled(1u << 23) ? 4u
                                               : 0u,
        .prev_size = {float(color.info.size.width), float(color.info.size.height)},
    };
    static u32 log_counter = 0;
    if (++log_counter % 200 == 0) {
        const auto& m = push.reproject;
        std::printf("Camera motion: proj %g %g %g %g prev %g %g %g %g\n"
                    "  view  %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f\n"
                    "  reproj %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f\n"
                    "  depth %s %ux%u, frame %ux%u\n",
                    push.proj[0], push.proj[1], push.proj[2], push.proj[3], push.prev_proj[0],
                    push.prev_proj[1], push.prev_proj[2], push.prev_proj[3], current.view[0],
                    current.view[1], current.view[2], current.view[3], current.view[4],
                    current.view[5], current.view[6], current.view[7], current.view[8],
                    current.view[9], current.view[10], current.view[11], m[0], m[1], m[2], m[3],
                    m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
                    vk::to_string(depth_format).c_str(), depth.info.size.width,
                    depth.info.size.height, color.info.size.width, color.info.size.height);
    }
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo color_info{.imageView = color_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const auto& prev_frame = *frames[frame_index ^ 1];
    const auto& next_frame = *frames[frame_index];
    frame_index ^= 1;
    const vk::DescriptorBufferInfo prev_info{prev_frame.Handle(), 0, prev_frame.SizeBytes()};
    const vk::DescriptorBufferInfo next_info{next_frame.Handle(), 0, next_frame.SizeBytes()};
    const std::array<vk::WriteDescriptorSet, 4> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &color_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &prev_info},
        {.dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &next_info},
    }};
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,
                        color.info.pixel_format == vk::Format::eA2B10G10R10UnormPack32
                            ? *overlay_rgb10a2_pipeline : *overlay_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                         &push);
    // The previous frame buffer was written by the last dispatch.
    const vk::MemoryBarrier2 frame_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &frame_barrier});
    cmdbuf.dispatch((color.info.size.width + 7) / 8, (color.info.size.height + 7) / 8, 1);

    scheduler.DeferOperation([device, depth_view, color_view] {
        device.destroyImageView(depth_view);
        device.destroyImageView(color_view);
    });
}

} // namespace Vulkan
