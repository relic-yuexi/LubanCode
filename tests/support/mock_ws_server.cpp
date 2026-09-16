#include "mock_ws_server.hpp"

#include <cstring>
#include <thread>

#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/net_sockets.h>  // MBEDTLS_ERR_NET_* 错误码
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha1.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>  // x509write_crt 系列也在这一头里

#include "channel/qq/ws_frame.hpp"
#include "platform/base64.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace lubancode::test_support {

namespace {

using NativeSocket =
#ifdef _WIN32
    SOCKET;
#else
    int;
#endif

constexpr NativeSocket kBadSocket =
#ifdef _WIN32
    INVALID_SOCKET;
#else
    -1;
#endif

struct WinsockGuard {
#ifdef _WIN32
    bool ok = false;
    WinsockGuard() {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockGuard() {
        if (ok) {
            WSACleanup();
        }
    }
#else
    static constexpr bool ok = true;
#endif
};

bool EnsureWinsock() {
    static WinsockGuard guard;
    return guard.ok;
}

void CloseNative(NativeSocket fd) {
    if (fd == kBadSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
}

bool WaitReadable(NativeSocket fd, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
    return ::select(static_cast<int>(fd) + 1, &set, nullptr, nullptr, &tv) > 0;
#else
    return ::select(fd + 1, &set, nullptr, nullptr, &tv) > 0;
#endif
}

bool ReadChunk(NativeSocket fd, std::string& into, int timeout_ms) {
    if (!WaitReadable(fd, timeout_ms)) {
        return false;
    }
    char buffer[4096];
    const int got = static_cast<int>(::recv(fd, buffer, sizeof(buffer), 0));
    if (got <= 0) {
        return false;
    }
    into.append(buffer, static_cast<std::size_t>(got));
    return true;
}

bool WriteAllNative(NativeSocket fd, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;  // Linux:不杀进程
#else
        const int flags = 0;  // macOS 靠 accept 后的 SO_NOSIGPIPE;Windows 无此问题
#endif
        const int wrote =
            static_cast<int>(::send(fd, data + sent, size - sent, flags));
        if (wrote <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(wrote);
    }
    return true;
}

std::string ToLowerCopy(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::string HeaderValueOf(const std::string& headers, const std::string& name) {
    const std::string prefix = ToLowerCopy(name) + ":";
    std::size_t pos = 0;
    while (pos < headers.size()) {
        const std::size_t eol = headers.find("\r\n", pos);
        const std::size_t line_end = eol == std::string::npos ? headers.size() : eol;
        const std::string line = ToLowerCopy(headers.substr(pos, line_end - pos));
        if (line.rfind(prefix, 0) == 0) {
            std::size_t start = pos + prefix.size();
            while (start < headers.size() && headers[start] == ' ') {
                ++start;
            }
            std::size_t end = line_end;
            while (end > start && headers[end - 1] == ' ') {
                --end;
            }
            return headers.substr(start, end - start);
        }
        if (eol == std::string::npos) {
            break;
        }
        pos = eol + 2;
    }
    return std::string();
}

std::string ComputeAcceptHeader(const std::string& key) {
    const std::string concatenated = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[20];
    mbedtls_sha1(reinterpret_cast<const unsigned char*>(concatenated.data()),
                 concatenated.size(), digest);
    return platform::Base64Encode(
        std::string_view(reinterpret_cast<const char*>(digest), sizeof(digest)));
}

// 读到双 CRLF,回升级请求全文。
std::expected<std::string, std::string> ReadUpgradeRequest(NativeSocket fd) {
    std::string received;
    while (received.find("\r\n\r\n") == std::string::npos) {
        if (received.size() > 16 * 1024) {
            return std::unexpected("upgrade request over cap");
        }
        if (!ReadChunk(fd, received, 5000)) {
            return std::unexpected("timeout waiting upgrade request");
        }
    }
    return received.substr(0, received.find("\r\n\r\n"));
}

std::expected<void, std::string> SendUpgradeResponse(NativeSocket fd,
                                                     const std::string& request) {
    const std::string key = HeaderValueOf(request, "Sec-WebSocket-Key");
    if (key.empty()) {
        return std::unexpected("request missing Sec-WebSocket-Key");
    }
    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        ComputeAcceptHeader(key) + "\r\n\r\n";
    if (!WriteAllNative(fd, response.data(), response.size())) {
        return std::unexpected("write upgrade response failed");
    }
    return {};
}

// 解客户端(带 mask)的单帧文本。QQ 客户端帧必 mask;这里只解 text 帧。
std::expected<std::string, std::string> DecodeClientTextFrame(const std::string& buffer,
                                                              std::size_t& consumed) {
    if (buffer.size() < 2) {
        return std::unexpected("need more");
    }
    const std::uint8_t first = static_cast<std::uint8_t>(buffer[0]);
    const std::uint8_t second = static_cast<std::uint8_t>(buffer[1]);
    const std::uint8_t opcode = first & 0x0F;
    const bool masked = (second & 0x80) != 0;
    std::uint64_t len = second & 0x7F;
    std::size_t header = 2;
    if (len == 126) {
        if (buffer.size() < 4) {
            return std::unexpected("need more");
        }
        len = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buffer[2])) << 8) |
              static_cast<std::uint8_t>(buffer[3]);
        header = 4;
    } else if (len == 127) {
        if (buffer.size() < 10) {
            return std::unexpected("need more");
        }
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | static_cast<std::uint8_t>(buffer[2 + i]);
        }
        header = 10;
    }
    if (opcode != 0x1) {
        return std::unexpected("not a text frame");
    }
    if (!masked) {
        return std::unexpected("client frame not masked");
    }
    if (buffer.size() < header + 4 + len) {
        return std::unexpected("need more");
    }
    const std::uint8_t mask[4] = {
        static_cast<std::uint8_t>(buffer[header]),
        static_cast<std::uint8_t>(buffer[header + 1]),
        static_cast<std::uint8_t>(buffer[header + 2]),
        static_cast<std::uint8_t>(buffer[header + 3]),
    };
    std::string out;
    out.reserve(static_cast<std::size_t>(len));
    for (std::uint64_t i = 0; i < len; ++i) {
        const char masked_byte = buffer[header + 4 + i];
        out.push_back(static_cast<char>(
            static_cast<std::uint8_t>(masked_byte) ^ mask[i % 4]));
    }
    consumed = header + 4 + static_cast<std::size_t>(len);
    return out;
}

std::expected<void, std::string> SendServerFrame(NativeSocket fd,
                                                 lubancode::channel::qq::WsOpcode opcode,
                                                 std::string_view payload) {
    const auto frame = lubancode::channel::qq::EncodeServerFrame(opcode, payload);
    if (!WriteAllNative(fd, reinterpret_cast<const char*>(frame.data()), frame.size())) {
        return std::unexpected("write frame failed");
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// MockWsServer(明文)
// ---------------------------------------------------------------------------

MockWsServer::~MockWsServer() { CloseNative(listen_fd_); }

std::expected<int, std::string> MockWsServer::Start() {
    if (!EnsureWinsock()) {
        return std::unexpected("winsock init failed");
    }
    listen_fd_ = static_cast<long long>(::socket(AF_INET, SOCK_STREAM, 0));
    const NativeSocket fd = static_cast<NativeSocket>(listen_fd_);
    if (fd == kBadSocket) {
        return std::unexpected("socket() failed");
    }
    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        CloseNative(fd);
        listen_fd_ = -1;
        return std::unexpected("bind failed");
    }
    if (::listen(fd, 4) != 0) {
        CloseNative(fd);
        listen_fd_ = -1;
        return std::unexpected("listen failed");
    }
    sockaddr_in actual{};
#ifdef _WIN32
    int len = sizeof(actual);
#else
    socklen_t len = sizeof(actual);
#endif
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len);
    return ntohs(actual.sin_port);
}

std::expected<MockWsServer::Connection, std::string> MockWsServer::AcceptNext(
    int timeout_ms) {
    if (!WaitReadable(static_cast<NativeSocket>(listen_fd_), timeout_ms)) {
        return std::unexpected("accept timeout");
    }
    const NativeSocket fd =
        ::accept(static_cast<NativeSocket>(listen_fd_), nullptr, nullptr);
    if (fd == kBadSocket) {
        return std::unexpected("accept failed");
    }
#ifdef __APPLE__
    // 写断开的客户端不发 SIGPIPE(册级进程命门,同 qq_socket 的口径)。
    {
        int nosigpipe = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
    }
#endif
    return Connection(static_cast<long long>(fd));
}

MockWsServer::Connection::~Connection() {
    if (fd_ >= 0) {
        CloseNative(static_cast<NativeSocket>(fd_));
    }
}

MockWsServer::Connection::Connection(Connection&& other) noexcept
    : fd_(other.fd_), pending_(std::move(other.pending_)) {
    other.fd_ = -1;
}

MockWsServer::Connection& MockWsServer::Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            CloseNative(static_cast<NativeSocket>(fd_));
        }
        fd_ = other.fd_;
        pending_ = std::move(other.pending_);
        other.fd_ = -1;
    }
    return *this;
}

std::expected<std::string, std::string> MockWsServer::Connection::AcceptUpgrade(
    int timeout_ms) {
    (void)timeout_ms;
    const auto request = ReadUpgradeRequest(static_cast<NativeSocket>(fd_));
    if (!request.has_value()) {
        return std::unexpected(request.error());
    }
    const auto sent = SendUpgradeResponse(static_cast<NativeSocket>(fd_), *request);
    if (!sent.has_value()) {
        return std::unexpected(sent.error());
    }
    return *request;
}

std::expected<void, std::string> MockWsServer::Connection::SendText(
    std::string_view payload) {
    return SendServerFrame(static_cast<NativeSocket>(fd_),
                           lubancode::channel::qq::WsOpcode::Text, payload);
}

std::expected<void, std::string> MockWsServer::Connection::SendRaw(std::string_view bytes) {
    if (!WriteAllNative(static_cast<NativeSocket>(fd_), bytes.data(), bytes.size())) {
        return std::unexpected("raw write failed");
    }
    return {};
}

std::expected<std::string, std::string> MockWsServer::Connection::ReadText(int timeout_ms) {
    while (true) {
        std::size_t consumed = 0;
        const auto decoded = DecodeClientTextFrame(pending_, consumed);
        if (decoded.has_value()) {
            pending_.erase(0, consumed);
            return *decoded;
        }
        if (decoded.error() != "need more") {
            return std::unexpected(decoded.error());
        }
        if (!ReadChunk(static_cast<NativeSocket>(fd_), pending_, timeout_ms)) {
            return std::unexpected("read timeout");
        }
    }
}

std::expected<std::string, std::string> MockWsServer::Connection::ReadRaw(int timeout_ms,
                                                                          std::size_t max_bytes) {
    while (pending_.size() < max_bytes) {
        if (!ReadChunk(static_cast<NativeSocket>(fd_), pending_, timeout_ms)) {
            return std::unexpected("read timeout");
        }
    }
    std::string out = pending_.substr(0, max_bytes);
    pending_.erase(0, max_bytes);
    return out;
}

void MockWsServer::Connection::Drop() {
    if (fd_ >= 0) {
        ::shutdown(static_cast<NativeSocket>(fd_), 2);
        CloseNative(static_cast<NativeSocket>(fd_));
        fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// 自签证书
// ---------------------------------------------------------------------------

std::expected<MockTlsCert, std::string> GenerateSelfSignedCert(
    const MockTlsCertOptions& options) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    const auto teardown = [&]() {
        mbedtls_x509write_crt_free(&crt);
        mbedtls_pk_free(&key);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    };
    char errbuf[256];
    const auto fail = [&](const std::string& where, int rc) {
        mbedtls_strerror(rc, errbuf, sizeof(errbuf));
        const std::string detail = where + ": " + errbuf;
        teardown();
        return std::unexpected(detail);
    };

    int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0);
    if (rc != 0) {
        return fail("drbg seed", rc);
    }
    // 3.6 无统一 key 生成口:pk_setup + rsa_gen_key 两步(pk_rsa 按值收参)。
    rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (rc == 0) {
        rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random, &drbg, 2048,
                                 65537);
    }
    if (rc != 0) {
        return fail("generate key", rc);
    }
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    // CN 默认回环地址:客户端按 host(127.0.0.1)做名字验证,证书无 SAN
    // 时 mbedTLS 回落 CN 匹配——回环测试最省事的锚。参数化后可造错主机
    // 名靶子(§六)。
    const std::string subject = std::string("CN=") + options.subject_cn;
    rc = mbedtls_x509write_crt_set_subject_name(&crt, subject.c_str());
    if (rc != 0) {
        return fail("subject name", rc);
    }
    rc = mbedtls_x509write_crt_set_issuer_name(&crt, subject.c_str());
    if (rc != 0) {
        return fail("issuer name", rc);
    }
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    mbedtls_mpi serial_mpi;
    mbedtls_mpi_init(&serial_mpi);
    rc = mbedtls_mpi_lset(&serial_mpi, 1);
    if (rc == 0) {
        rc = mbedtls_x509write_crt_set_serial(&crt, &serial_mpi);
    }
    mbedtls_mpi_free(&serial_mpi);
    if (rc != 0) {
        return fail("serial", rc);
    }
    rc = mbedtls_x509write_crt_set_validity(&crt, options.not_before, options.not_after);
    if (rc != 0) {
        return fail("validity", rc);
    }
    rc = mbedtls_x509write_crt_set_basic_constraints(&crt, /*is_ca=*/1, /*pathlen=*/0);
    if (rc != 0) {
        return fail("basic constraints", rc);
    }
    unsigned char cert_pem[4096];
    rc = mbedtls_x509write_crt_pem(&crt, cert_pem, sizeof(cert_pem), mbedtls_ctr_drbg_random,
                                  &drbg);
    if (rc != 0) {
        return fail("write cert pem", rc);
    }
    unsigned char key_pem[4096];
    rc = mbedtls_pk_write_key_pem(&key, key_pem, sizeof(key_pem));
    if (rc != 0) {
        return fail("write key pem", rc);
    }

    MockTlsCert out;
    out.ca_pem = reinterpret_cast<const char*>(cert_pem);
    out.private_pem = reinterpret_cast<const char*>(key_pem);
    teardown();
    return out;
}

// ---------------------------------------------------------------------------
// MockTlsServer
// ---------------------------------------------------------------------------

MockTlsServer::~MockTlsServer() {
    if (config_ != nullptr) {
        mbedtls_ssl_config_free(static_cast<mbedtls_ssl_config*>(config_));
        delete static_cast<mbedtls_ssl_config*>(config_);
    }
    if (cert_ != nullptr) {
        mbedtls_x509_crt_free(static_cast<mbedtls_x509_crt*>(cert_));
        delete static_cast<mbedtls_x509_crt*>(cert_);
    }
    if (key_ != nullptr) {
        mbedtls_pk_free(static_cast<mbedtls_pk_context*>(key_));
        delete static_cast<mbedtls_pk_context*>(key_);
    }
    CloseNative(static_cast<NativeSocket>(listen_fd_));
}

std::expected<int, std::string> MockTlsServer::Start(const MockTlsCert& cert) {
    if (!EnsureWinsock()) {
        return std::unexpected("winsock init failed");
    }
    auto* config = new mbedtls_ssl_config();
    auto* server_cert = new mbedtls_x509_crt();
    auto* server_key = new mbedtls_pk_context();
    mbedtls_ssl_config_init(config);
    mbedtls_x509_crt_init(server_cert);
    mbedtls_pk_init(server_key);

    int rc = mbedtls_x509_crt_parse(server_cert,
                                    reinterpret_cast<const unsigned char*>(cert.ca_pem.c_str()),
                                    cert.ca_pem.size() + 1);
    if (rc != 0) {
        return std::unexpected("cert parse failed");
    }
    rc = mbedtls_pk_parse_key(server_key,
                              reinterpret_cast<const unsigned char*>(cert.private_pem.c_str()),
                              cert.private_pem.size() + 1, nullptr, 0, nullptr, nullptr);
    if (rc != 0) {
        return std::unexpected("key parse failed");
    }
    rc = mbedtls_ssl_config_defaults(config, MBEDTLS_SSL_IS_SERVER,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        return std::unexpected("config defaults failed");
    }
    // 测试夹具:不要求客户端证书(客户端仍被服务端证书锚定)。
    mbedtls_ssl_conf_authmode(config, MBEDTLS_SSL_VERIFY_NONE);
    static mbedtls_ctr_drbg_context s_drbg;
    static mbedtls_entropy_context s_entropy;
    static bool s_rng_ready = []() {
        mbedtls_entropy_init(&s_entropy);
        mbedtls_ctr_drbg_init(&s_drbg);
        return mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy, nullptr,
                                     0) == 0;
    }();
    if (!s_rng_ready) {
        return std::unexpected("rng init failed");
    }
    mbedtls_ssl_conf_rng(config, mbedtls_ctr_drbg_random, &s_drbg);
    rc = mbedtls_ssl_conf_own_cert(config, server_cert, server_key);
    if (rc != 0) {
        return std::unexpected("own cert failed");
    }

    listen_fd_ = static_cast<long long>(::socket(AF_INET, SOCK_STREAM, 0));
    const NativeSocket fd = static_cast<NativeSocket>(listen_fd_);
    if (fd == kBadSocket) {
        return std::unexpected("socket() failed");
    }
    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 4) != 0) {
        CloseNative(fd);
        listen_fd_ = -1;
        return std::unexpected("bind/listen failed");
    }
    sockaddr_in actual{};
#ifdef _WIN32
    int len = sizeof(actual);
#else
    socklen_t len = sizeof(actual);
#endif
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len);
    config_ = config;
    cert_ = server_cert;
    key_ = server_key;
    return ntohs(actual.sin_port);
}

namespace {

// TLS 连接的字节层(供 Connection 的收发函数复用明文的帧逻辑)。
bool TlsReadChunk(mbedtls_ssl_context* ssl, std::string& into, int timeout_ms) {
    (void)timeout_ms;  // mbedTLS 读带内部超时;夹具按阻塞用
    unsigned char buffer[4096];
    const int got = mbedtls_ssl_read(ssl, buffer, sizeof(buffer));
    if (got <= 0) {
        return false;
    }
    into.append(reinterpret_cast<const char*>(buffer), static_cast<std::size_t>(got));
    return true;
}

bool TlsWriteAll(mbedtls_ssl_context* ssl, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const int wrote = mbedtls_ssl_write(
            ssl, reinterpret_cast<const unsigned char*>(data + sent), size - sent);
        if (wrote <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(wrote);
    }
    return true;
}

}  // namespace

std::expected<MockTlsServer::Connection, std::string> MockTlsServer::AcceptNext(
    int timeout_ms) {
    if (!WaitReadable(static_cast<NativeSocket>(listen_fd_), timeout_ms)) {
        return std::unexpected("accept timeout");
    }
    const NativeSocket fd =
        ::accept(static_cast<NativeSocket>(listen_fd_), nullptr, nullptr);
    if (fd == kBadSocket) {
        return std::unexpected("accept failed");
    }
#ifdef __APPLE__
    // 写断开的客户端不发 SIGPIPE(册级进程命门,同 qq_socket 的口径)。
    {
        int nosigpipe = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
    }
#endif
    auto* ssl = new mbedtls_ssl_context();
    mbedtls_ssl_init(ssl);
    if (mbedtls_ssl_setup(ssl, static_cast<mbedtls_ssl_config*>(config_)) != 0) {
        mbedtls_ssl_free(ssl);
        delete ssl;
        CloseNative(fd);
        return std::unexpected("ssl setup failed");
    }
    mbedtls_ssl_set_bio(ssl, reinterpret_cast<void*>(
                                 static_cast<long long>(fd)),
                        [](void* ctx, const unsigned char* buf, std::size_t len) {
                            const NativeSocket raw =
                                static_cast<NativeSocket>(reinterpret_cast<long long>(ctx));
                            // Winsock 收 char*(MSVC 符号性严格);Linux 写断开的
                            // 对端须 MSG_NOSIGNAL(macOS 由 accept 后的
                            // SO_NOSIGPIPE 兜底,与 WriteAllNative 同一套账)。
#ifdef MSG_NOSIGNAL
                            const int send_flags = MSG_NOSIGNAL;
#else
                            const int send_flags = 0;
#endif
                            const int wrote = static_cast<int>(::send(
                                raw, reinterpret_cast<const char*>(buf), len, send_flags));
                            return wrote <= 0 ? MBEDTLS_ERR_NET_SEND_FAILED : wrote;
                        },
                        [](void* ctx, unsigned char* buf, std::size_t len) {
                            const NativeSocket raw =
                                static_cast<NativeSocket>(reinterpret_cast<long long>(ctx));
                            const int got = static_cast<int>(
                                ::recv(raw, reinterpret_cast<char*>(buf), len, 0));
                            if (got == 0) {
                                return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
                            }
                            return got < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : got;
                        },
                        nullptr);
    while (true) {
        const int rc = mbedtls_ssl_handshake(ssl);
        if (rc == 0) {
            break;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        mbedtls_ssl_free(ssl);
        delete ssl;
        CloseNative(fd);
        return std::unexpected("tls handshake failed");
    }
    Connection connection;
    connection.ssl_ = ssl;
    connection.fd_ = static_cast<long long>(fd);
    return connection;
}

MockTlsServer::Connection::~Connection() {
    if (ssl_ != nullptr) {
        mbedtls_ssl_free(static_cast<mbedtls_ssl_context*>(ssl_));
        delete static_cast<mbedtls_ssl_context*>(ssl_);
    }
    if (fd_ >= 0) {
        CloseNative(static_cast<NativeSocket>(fd_));
    }
}

MockTlsServer::Connection::Connection(Connection&& other) noexcept
    : ssl_(other.ssl_),
      fd_(other.fd_),
      pending_(std::move(other.pending_)) {
    other.ssl_ = nullptr;
    other.fd_ = -1;
}

MockTlsServer::Connection& MockTlsServer::Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        if (ssl_ != nullptr) {
            mbedtls_ssl_free(static_cast<mbedtls_ssl_context*>(ssl_));
            delete static_cast<mbedtls_ssl_context*>(ssl_);
        }
        if (fd_ >= 0) {
            CloseNative(static_cast<NativeSocket>(fd_));
        }
        ssl_ = other.ssl_;
        fd_ = other.fd_;
        pending_ = std::move(other.pending_);
        other.ssl_ = nullptr;
        other.fd_ = -1;
    }
    return *this;
}

std::expected<std::string, std::string> MockTlsServer::Connection::AcceptUpgrade(
    int timeout_ms) {
    auto* ssl = static_cast<mbedtls_ssl_context*>(ssl_);
    (void)timeout_ms;
    std::string received;
    while (received.find("\r\n\r\n") == std::string::npos) {
        if (received.size() > 16 * 1024) {
            return std::unexpected("upgrade request over cap");
        }
        if (!TlsReadChunk(ssl, received, 5000)) {
            return std::unexpected("timeout waiting upgrade request");
        }
    }
    const std::string request = received.substr(0, received.find("\r\n\r\n"));
    const std::string key = HeaderValueOf(request, "Sec-WebSocket-Key");
    if (key.empty()) {
        return std::unexpected("request missing Sec-WebSocket-Key");
    }
    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        ComputeAcceptHeader(key) + "\r\n\r\n";
    if (!TlsWriteAll(ssl, response.data(), response.size())) {
        return std::unexpected("write upgrade response failed");
    }
    return request;
}

std::expected<void, std::string> MockTlsServer::Connection::SendText(
    std::string_view payload) {
    const auto frame = lubancode::channel::qq::EncodeServerFrame(
        lubancode::channel::qq::WsOpcode::Text, payload);
    if (!TlsWriteAll(static_cast<mbedtls_ssl_context*>(ssl_),
                     reinterpret_cast<const char*>(frame.data()), frame.size())) {
        return std::unexpected("tls write frame failed");
    }
    return {};
}

std::expected<void, std::string> MockTlsServer::Connection::SendRaw(std::string_view bytes) {
    if (!TlsWriteAll(static_cast<mbedtls_ssl_context*>(ssl_), bytes.data(), bytes.size())) {
        return std::unexpected("tls raw write failed");
    }
    return {};
}

std::expected<std::string, std::string> MockTlsServer::Connection::ReadText(int timeout_ms) {
    (void)timeout_ms;
    auto* ssl = static_cast<mbedtls_ssl_context*>(ssl_);
    while (true) {
        std::size_t consumed = 0;
        const auto decoded = DecodeClientTextFrame(pending_, consumed);
        if (decoded.has_value()) {
            pending_.erase(0, consumed);
            return *decoded;
        }
        if (decoded.error() != "need more") {
            return std::unexpected(decoded.error());
        }
        if (!TlsReadChunk(ssl, pending_, 5000)) {
            return std::unexpected("read timeout");
        }
    }
}

void MockTlsServer::Connection::Drop() {
    if (ssl_ != nullptr) {
        (void)mbedtls_ssl_close_notify(static_cast<mbedtls_ssl_context*>(ssl_));
    }
    if (fd_ >= 0) {
        ::shutdown(static_cast<NativeSocket>(fd_), 2);
        CloseNative(static_cast<NativeSocket>(fd_));
        fd_ = -1;
    }
}

}  // namespace lubancode::test_support
