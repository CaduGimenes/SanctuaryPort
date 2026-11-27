// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace Libraries::Http::D3 {

enum class AuthRouteResult { Unchanged, Routed, InvalidEndpoint };

// Only the title-auth hosts in this executable are private-backend routes. The
// path and body are left to libhttp unchanged; no on-disk game URL is patched.
inline bool IsAuthHost(std::string_view host) {
    return host == "ammod-ps4-auth3.dev.demonware.net" ||
           host == "ammod-ps4-auth3.cert.demonware.net" ||
           host == "ammod-ps4-auth3.prod.demonware.net";
}

inline AuthRouteResult RouteAuth(bool enabled, std::string_view serial,
                                  std::string_view endpoint, std::string& scheme,
                                  std::string& host, std::uint16_t& port, bool& secure) {
    if (!enabled || serial != "CUSA00242" || !IsAuthHost(host)) {
        return AuthRouteResult::Unchanged;
    }

    const auto separator = endpoint.find("://");
    if (separator == std::string_view::npos) return AuthRouteResult::InvalidEndpoint;
    const auto target_scheme = endpoint.substr(0, separator);
    if (target_scheme != "http" && target_scheme != "https") {
        return AuthRouteResult::InvalidEndpoint;
    }
    auto authority = endpoint.substr(separator + 3);
    if (authority.ends_with('/')) authority.remove_suffix(1);
    if (authority.empty() || authority.find_first_of("/@?#\\\r\n\t ") != std::string_view::npos) {
        return AuthRouteResult::InvalidEndpoint;
    }
    const auto colon = authority.find(':');
    const auto target_host = authority.substr(0, colon);
    if (target_host.empty()) return AuthRouteResult::InvalidEndpoint;
    for (const char c : target_host) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) {
            return AuthRouteResult::InvalidEndpoint;
        }
    }
    unsigned target_port = target_scheme == "https" ? 443 : 80;
    if (colon != std::string_view::npos) {
        const auto text = authority.substr(colon + 1);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), target_port);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            target_port == 0 || target_port > 65535) {
            return AuthRouteResult::InvalidEndpoint;
        }
    }
    scheme = target_scheme;
    host = target_host;
    port = static_cast<std::uint16_t>(target_port);
    secure = scheme == "https";
    return AuthRouteResult::Routed;
}

} // namespace Libraries::Http::D3
