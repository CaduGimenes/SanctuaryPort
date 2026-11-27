// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <openssl/hmac.h>
#include <string_view>
#include <vector>
#include "common/types.h"

namespace ShadNet {
// Authenticate UDP publication without exposing the login bearer token.
inline bool SignUdp(std::vector<u8>& packet, std::string_view token) {
    if (token.size() != 64) return false;
    const u64 timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (int shift = 56; shift >= 0; shift -= 8) packet.push_back(u8(timestamp >> shift));
    u8 signature[EVP_MAX_MD_SIZE]; unsigned length = 0;
    if (!HMAC(EVP_sha256(), token.data(), int(token.size()), packet.data(), packet.size(),
              signature, &length) || length != 32) return false;
    packet.insert(packet.end(), signature, signature + length);
    return true;
}
}
