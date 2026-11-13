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

} // namespace AmdGpu
