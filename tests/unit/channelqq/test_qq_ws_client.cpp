// WS 客户端真 socket 册(QQ 机器人接入单 Q1):连本机 MockWsServer/MockTlsServer
// 走真 TCP + 真升级握手 + 真帧(明文与 mbedTLS 自签两路)。Accept 校验、
// 半帧/分片拼装、ping 自动回 pong、close 传播、坏响应拒绝逐项钉。
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "channel/qq/qq_ws_client.hpp"
#include "mock_ws_server.hpp"

namespace lubancode::channel::qq {
namespace {

using test_support::MockTlsServer;
using test_support::MockWsServer;

}  // namespace

TEST_CASE("qq_ws_client: 明文握手+收发文本+Ping 自动回 Pong") {
    MockWsServer server;
    const auto port = server.Start();
    REQUIRE(port.has_value());

    bool accept_done = false;
    MockWsServer::Connection server_side;
    std::string upgrade_request;
    std::thread acceptor([&]() {
        auto connection = server.AcceptNext(5'000);
        if (!connection.has_value()) {
            return;
        }
        const auto upgrade = connection->AcceptUpgrade(5'000);
        if (!upgrade.has_value()) {
            return;
        }
        upgrade_request = *upgrade;
        server_side = std::move(*connection);
        accept_done = true;
    });

    WsConnectOptions options;
    options.url = "ws://127.0.0.1:" + std::to_string(*port) + "/path?q=1";
    auto client = WsClient::Connect(options);
    REQUIRE(client.has_value());
    REQUIRE(client->SendText(R"({"op":2})").has_value());

    acceptor.join();
    REQUIRE(accept_done);
    // 升级请求头钉死(RFC 6455 §4.2.1)。
    CHECK(upgrade_request.find("GET /path?q=1 HTTP/1.1") == 0);
    CHECK(upgrade_request.find("Upgrade: websocket") != std::string::npos);
    CHECK(upgrade_request.find("Sec-WebSocket-Version: 13") != std::string::npos);
    CHECK(upgrade_request.find("Sec-WebSocket-Key: ") != std::string::npos);

    // 服务端发文本,客户端收。
    REQUIRE(server_side.SendText(R"({"op":10})").has_value());
    const auto message = client->ReadMessage(5'000);
    REQUIRE(message.has_value());
    CHECK(*message == R"({"op":10})");

    // 服务端收客户端文本(mask 已被 mock 解开)。
    const auto from_client = server_side.ReadText(5'000);
    REQUIRE(from_client.has_value());
    CHECK(*from_client == R"({"op":2})");

    // Ping → 客户端自动回 Pong(读文本调用内消化),载荷回传。
    REQUIRE(server_side.SendRaw(std::string{"\x89\x03" "abc", 5}).has_value());
    REQUIRE(server_side.SendText(R"({"after":"ping"})").has_value());
    // 客户端下一读收到的是 Ping 之后的消息(不是 Ping 本身),且连接活着。
    const auto after_ping = client->ReadMessage(5'000);
    REQUIRE(after_ping.has_value());
    CHECK(*after_ping == R"({"after":"ping"})");
    // 客户端回的 Pong 帧从原始字节验:0x8A + len 3 + mask 4 + "abc" 掩码。
    const auto raw = server_side.ReadRaw(5'000, 9);
    REQUIRE(raw.has_value());
    CHECK(static_cast<std::uint8_t>((*raw)[0]) == 0x8A);
    CHECK(static_cast<std::uint8_t>((*raw)[1]) == 0x83);  // mask 位 + len 3

    (void)client->Close(1000, "done");
}

TEST_CASE("qq_ws_client: 半帧+分片——服务端切三段发,客户端拼回完整") {
    MockWsServer server;
    const auto port = server.Start();
    REQUIRE(port.has_value());

    MockWsServer::Connection server_side;
    std::thread acceptor([&]() {
        auto connection = server.AcceptNext(5'000);
        if (!connection.has_value()) {
            return;
        }
        if (!connection->AcceptUpgrade(5'000).has_value()) {
            return;
        }
        // 切三段:起始 fin=0 / continuation fin=0 / 尾 fin=1(payload 40 字节,
        // 尾段 40-16=24=0x18)。
        const std::string payload = R"({"op":0,"s":42,"t":"C2C_MESSAGE_CREATE"})";
        (void)connection->SendRaw(std::string{"\x01\x08", 2} + payload.substr(0, 8));
        (void)connection->SendRaw(std::string{"\x00\x08", 2} + payload.substr(8, 8));
        (void)connection->SendRaw(std::string{"\x80\x18", 2} + payload.substr(16));
        server_side = std::move(*connection);
    });

    WsConnectOptions options;
    options.url = "ws://127.0.0.1:" + std::to_string(*port) + "/";
    auto client = WsClient::Connect(options);
    REQUIRE(client.has_value());
    const auto message = client->ReadMessage(5'000);
    REQUIRE(message.has_value());
    CHECK(*message == R"({"op":0,"s":42,"t":"C2C_MESSAGE_CREATE"})");

    acceptor.join();
    (void)client->Close(1000, "done");
}

TEST_CASE("qq_ws_client: close 帧让 ReadMessage 以 Closed 分型返回") {
    MockWsServer server;
    const auto port = server.Start();
    REQUIRE(port.has_value());

    std::thread acceptor([&]() {
        auto connection = server.AcceptNext(5'000);
        if (!connection.has_value() || !connection->AcceptUpgrade(5'000).has_value()) {
            return;
        }
        // close 1000 "bye"。
        (void)connection->SendRaw(std::string{"\x88\x06\x03\xe8" "bye", 8});
        // 等客户端 close 回帧(尽力)。
        std::this_thread::sleep_for(std::chrono::milliseconds(1'500));
    });

    WsConnectOptions options;
    options.url = "ws://127.0.0.1:" + std::to_string(*port) + "/";
    auto client = WsClient::Connect(options);
    REQUIRE(client.has_value());
    const auto closed = client->ReadMessage(5'000);
    REQUIRE_FALSE(closed.has_value());
    CHECK(closed.error().kind == WsError::Kind::Closed);
    CHECK(closed.error().close_code == 1000);

    acceptor.join();
}

TEST_CASE("qq_ws_client: 服务端直发超帽长度立即报协议错") {
    MockWsServer server;
    const auto port = server.Start();
    REQUIRE(port.has_value());

    std::thread acceptor([&]() {
        auto connection = server.AcceptNext(5'000);
        if (!connection.has_value() || !connection->AcceptUpgrade(5'000).has_value()) {
            return;
        }
        // text 帧,64 位长度声明 9 MiB(超 8 MiB 帽),载荷不发。
        std::string frame(1, static_cast<char>(0x81));
        frame.push_back(static_cast<char>(127));
        const std::uint64_t huge = 9 * 1024 * 1024;
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((huge >> shift) & 0xFF));
        }
        (void)connection->SendRaw(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(1'500));
    });

    WsConnectOptions options;
    options.url = "ws://127.0.0.1:" + std::to_string(*port) + "/";
    auto client = WsClient::Connect(options);
    REQUIRE(client.has_value());
    const auto result = client->ReadMessage(3'000);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == WsError::Kind::Protocol);

    acceptor.join();
}

TEST_CASE("qq_ws_client: TLS 自签握手收发(wss)") {
    const auto cert = test_support::GenerateSelfSignedCert();
    REQUIRE(cert.has_value());
    test_support::MockTlsServer server;
    const auto port = server.Start(*cert);
    REQUIRE(port.has_value());

    std::thread acceptor([&]() {
        auto connection = server.AcceptNext(10'000);
        if (!connection.has_value() || !connection->AcceptUpgrade(10'000).has_value()) {
            return;
        }
        (void)connection->SendText(R"({"op":10,"d":{"heartbeat_interval_ms":1000}})");
        // 活够久:客户端要读完 Hello 再 Close,提前拆连接会把客户端的
        // Pong/close 回写撞成 -78。
        std::this_thread::sleep_for(std::chrono::milliseconds(2'000));
    });

    WsConnectOptions options;
    options.url = "wss://127.0.0.1:" + std::to_string(*port) + "/ws";
    options.ca_pem = cert->ca_pem;
    auto client = WsClient::Connect(options);
    if (!client.has_value()) {
        // 先收线程再报错——REQUIRE 直接抛会把 joinable 的 acceptor 析构成
        // SIGABRT;FAIL 带 detail 出来诊断。
        acceptor.join();
        FAIL(client.error().detail);
    }
    const auto message = client->ReadMessage(5'000);
    REQUIRE(message.has_value());
    CHECK(message->find(R"("heartbeat_interval_ms")") != std::string::npos);

    acceptor.join();
    (void)client->Close(1000, "done");
}

TEST_CASE("qq_ws_client: TLS 信任锚不匹配拒握手(CertVerifyFailed 分型)") {
    const auto cert = test_support::GenerateSelfSignedCert();
    REQUIRE(cert.has_value());
    // 另一把无关证书当"锚"——验证必须失败。
    const auto other = test_support::GenerateSelfSignedCert();
    REQUIRE(other.has_value());

    test_support::MockTlsServer server;
    const auto port = server.Start(*cert);
    REQUIRE(port.has_value());
    std::thread acceptor([&]() {
        (void)server.AcceptNext(5'000);
    });

    WsConnectOptions options;
    options.url = "wss://127.0.0.1:" + std::to_string(*port) + "/ws";
    options.ca_pem = other->ca_pem;
    const auto client = WsClient::Connect(options);
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error().kind == WsError::Kind::Failed);
    CHECK(client.error().detail.find("tls") != std::string::npos);

    acceptor.join();
}

}  // namespace lubancode::channel::qq
