// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace BbD3 {

inline std::string Base64Url(std::string_view input) {
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) - 2) return {};
    std::string encoded(((input.size() + 2) / 3) * 4 + 1, '\0');
    const int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                       reinterpret_cast<const unsigned char*>(input.data()),
                                       static_cast<int>(input.size()));
    if (length < 0) return {};
    encoded.resize(static_cast<std::size_t>(length));
    for (char& c : encoded) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
}

inline bool ValidOnlineName(std::string_view name) {
    const auto letter = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    if (name.size() < 3 || name.size() > 16 || !letter(name.front())) return false;
    for (const char c : name) {
        if (!letter(c) && !(c >= '0' && c <= '9') && c != '_' && c != '-') return false;
    }
    return true;
}

inline bool ValidSessionBearer(std::string_view bearer) {
    if (bearer.size() != 64) return false;
    for (const char c : bearer) {
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') &&
            !(c >= 'A' && c <= 'F')) return false;
    }
    return true;
}

// Private-master identity, not a Sony ID token. The active master session is the
// issuer's credential and the HS256 key; its TCP-session lifetime still applies.
// No password, certificate, persistent user record or game file is involved.
inline std::string BuildIdToken(std::uint64_t account_id, std::string_view name,
                                 std::string_view bearer, std::int64_t now_unix) {
    if (account_id == 0 || account_id > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        !ValidOnlineName(name) || !ValidSessionBearer(bearer) || now_unix < 0 ||
        now_unix > std::numeric_limits<std::int64_t>::max() - 900) return {};

    std::string identity = "d3pc:";
    identity.append(name);
    std::array<unsigned char, 32> hash{};
    unsigned int hash_size = 0;
    if (EVP_Digest(identity.data(), identity.size(), hash.data(), &hash_size,
                   EVP_sha256(), nullptr) != 1 || hash_size != hash.size()) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string duid(64, '0');
    for (std::size_t i = 0; i < hash.size(); ++i) {
        duid[i * 2] = hex[hash[i] >> 4];
        duid[i * 2 + 1] = hex[hash[i] & 15];
    }

    const nlohmann::json header = {{"alg", "HS256"}, {"typ", "JWT"}};
    const nlohmann::json claims = {{"iss", "d3pc"}, {"sub", std::to_string(account_id)},
                                   {"name", std::string(name)}, {"d3pc_session", std::string(bearer)},
                                   {"env_iss_id", 256}, {"duid", duid}, {"iat", now_unix},
                                   {"exp", now_unix + 900}};
    std::string token = Base64Url(header.dump());
    token += '.';
    token += Base64Url(claims.dump());
    std::array<unsigned char, 32> signature{};
    unsigned int signature_size = 0;
    if (!HMAC(EVP_sha256(), bearer.data(), static_cast<int>(bearer.size()),
               reinterpret_cast<const unsigned char*>(token.data()), token.size(),
               signature.data(), &signature_size) || signature_size != signature.size()) return {};
    token += '.';
    token += Base64Url(std::string_view(reinterpret_cast<const char*>(signature.data()), signature.size()));
    return token.size() < 4097 ? token : std::string{};
}

} // namespace BbD3
