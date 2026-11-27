// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/network/net_ctl_codes.h"
#include "core/libraries/network/net_ctl_obj.h"
#include "core/tls.h"

namespace Libraries::NetCtl {

NetCtlInternal::NetCtlInternal() = default;

NetCtlInternal::~NetCtlInternal() = default;

s32 NetCtlInternal::RegisterCallback(OrbisNetCtlCallback func, void* arg) {
    std::scoped_lock lock{m_mutex};

    // Find the next available slot
    const auto it = std::ranges::find(callbacks, nullptr, &NetCtlCallback::func);
    if (it == callbacks.end()) {
        return ORBIS_NET_CTL_ERROR_CALLBACK_MAX;
    }

    const int next_id = std::distance(callbacks.begin(), it);
    callbacks[next_id].func = func;
    callbacks[next_id].arg = arg;
    return next_id;
}

s32 NetCtlInternal::RegisterNpToolkitCallback(OrbisNetCtlCallbackForNpToolkit func, void* arg) {
    std::scoped_lock lock{m_mutex};

    // Find the next available slot
    const auto it = std::ranges::find(nptool_callbacks, nullptr, &NetCtlCallbackForNpToolkit::func);
    if (it == nptool_callbacks.end()) {
        return ORBIS_NET_CTL_ERROR_CALLBACK_MAX;
    }

    const int next_id = std::distance(nptool_callbacks.begin(), it);
    nptool_callbacks[next_id].func = func;
    nptool_callbacks[next_id].arg = arg;
    nptool_callbacks[next_id].delivered_event = 0;
    return next_id;
}

s32 NetCtlInternal::UnregisterNpToolkitCallback(int cid) {
    if (cid < 0 || cid >= static_cast<int>(nptool_callbacks.size()))
        return ORBIS_NET_CTL_ERROR_INVALID_ID;
    std::scoped_lock lock{m_mutex};
    if (!nptool_callbacks[cid].func) return ORBIS_NET_CTL_ERROR_ID_NOT_FOUND;
    nptool_callbacks[cid] = {};
    return 0;
}

void NetCtlInternal::CheckCallback() {
    std::scoped_lock lock{m_mutex};
    const auto event = EmulatorSettings.IsConnectedToNetwork()
                           ? ORBIS_NET_CTL_EVENT_TYPE_IPOBTAINED
                           : ORBIS_NET_CTL_EVENT_TYPE_DISCONNECTED;
    for (const auto [func, arg] : callbacks) {
        if (func != nullptr) {
            func(event, arg);
        }
    }
}

void NetCtlInternal::CheckNpToolkitCallback() {
    const auto event = EmulatorSettings.IsConnectedToNetwork()
                           ? ORBIS_NET_CTL_EVENT_TYPE_IPOBTAINED
                           : ORBIS_NET_CTL_EVENT_TYPE_DISCONNECTED;
    for (size_t i = 0; i < nptool_callbacks.size(); ++i) {
        OrbisNetCtlCallbackForNpToolkit func = nullptr;
        void* arg = nullptr;
        {
            std::scoped_lock lock{m_mutex};
            auto& entry = nptool_callbacks[i];
            if (entry.func && entry.delivered_event != event) {
                entry.delivered_event = event;
                func = entry.func;
                arg = entry.arg;
            }
        }
        // Guest code may unregister itself or another listener here.
        if (func) func(event, arg);
    }
}

} // namespace Libraries::NetCtl
