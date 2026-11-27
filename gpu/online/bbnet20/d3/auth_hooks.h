// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include "d3/identity.h"

namespace BbD3 {
inline bool VerifyAuthResponse(std::string_view body, std::string_view signature,
                               std::string_view bearer) {
    if (body.empty() || body.size() >= 0x1400 || signature.size() != 64 ||
        !ValidSessionBearer(bearer)) return false;
    std::array<unsigned char, 32> expected{}, supplied{};
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < supplied.size(); ++i) {
        const int hi = digit(signature[2*i]), lo = digit(signature[2*i+1]);
        if (hi < 0 || lo < 0) return false;
        supplied[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    unsigned size = 0;
    return HMAC(EVP_sha256(), bearer.data(), static_cast<int>(bearer.size()),
                reinterpret_cast<const unsigned char*>(body.data()), body.size(),
                expected.data(), &size) && size == expected.size() &&
           CRYPTO_memcmp(expected.data(), supplied.data(), expected.size()) == 0;
}

inline constexpr std::size_t AuthVerifierOffset = 0xde4db0;
inline constexpr std::array<std::uint8_t, 64> AuthVerifierBytes = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xe4, 0xe0, 0x48, 0x81, 0xec,
    0xe0, 0x02, 0x00, 0x00, 0x4c, 0x8b, 0x3d, 0xfd, 0x37, 0x62, 0x00, 0xc5, 0xfc, 0x57, 0xc0, 0x49,
    0x89, 0xfe, 0x48, 0x8d, 0x35, 0x02, 0x04, 0x33, 0x00, 0x48, 0x8d, 0x94, 0x24, 0x60, 0x01, 0x00,
    0x00, 0xb9, 0x59, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x07, 0x48, 0x89, 0x84, 0x24, 0xd0, 0x02, 0x00
};

inline constexpr std::size_t BootstrapPortOffset = 0xe75ddc;
inline constexpr std::array<std::uint8_t, 6> BootstrapPortBytes = {0x66, 0xc7, 0x43, 0x20, 0x02, 0x0c};
inline constexpr std::uint16_t PrivateBootstrapPort = 31317;

// Called while the runtime's mapped image is writable, before guest execution.
// No file is opened or modified. A different title/build/byte pattern is refused.
inline bool PatchOnlineImage(std::uint8_t* image, std::uint64_t size,
                              std::string_view serial, std::string_view version,
                              std::uintptr_t verifier, std::uint16_t bootstrap_port = PrivateBootstrapPort) {
    if (!image || !verifier || !bootstrap_port || serial != "CUSA00242" || version != "01.46" ||
        size < AuthVerifierOffset + AuthVerifierBytes.size() ||
        size < BootstrapPortOffset + BootstrapPortBytes.size() ||
        std::memcmp(image + AuthVerifierOffset, AuthVerifierBytes.data(), AuthVerifierBytes.size()) ||
        std::memcmp(image + BootstrapPortOffset, BootstrapPortBytes.data(), BootstrapPortBytes.size())) return false;
    // Check both sites before any write. Change only the remote discovery
    // port; the game's local socket still uses its native gameplay port.
    std::array<std::uint8_t, 14> jump = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(jump.data() + 6, &verifier, sizeof(verifier));
    std::memcpy(image + AuthVerifierOffset, jump.data(), jump.size());
    image[BootstrapPortOffset + 4] = bootstrap_port & 0xff;
    image[BootstrapPortOffset + 5] = bootstrap_port >> 8;
    return true;
}
using MemoryCheck = int (*)(std::uintptr_t, std::uint64_t);
void InstallAuthHooks(std::uint8_t* image, std::uint64_t size, const char* serial,
                       const char* version, MemoryCheck memory_check);
bool AuthHooksReady();
} // namespace BbD3
