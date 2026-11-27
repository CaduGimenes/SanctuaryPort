// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace BbD3 {
inline std::uint16_t PortFromEnvironment(const char* name, std::uint16_t fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    const std::string_view text(value);
    unsigned port = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), port);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        port == 0 || port > 65535) return fallback;
    return static_cast<std::uint16_t>(port);
}
} // namespace BbD3
