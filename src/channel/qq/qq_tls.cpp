#include "channel/qq/qq_tls.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

namespace lubancode::channel::qq {

namespace {

int MbedSend(void* ctx, const unsigned char* buf, std::size_t len) {
    auto* sock = static_cast<TcpSocket*>(ctx);
    const auto result = sock->WriteAll(
        std::string_view(reinterpret_cast<const char*>(buf), len), 10'000);
    if (!result.has_value()) {
        switch (result.error().kind) {
            case SocketErrorKind::Timeout:
                return MBEDTLS_ERR_SSL_TIMEOUT;
            default:
                return MBEDTLS_ERR_NET_SEND_FAILED;
        }
    }
    return static_cast<int>(len);
}

int MbedRecv(void* ctx, unsigned char* buf, std::size_t len) {
    auto* sock = static_cast<TcpSocket*>(ctx);
    const auto result = sock->ReadSome(reinterpret_cast<char*>(buf), len, 10'000);
    if (!result.has_value()) {
        switch (result.error().kind) {
            case SocketErrorKind::Timeout:
                return MBEDTLS_ERR_SSL_TIMEOUT;
            case SocketErrorKind::Closed:
                return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
            default:
                return MBEDTLS_ERR_NET_RECV_FAILED;
        }
    }
    return static_cast<int>(*result);
}

int MbedRecvTimeout(void* ctx, unsigned char* buf, std::size_t len, std::uint32_t timeout_ms) {
    auto* sock = static_cast<TcpSocket*>(ctx);
    const auto result =
        sock->ReadSome(reinterpret_cast<char*>(buf), len, static_cast<int>(timeout_ms));
    if (!result.has_value()) {
        switch (result.error().kind) {
            case SocketErrorKind::Timeout:
                return MBEDTLS_ERR_SSL_TIMEOUT;
            case SocketErrorKind::Closed:
                return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
            default:
                return MBEDTLS_ERR_NET_RECV_FAILED;
        }
    }
    return static_cast<int>(*result);
}

std::string MbedErrorText(int code) {
    char buf[256] = {0};
    mbedtls_strerror(code, buf, sizeof(buf));
    return std::string(buf) + " (" + std::to_string(code) + ")";
}

SocketErrorKind ToSocketKind(int mbed_code) {
    if (mbed_code == MBEDTLS_ERR_SSL_TIMEOUT) {
        return SocketErrorKind::Timeout;
    }
    if (mbed_code == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
        mbed_code == MBEDTLS_ERR_SSL_CONN_EOF ||
        mbed_code == MBEDTLS_ERR_NET_CONN_RESET) {
        return SocketErrorKind::Closed;
    }
    return SocketErrorKind::Failed;
}

}  // namespace

std::string DetectSystemCaPemPath() {
    static const char* kCandidates[] = {
        "/etc/ssl/cert.pem",                            // macOS / 部分 Linux
        "/etc/ssl/certs/ca-certificates.crt",           // Debian 系
        "/etc/pki/tls/certs/ca-bundle.crt",             // RHEL 系
        "/etc/openssl/cert.pem",                        // FreeBSD
    };
    for (const char* path : kCandidates) {
        std::FILE* probe = std::fopen(path, "rb");
        if (probe != nullptr) {
            std::fclose(probe);
            return path;
        }
    }
    return std::string();
}

TlsClientStream::~TlsClientStream() { Cleanup(); }

TlsClientStream::TlsClientStream(TlsClientStream&& other) noexcept
    : ssl_(other.ssl_),
      config_(other.config_),
      ca_(other.ca_),
      drbg_(other.drbg_),
      entropy_(other.entropy_),
      sock_(other.sock_) {
    other.ssl_ = nullptr;
    other.config_ = nullptr;
    other.ca_ = nullptr;
    other.drbg_ = nullptr;
    other.entropy_ = nullptr;
    other.sock_ = nullptr;
}

TlsClientStream& TlsClientStream::operator=(TlsClientStream&& other) noexcept {
    if (this != &other) {
        Cleanup();
        ssl_ = other.ssl_;
        config_ = other.config_;
        ca_ = other.ca_;
        drbg_ = other.drbg_;
        entropy_ = other.entropy_;
        sock_ = other.sock_;
        other.ssl_ = nullptr;
        other.config_ = nullptr;
        other.ca_ = nullptr;
        other.drbg_ = nullptr;
        other.entropy_ = nullptr;
        other.sock_ = nullptr;
    }
    return *this;
}

void TlsClientStream::Cleanup() {
    if (ssl_ != nullptr) {
        mbedtls_ssl_free(ssl_);
        delete ssl_;
        ssl_ = nullptr;
    }
    if (config_ != nullptr) {
        mbedtls_ssl_config_free(config_);
        delete config_;
        config_ = nullptr;
    }
    if (ca_ != nullptr) {
        mbedtls_x509_crt_free(ca_);
        delete ca_;
        ca_ = nullptr;
    }
    if (drbg_ != nullptr) {
        mbedtls_ctr_drbg_free(drbg_);
        delete drbg_;
        drbg_ = nullptr;
    }
    if (entropy_ != nullptr) {
        mbedtls_entropy_free(entropy_);
        delete entropy_;
        entropy_ = nullptr;
    }
    sock_ = nullptr;
}

std::expected<TlsClientStream, TlsError> TlsClientStream::Connect(TcpSocket* sock,
                                                                  const std::string& host,
                                                                  const std::string& ca_pem,
                                                                  int handshake_timeout_ms) {
    if (sock == nullptr || !sock->valid()) {
        return std::unexpected(TlsError{TlsErrorKind::Failed, "socket not connected"});
    }

    auto* ssl = new mbedtls_ssl_context();
    auto* config = new mbedtls_ssl_config();
    auto* ca = new mbedtls_x509_crt();
    auto* drbg = new mbedtls_ctr_drbg_context();
    auto* entropy = new mbedtls_entropy_context();
    mbedtls_ssl_init(ssl);
    mbedtls_ssl_config_init(config);
    mbedtls_x509_crt_init(ca);
    mbedtls_ctr_drbg_init(drbg);
    mbedtls_entropy_init(entropy);

    const auto fail = [&](TlsErrorKind kind, std::string detail) {
        mbedtls_ssl_free(ssl);
        mbedtls_ssl_config_free(config);
        mbedtls_x509_crt_free(ca);
        mbedtls_ctr_drbg_free(drbg);
        mbedtls_entropy_free(entropy);
        delete ssl;
        delete config;
        delete ca;
        delete drbg;
        delete entropy;
        return std::unexpected(TlsError{kind, std::move(detail)});
    };

    int rc = mbedtls_ctr_drbg_seed(drbg, mbedtls_entropy_func, entropy, nullptr, 0);
    if (rc != 0) {
        return fail(TlsErrorKind::Failed, "drbg seed: " + MbedErrorText(rc));
    }
    rc = mbedtls_x509_crt_parse(ca, reinterpret_cast<const unsigned char*>(ca_pem.data()),
                                ca_pem.size() + 1);
    if (rc != 0) {
        return fail(TlsErrorKind::Failed, "ca parse: " + MbedErrorText(rc));
    }
    rc = mbedtls_ssl_config_defaults(config, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        return fail(TlsErrorKind::Failed, "config defaults: " + MbedErrorText(rc));
    }
    mbedtls_ssl_conf_authmode(config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(config, ca, nullptr);
    mbedtls_ssl_conf_rng(config, mbedtls_ctr_drbg_random, drbg);
    mbedtls_ssl_conf_read_timeout(config, static_cast<std::uint32_t>(handshake_timeout_ms));

    rc = mbedtls_ssl_setup(ssl, config);
    if (rc != 0) {
        return fail(TlsErrorKind::Failed, "ssl setup: " + MbedErrorText(rc));
    }
    rc = mbedtls_ssl_set_hostname(ssl, host.c_str());
    if (rc != 0) {
        return fail(TlsErrorKind::Failed, "set hostname: " + MbedErrorText(rc));
    }
    mbedtls_ssl_set_bio(ssl, sock, MbedSend, MbedRecv, MbedRecvTimeout);

    while (true) {
        rc = mbedtls_ssl_handshake(ssl);
        if (rc == 0) {
            break;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            const std::uint32_t flags = mbedtls_ssl_get_verify_result(ssl);
            return fail(TlsErrorKind::CertVerifyFailed,
                        "cert verify failed: flags=0x" + std::to_string(flags));
        }
        return fail(TlsErrorKind::HandshakeFailed, "handshake: " + MbedErrorText(rc));
    }
    // 握手成功后仍须核 verify flags(链不完整等在 REQUIRED 模式下应由
    // handshake 返回,这里再核一道,双保险)。
    const std::uint32_t flags = mbedtls_ssl_get_verify_result(ssl);
    if (flags != 0) {
        return fail(TlsErrorKind::CertVerifyFailed, "post-handshake verify flags=0x" +
                                                        std::to_string(flags));
    }

    return TlsClientStream(ssl, config, ca, drbg, entropy, sock);
}

std::expected<std::size_t, SocketError> TlsClientStream::ReadSome(char* buf, std::size_t len,
                                                                  int timeout_ms) const {
    if (ssl_ == nullptr) {
        return std::unexpected(SocketError{SocketErrorKind::Closed, "tls not open"});
    }
    mbedtls_ssl_conf_read_timeout(config_, static_cast<std::uint32_t>(timeout_ms));
    while (true) {
        const int rc = mbedtls_ssl_read(ssl_, reinterpret_cast<unsigned char*>(buf), len);
        if (rc >= 0) {
            return static_cast<std::size_t>(rc);
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return std::unexpected(SocketError{SocketErrorKind::Closed, "tls close notify"});
        }
        return std::unexpected(SocketError{ToSocketKind(rc), "tls read: " + MbedErrorText(rc)});
    }
}

std::expected<void, SocketError> TlsClientStream::WriteAll(std::string_view bytes,
                                                           int timeout_ms) const {
    if (ssl_ == nullptr) {
        return std::unexpected(SocketError{SocketErrorKind::Closed, "tls not open"});
    }
    mbedtls_ssl_conf_read_timeout(config_, static_cast<std::uint32_t>(timeout_ms));
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int rc = mbedtls_ssl_write(
            ssl_, reinterpret_cast<const unsigned char*>(bytes.data() + sent),
            bytes.size() - sent);
        if (rc > 0) {
            sent += static_cast<std::size_t>(rc);
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        return std::unexpected(
            SocketError{ToSocketKind(rc), "tls write: " + MbedErrorText(rc)});
    }
    return {};
}

void TlsClientStream::CloseNotify() {
    if (ssl_ == nullptr) {
        return;
    }
    (void)mbedtls_ssl_close_notify(ssl_);
}

}  // namespace lubancode::channel::qq
