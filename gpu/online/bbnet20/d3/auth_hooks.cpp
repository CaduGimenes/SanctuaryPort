// SPDX-License-Identifier: GPL-2.0-or-later
#include "d3/auth_hooks.h"
#include "d3/ports.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "common/types.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/libraries/network/d3_auth_route.h"
#include "core/libraries/np/np_handler/np_handler.h"

namespace BbD3 {
namespace {
std::atomic<bool> ready{false};
MemoryCheck mapped = nullptr;
std::uintptr_t image_base = 0;
std::uint64_t image_size = 0;

bool Readable(std::uintptr_t at, std::uint64_t size) {
    if (!mapped || !at || at > UINTPTR_MAX - size) return false;
    if (mapped(at, size)) return true;
    // Static game vtables live in the runtime's low image mappings rather
    // than its direct/flexible-memory VMA table.
    int prot = 0;
    std::uintptr_t end = 0;
    return runtime_memory_low_info(at, &prot, &end) && end >= at &&
           size <= end - at && (prot & 1);
}

bool PS4_SYSV_ABI VerifyPrivateResponse(void* object) {
    if (!ready.load(std::memory_order_acquire)) return false;
    const auto auth = reinterpret_cast<std::uintptr_t>(object);
    if (!Readable(auth, 0x1574)) return false;
    std::uintptr_t http = 0, table = 0, entry = 0;
    std::memcpy(&http, reinterpret_cast<const void*>(auth + 0x160), sizeof(http));
    if (!Readable(http, sizeof(table))) return false;
    std::memcpy(&table, reinterpret_cast<const void*>(http), sizeof(table));
    if (!Readable(table, 0x30)) return false;
    std::memcpy(&entry, reinterpret_cast<const void*>(table + 0x28), sizeof(entry));
    if (entry < image_base || entry >= image_base + image_size) return false;
    using GetHeader = bool (PS4_SYSV_ABI *)(void*, const char*, char*, std::uint32_t);
    char signature[65] = {};
    if (!reinterpret_cast<GetHeader>(entry)(reinterpret_cast<void*>(http), "X-D3-Signature",
                                            signature, sizeof(signature))) return false;
    const auto* body = reinterpret_cast<const char*>(auth + 0x174);
    const std::size_t length = strnlen(body, 0x1400);
    const auto bearer = Libraries::Np::NpHandler::GetInstance().GetBearerToken(1);
    const bool valid = VerifyAuthResponse({body, length},
                                          {signature, strnlen(signature, sizeof(signature))}, bearer);
    if (!valid) std::printf("D3 online: private title-auth response signature rejected\n");
    return valid;
}
} // namespace

bool AuthHooksReady() { return ready.load(std::memory_order_acquire); }

void InstallAuthHooks(std::uint8_t* image, std::uint64_t size, const char* serial,
                       const char* version, MemoryCheck memory_check) {
    if (!BbNetSettingsImpl::Flag("BB_D3_MASTER", false) ||
        !BbNetSettingsImpl::Flag("BB_D3_DEMONWARE", false)) {
        std::printf("D3 online: auth hook skipped (BB_D3_MASTER/BB_D3_DEMONWARE not both 1)\n");
        return;
    }
    // Validate the same origin used by libhttp before permitting private tokens.
    std::string scheme, host = "ammod-ps4-auth3.prod.demonware.net";
    std::uint16_t port = 443;
    bool secure = true;
    const auto bootstrap_port = PortFromEnvironment("BB_D3_STUN_PORT", PrivateBootstrapPort);
    const std::string endpoint = EmulatorSettings.GetD3TitleAuthServer();
    const auto route = Libraries::Http::D3::RouteAuth(true, serial ? serial : "", endpoint,
                                                      scheme, host, port, secure);
    if (!memory_check || route != Libraries::Http::D3::AuthRouteResult::Routed) {
        std::printf("D3 online: title-auth hook refused: serial=%s version=%s endpoint='%s' route=%d (need CUSA00242 01.46 and a private endpoint)\n",
                    serial ? serial : "", version ? version : "", endpoint.c_str(), int(route));
        return;
    }
    if (!PatchOnlineImage(image, size, serial ? serial : "", version ? version : "",
                          reinterpret_cast<std::uintptr_t>(&VerifyPrivateResponse), bootstrap_port)) {
        std::printf("D3 online: image patch refused: serial=%s version=%s image=%llu bytes (byte pattern or size mismatch)\n",
                    serial ? serial : "", version ? version : "",
                    static_cast<unsigned long long>(size));
        return;
    }
    mapped = memory_check;
    image_base = reinterpret_cast<std::uintptr_t>(image);
    image_size = size;
    ready.store(true, std::memory_order_release);
    std::printf("D3 online: private title-auth verifier and UDP bootstrap port %u installed in mapped CUSA00242 01.46 image\n",
                static_cast<unsigned>(bootstrap_port));
}
} // namespace BbD3
