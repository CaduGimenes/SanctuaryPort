// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include "video_core/renderer_vulkan/gtao_cache_policy.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vertex_fetch_window.h"

namespace {

void CheckCacheWarmup(Vulkan::GtaoCachePolicy& policy) {
    for (int frame = 0; frame < 11; ++frame) {
        assert(!policy.Update(true));
    }
    assert(policy.Update(true));
    assert(policy.Update(true));
}

void TestAdaptiveGtao() {
    Vulkan::GtaoCachePolicy policy;
    assert(!policy.Update(false));
    for (int frame = 0; frame < 11; ++frame) {
        assert(!policy.Update(true));
    }
    assert(!policy.Update(false)); // Projection, preset, or depth sample mode changed.
    CheckCacheWarmup(policy);
    assert(!policy.Update(false)); // Drop an already active cache.
    CheckCacheWarmup(policy);
    policy.Reset(); // AO disabled or history targets recreated.
    CheckCacheWarmup(policy);
}

void TestVertexFetchWindow() {
    using Vulkan::VertexFetchBytes;
    using Vulkan::Motion::IndexedRange;

    const std::array<uint16_t, 3> indices{105, 100, 107};
    const auto vertices = IndexedRange(std::span{indices}, -4, false);
    const auto bytes = VertexFetchBytes(vertices.first, vertices.count, 32, 12, 8192);
    assert(bytes && bytes->start == 3072 && bytes->end == 3308);
    assert(IndexedRange(std::span{indices}, -101, false).count == 0);

    const auto end = VertexFetchBytes(4, 1, 16, 8, 72);
    assert(end && end->start == 64 && end->end == 72);
    assert(!VertexFetchBytes(4, 1, 16, 8, 71));
    assert(!VertexFetchBytes(0, 0, 16, 8, 64));
    assert(!VertexFetchBytes(0, 1, 0, 8, 64));
    assert(!VertexFetchBytes(0, 1, 4, 8, 64));
    assert(!VertexFetchBytes(UINT32_MAX, UINT32_MAX, UINT32_MAX, 1, UINT32_MAX));
}

} // namespace

int main() {
    TestAdaptiveGtao();
    TestVertexFetchWindow();
}
