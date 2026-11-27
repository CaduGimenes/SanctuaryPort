// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <string_view>
#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace BbD3 {
enum class PeerRouteResult { Unchanged, Routed, InvalidEndpoint };

inline bool IsLobbyHost(std::string_view host) {
    return host == "ammod-ps4-lobby.dev.demonware.net" ||
           host == "ammod-ps4-lobby.cert.demonware.net" ||
           host == "ammod-ps4-lobby.prod.demonware.net";
}

inline bool IsBootstrapHost(std::string_view host) {
    return IsLobbyHost(host) ||
           host == "stun.us.demonware.net" || host == "stun.eu.demonware.net" ||
           host == "stun.jp.demonware.net" || host == "stun.au.demonware.net";
}

// Only this title's lobby/bootstrap DNS names are replaced. The separate
// mapped-image hook selects discovery's port; TCP routing below selects the
// lobby port. Gameplay peer addresses and ports retain their native values.
// The Demonware services can live on their own host; only the ports are
// configured separately.
inline PeerRouteResult RouteBootstrapHost(bool enabled, std::string_view serial,
                                          std::string_view version, std::string_view demonware,
                                          std::string& host) {
    if (!enabled || serial != "CUSA00242" || version != "01.46" || !IsBootstrapHost(host))
        return PeerRouteResult::Unchanged;
    if (demonware.empty()) return PeerRouteResult::InvalidEndpoint;
    for (const char c : demonware) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
            return PeerRouteResult::InvalidEndpoint;
    }
    host = std::string(demonware);
    return PeerRouteResult::Routed;
}

class LobbyEndpoints {
public:
    void Remember(std::uint32_t address) {
        std::lock_guard lock(mutex);
        // A bounded cache for the private lobby's DNS results (normally one).
        if (addresses.size() >= 64 && !addresses.contains(address)) addresses.clear();
        addresses.insert(address);
    }
    std::uint16_t Port(bool enabled, std::string_view serial, std::string_view version,
                       bool stream, std::uint32_t address, std::uint16_t original,
                       std::uint16_t configured) {
        if (!enabled || serial != "CUSA00242" || version != "01.46" ||
            !stream || original != 3074 || !configured) return original;
        std::lock_guard lock(mutex);
        return addresses.contains(address) ? configured : original;
    }
private:
    std::mutex mutex;
    std::unordered_set<std::uint32_t> addresses;
};

inline LobbyEndpoints& PrivateLobbyEndpoints() {
    static LobbyEndpoints endpoints;
    return endpoints;
}
} // namespace BbD3
