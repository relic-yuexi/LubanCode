// MockWsServer(QQ 机器人接入单 Q1 的测试夹具):本机回环 WebSocket 服务端。
//
// 给自实现 WS 客户端(transport 层)与渠道网关测试当对端——真 socket、真升级握手
// (算 Sec-WebSocket-Accept)、真帧收发;另留 SendRaw 注原始字节,半帧/
// 粘帧/坏帧全可注入。TLS 路用 mbedTLS 服务端 + 测试内生成的自签证书
// (MockTlsServer),与客户端共享同一份 CA PEM——握手是 mbedTLS 真握手,
// 不是 stub。
//
// 一次一连接:AcceptNext 收一只,服务完再收下一只(QQ 网关场景即单连接)。
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace lubancode::test_support {

// ---------------------------------------------------------------------------
// 明文 TCP 变体
// ---------------------------------------------------------------------------

class MockWsServer {
public:
    MockWsServer() = default;
    ~MockWsServer();
    MockWsServer(const MockWsServer&) = delete;
    MockWsServer& operator=(const MockWsServer&) = delete;

    // 监听 127.0.0.1:0(系统分派端口),返回实际端口。
    std::expected<int, std::string> Start();

    class Connection {
    public:
        Connection() = default;
        ~Connection();
        Connection(Connection&& other) noexcept;
        Connection& operator=(Connection&& other) noexcept;

        bool valid() const { return fd_ >= 0; }

        // 等升级请求(到 \r\n\r\n),回 101(校验 Sec-WebSocket-Key 的
        // Accept)。返回升级请求全文(断言头用)。失败给人话。
        std::expected<std::string, std::string> AcceptUpgrade(int timeout_ms);

        // 服务端方向发文本帧。
        std::expected<void, std::string> SendText(std::string_view payload);
        // 服务端方向发二进制帧(飞书 F1 的 pbbp2 帧走 BinaryMessage)。
        std::expected<void, std::string> SendBinary(std::string_view payload);
        // 注原始字节(半帧/粘帧/坏帧注入口)。
        std::expected<void, std::string> SendRaw(std::string_view bytes);
        // 读客户端发来的下一条数据帧(解客户端 mask;text/binary 都收,
        // 载荷原样返回)。
        std::expected<std::string, std::string> ReadText(int timeout_ms);
        // 收原始字节(验控制帧回执用,如客户端回的 Pong)。
        std::expected<std::string, std::string> ReadRaw(int timeout_ms, std::size_t max_bytes);
        // 主动断 TCP(模拟断线)。
        void Drop();

    private:
        friend class MockWsServer;
        explicit Connection(long long fd) : fd_(fd) {}
        long long fd_ = -1;
        std::string pending_;
    };

    // 收下一只连接(阻塞 timeout_ms)。
    std::expected<Connection, std::string> AcceptNext(int timeout_ms);

private:
    long long listen_fd_ = -1;
};

// ---------------------------------------------------------------------------
// TLS 变体:mbedTLS 服务端 + 进程内自签证书
// ---------------------------------------------------------------------------

// 自签材料(CA 证书 PEM + 私钥),进程内生成一次。失败给人话。
struct MockTlsCert {
    std::string ca_pem;       // 自签证书(同时当 CA 与服务端叶子证书)
    std::string private_pem;  // 对应私钥 PEM
};

// 证书生成参数(证书部分解析误判修复单 §六:错主机名/过期案要可控
// CN 与有效期)。全默认 = 原有行为,老调用零变化。
struct MockTlsCertOptions {
    const char* subject_cn = "127.0.0.1";    // 证书 CN(主机名验证的靶子)
    const char* not_before = "20240101000000";  // 生效(可设过去/未来)
    const char* not_after = "20440101000000";   // 过期(可设过去造过期证)
};
std::expected<MockTlsCert, std::string> GenerateSelfSignedCert(
    const MockTlsCertOptions& options = {});

// TLS 服务端:监听 + accept + mbedTLS 服务端握手 + 帧收发(与 MockWsServer
// 的 Connection 同一形状的收发面,后续帧层复用明文的实现——TLS 只换字节层)。
class MockTlsServer {
public:
    MockTlsServer() = default;
    ~MockTlsServer();
    MockTlsServer(const MockTlsServer&) = delete;
    MockTlsServer& operator=(const MockTlsServer&) = delete;

    // cert 来自 GenerateSelfSignedCert;监听 127.0.0.1:0。
    std::expected<int, std::string> Start(const MockTlsCert& cert);

    class Connection {
    public:
        Connection() = default;
        ~Connection();
        Connection(Connection&& other) noexcept;
        Connection& operator=(Connection&& other) noexcept;

        bool valid() const { return ssl_ != nullptr; }
        std::expected<std::string, std::string> AcceptUpgrade(int timeout_ms);
        std::expected<void, std::string> SendText(std::string_view payload);
        std::expected<void, std::string> SendBinary(std::string_view payload);
        std::expected<void, std::string> SendRaw(std::string_view bytes);
        std::expected<std::string, std::string> ReadText(int timeout_ms);
        std::expected<std::string, std::string> ReadRaw(int timeout_ms, std::size_t max_bytes);
        void Drop();

    private:
        friend class MockTlsServer;
        void* ssl_ = nullptr;        // mbedtls_ssl_context*(void* 免头拖累)
        void* config_ = nullptr;     // mbedtls_ssl_config*
        void* cert_ = nullptr;       // mbedtls_x509_crt*
        void* key_ = nullptr;        // mbedtls_pk_context*
        long long fd_ = -1;
        std::string pending_;
    };

    std::expected<Connection, std::string> AcceptNext(int timeout_ms);

private:
    long long listen_fd_ = -1;
    void* config_ = nullptr;  // 服务端 ssl_config(accept 出的连接共享)
    void* cert_ = nullptr;
    void* key_ = nullptr;
};

}  // namespace lubancode::test_support
