// SPDX-License-Identifier: GPL-2.0-or-later
// Optional verified TLS for the D3 master. Existing shadNet servers keep their
// original transport unless BB_SHADNET_TLS=1 is explicitly selected.
#pragma once
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace ShadNet {
class TlsSocket {
public:
    static bool Enabled() {
        const char* value = std::getenv("BB_SHADNET_TLS");
        return value && value[0] == '1';
    }
    ~TlsSocket() { if (ssl_) SSL_free(ssl_); if (ctx_) SSL_CTX_free(ctx_); }
    bool Connect(ShadSocketHandle socket, const std::string& host) {
        socket_ = socket;
        write_socket_ = socket;
        return ConnectImpl(host, false);
    }
#ifndef _WIN32
    // Pipe-backed interoperability tests exercise the same TLS and verification
    // path in sandboxes that prohibit creating network sockets.
    bool ConnectPipes(int input, int output, const std::string& host) {
        socket_ = input;
        write_socket_ = output;
        return ConnectImpl(host, true);
    }
#endif
private:
    bool ConnectImpl(const std::string& host, bool pipes) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) return false;
        SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        const char* ca = std::getenv("BB_SHADNET_CA");
        if (ca && *ca) {
            if (SSL_CTX_load_verify_locations(ctx_, ca, nullptr) != 1) return false;
        } else if (SSL_CTX_set_default_verify_paths(ctx_) != 1) return false;
        ssl_ = SSL_new(ctx_);
        if (!ssl_) return false;
#ifndef _WIN32
        if (pipes) {
            BIO* input = BIO_new_fd(socket_, BIO_NOCLOSE);
            BIO* output = BIO_new_fd(write_socket_, BIO_NOCLOSE);
            if (!input || !output) { BIO_free(input); BIO_free(output); return false; }
            SSL_set_bio(ssl_, input, output);
        } else
#endif
        if (SSL_set_fd(ssl_, static_cast<int>(socket_)) != 1) return false;
        in_addr address{};
        if (inet_pton(AF_INET, host.c_str(), &address) == 1) {
            if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl_), host.c_str()) != 1) return false;
        } else if (SSL_set_tlsext_host_name(ssl_, host.c_str()) != 1 ||
                   SSL_set1_host(ssl_, host.c_str()) != 1) return false;
#ifdef _WIN32
        u_long nonblock = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblock)) return false;
#else
        int flags = fcntl(socket_, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK)) return false;
        if (write_socket_ != socket_) {
            flags = fcntl(write_socket_, F_GETFL, 0);
            if (flags < 0 || fcntl(write_socket_, F_SETFL, flags | O_NONBLOCK)) return false;
        }
#endif
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            int r = SSL_connect(ssl_);
            if (r == 1) return SSL_get_verify_result(ssl_) == X509_V_OK;
            int error = SSL_get_error(ssl_, r);
            if (!Wait(error, deadline)) return false;
        }
    }
public:
    int Read(void* data, int length, bool handshake = false) {
        return Transfer(data, length, false, handshake);
    }
    int Write(const void* data, int length) {
        return Transfer(const_cast<void*>(data), length, true, false);
    }
private:
    int Transfer(void* data, int length, bool writing, bool handshake) {
        const auto deadline = handshake ? std::chrono::steady_clock::now() + std::chrono::seconds(10)
                                        : std::chrono::steady_clock::time_point::max();
        for (;;) {
            int r, error;
            { // OpenSSL objects are serialized; never hold the mutex while waiting for I/O.
                std::lock_guard lock(mutex_);
                r = writing ? SSL_write(ssl_, data, length) : SSL_read(ssl_, data, length);
                error = r > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl_, r);
            }
            if (r > 0) return r;
            if (!Wait(error, deadline)) return -1;
        }
    }
    bool Wait(int error, std::chrono::steady_clock::time_point deadline) {
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) return false;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto fd = error == SSL_ERROR_WANT_READ ? socket_ : write_socket_;
            fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
            timeval timeout{0, 250000};
            int r = select(static_cast<int>(fd) + 1,
                           error == SSL_ERROR_WANT_READ ? &fds : nullptr,
                           error == SSL_ERROR_WANT_WRITE ? &fds : nullptr, nullptr, &timeout);
            if (r > 0) return true;
            if (r < 0) return false;
        }
        return false;
    }
    ShadSocketHandle socket_ = SHAD_INVALID_SOCK;
    ShadSocketHandle write_socket_ = SHAD_INVALID_SOCK;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    std::mutex mutex_;
};
} // namespace ShadNet
