// SPDX-License-Identifier: GPL-2.0-or-later
// sanctuary: guest multisampled images created and sampled as single-sampled ones.
//
// Some games draw with one color fragment and two depth samples, and its shaders fetch
// that depth buffer (D32S8, 2 samples) as a multisampled texture. On the RX 6600 with AMD's
// Windows driver the fetch returns zero in the lower half of many 8x8 pixel tiles, the ones
// with little depth variation (it looks like compressed depth that was not expanded). That
// shows as black lines 8 px wide and 4 px tall wherever depth is used (lighting, fog, bloom).
// Sampling in the General layout does not help, so the images lose their extra samples
// instead: the depth test and the shaders then use the same single-sampled buffer. Guest
// sizes and addresses still use the real sample count (GuestNumSamples).
//
// On with AMD's and Intel's Windows drivers (sanctuary_msaa_default.h); SANCTUARY_NO_MSAA=0 or 1 decides
// otherwise (vk_instance.cpp). SANCTUARY_NO_MSAA=0 on those drivers is experimental: the multisampled
// depth is then created uncompressed where the driver allows it (MsaaUncompressedDepth).

#pragma once

#include <atomic>

namespace AmdGpu {

inline std::atomic<bool> no_msaa{false};

inline bool NoMsaa() {
    return no_msaa.load(std::memory_order_relaxed);
}
/// Experimental: multisampling kept on (SANCTUARY_NO_MSAA=0) with a driver that reads compressed
/// multisampled depth wrong: those depth images are created without compression
/// (VK_EXT_image_compression_control), the suspected cause of the black lines.
inline std::atomic<bool> msaa_uncompressed_depth{false};
inline bool MsaaUncompressedDepth() {
    return msaa_uncompressed_depth.load(std::memory_order_relaxed);
}

/// SANCTUARY_MSAA_RESOLVE=1 (launcher Graphics > Game MSAA, any count but the game's own): a
/// multisampled color image a shader samples is read from a copy whose every sample holds the mean
/// of its samples. Diablo III copies its scene for the post passes with one texel fetch of sample
/// 0, so without it the extra samples never reach the picture; its PC version resolves the same
/// targets with ResolveSubresource. Set by vk_instance.cpp; depth images are left alone.
inline std::atomic<bool> msaa_resolve{false};
inline bool MsaaResolve() {
    return msaa_resolve.load(std::memory_order_relaxed);
}
/// Whether a sampled image is read from its averaged copy: color only (Diablo III's SSAO and depth
/// copy read sample 0 of the depth, which an average would move), and not while the image's
/// storage is a stand-in of another sample count (Runtime::SetBackingSamples).
inline bool ReadsSampleMean(bool is_depth, unsigned host_samples, unsigned image_samples) {
    return MsaaResolve() && !is_depth && host_samples > 1 && host_samples == image_samples;
}

} // namespace AmdGpu
