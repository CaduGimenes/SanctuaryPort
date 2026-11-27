// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <optional>

namespace Vulkan {

struct VertexFetchWindow {
    uint32_t start;
    uint32_t end;
};

[[nodiscard]] constexpr std::optional<VertexFetchWindow> VertexFetchBytes(
    uint32_t first, uint32_t count, uint32_t stride, uint32_t width, uint32_t size) {
    if (count == 0 || stride == 0 || width == 0 || width > stride || width > size) {
        return std::nullopt;
    }
    const uint64_t last = uint64_t(first) + count - 1;
    if (last > (size - width) / stride) {
        return std::nullopt;
    }
    return VertexFetchWindow{first * stride, uint32_t(last) * stride + width};
}

} // namespace Vulkan
