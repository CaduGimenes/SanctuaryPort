// SPDX-License-Identifier: GPL-2.0-or-later
// Private settings for the native networking bridge. Renderer settings stay separate.
#pragma once
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <string>

class BbNetSettingsImpl {
public:
    static BbNetSettingsImpl* GetInstance() { static BbNetSettingsImpl s; return &s; }
    static bool Flag(const char* name, bool fallback) {
        const char* v = std::getenv(name);
        return v ? v[0] == '1' : fallback;
    }
    static std::string Text(const char* name) {
        const char* v = std::getenv(name);
        return v ? v : "";
    }
    bool IsConnectedToNetwork() const { return Flag("BB_ONLINE", false); }
    bool IsShadNetEnabled() const { return IsConnectedToNetwork() && !shadnet_session_disabled.load(); }
    void SetShadNetSessionDisabled(bool v) { shadnet_session_disabled.store(v); }
    std::string GetShadNetServer() const { return Text("BB_SHADNET_SERVER"); }
    std::string GetShadNetHost() const {
        const std::string server = GetShadNetServer();
        const auto colon = server.rfind(':');
        return colon == std::string::npos ? server : server.substr(0, colon);
    }
    // The Demonware services may run on a different host from the PSN master.
    // An unset value keeps the single-host layout; a set empty value is invalid.
    std::string GetD3DemonwareHost() const {
        if (const char* value = std::getenv("BB_D3_DEMONWARE_HOST")) return value;
        return GetShadNetHost();
    }
    std::string GetShadNetWebApiServer() const {
        // An explicit value (including an empty value) overrides the private
        // master's conventional HTTP port. No game URL or file is changed.
        if (const char* value = std::getenv("BB_SHADNET_WEBAPI")) return value;
        if (!Flag("BB_D3_MASTER", false)) return {};
        const std::string server = GetShadNetServer();
        const auto colon = server.rfind(':');
        if (colon == std::string::npos || colon == 0) return {};
        unsigned port = 0;
        const auto parsed = std::from_chars(server.data()+colon+1,
                                             server.data()+server.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != server.data()+server.size() ||
            port == 0 || port > 65533) return {};
        return "http://" + server.substr(0, colon+1) + std::to_string(port+2);
    }
    // The Demonware title-auth endpoint may live in a separate server process.
    // An unset value keeps the pre-split single-endpoint layout; an explicit
    // empty value disables private title authentication.
    std::string GetD3TitleAuthServer() const {
        if (const char* value = std::getenv("BB_D3_TITLEAUTH")) return value;
        return GetShadNetWebApiServer();
    }
    bool IsUPnPEnabled() const { return Flag("BB_UPNP", false); }
    /// shadPS4's disable_https: HTTPS requests the game makes go out as plain HTTP.
    bool IsForcedHttpsDisabled() const { return Flag("BB_DISABLE_HTTPS", false); }
    /// The UDP port P2P traffic uses: 3658, the PS4's, unless BB_P2P_PORT (0: any free port).
    int GetP2PPort() const {
        const char* v = std::getenv("BB_P2P_PORT");
        return v && *v ? std::atoi(v) : 3658;
    }
private:
    std::atomic<bool> shadnet_session_disabled{false};
};
#define EmulatorSettings (*BbNetSettingsImpl::GetInstance())
