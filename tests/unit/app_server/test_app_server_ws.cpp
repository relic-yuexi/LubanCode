// WS 承载单(多前端外壳阶段 A)的测试册:
//   1. 纯函数层:SHA-1/base64/握手算料(RFC 向量钉死)、帧编解码(掩码、
//      分片、控制帧、协议错分型);
//   2. token 门的首帧形状(纯函数);
//   3. 真监听回环:升级 → 握手 → 方法 → 事件带 seq → 断线 → 重连
//      (dispatcher 重铸、seq 不回卷)→ exit 收线;
//   4. token 门的线上行为:错 token 即断,对 token 放行。
// 真监听走 127.0.0.1 + 系统分配端口(port 0),不起进程、不碰 stdio。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "app_server/connection.hpp"
#include "app_server/protocol.hpp"
#include "app_server/server.hpp"
#include "app_server/ws_frames.hpp"
#include "app_server/ws_sockets.hpp"
#include "app_server/ws_transport.hpp"

using namespace lubancode;

namespace {

// ---- 帧字节的小工具 ----

std::string Hex(std::string_view bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const char byte : bytes) {
        out.push_back(digits[(static_cast<unsigned char>(byte) >> 4) & 0xF]);
        out.push_back(digits[static_cast<unsigned char>(byte) & 0xF]);
    }
    return out;
}

std::string Bytes(std::initializer_list<std::uint8_t> bytes) {
    std::string out;
    for (const std::uint8_t byte : bytes) {
        out.push_back(static_cast<char>(byte));
    }
    return out;
}

// 客户端出帧(必须掩码):opcode/fin/载荷/掩码键都由调用方定。
std::string MaskedFrame(std::uint8_t opcode, bool fin, std::string_view payload,
                        const std::uint8_t mask[4]) {
    std::string frame;
    frame.push_back(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> shift) & 0xFF));
        }
    }
    frame.push_back(static_cast<char>(mask[0]));
    frame.push_back(static_cast<char>(mask[1]));
    frame.push_back(static_cast<char>(mask[2]));
    frame.push_back(static_cast<char>(mask[3]));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4]));
    }
    return frame;
}

// ---- 测试用 WS 客户端:裸 socket + 手搓升级与帧 ----

class TestWsClient {
public:
    explicit TestWsClient(int port) {
        std::string error;
        socket_ = app_server::net::ConnectTcp("127.0.0.1", port, error);
        REQUIRE_MESSAGE(socket_.valid(), ("connect 失败: " + error).c_str());
        // 收超时:让"排干事件"的循环在静默 300ms 后收口,不吊死测试。
        socket_.SetRecvTimeoutMs(300);
    }

    bool Upgrade() {
        const std::string key = "dGhlIHNhbXBsZSBub25jZQ=="; // RFC 6455 §1.3 的样例键
        std::string request;
        request += "GET /ws HTTP/1.1\r\n";
        request += "Host: 127.0.0.1\r\n";
        request += "Upgrade: websocket\r\n";
        request += "Connection: keep-alive, Upgrade\r\n";
        request += "Sec-WebSocket-Key: " + key + "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n";
        request += "Sec-WebSocket-Extensions: permessage-deflate\r\n";
        request += "\r\n";
        if (!socket_.SendAll(request)) {
            return false;
        }
        std::string header;
        while (header.find("\r\n\r\n") == std::string::npos) {
            char buffer[1024];
            const long got = socket_.Recv(buffer, sizeof(buffer));
            if (got <= 0) {
                return false;
            }
            header.append(buffer, buffer + got);
        }
        // 101 + accept 对账;扩展头不许回(没谈成压缩)。
        if (header.find("HTTP/1.1 101") == std::string::npos) {
            return false;
        }
        if (header.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == std::string::npos) {
            return false;
        }
        if (header.find("permessage-deflate") != std::string::npos) {
            return false;
        }
        // 101 之后服务端不会再塞字节,残余(若有)留给帧读。
        rest_after_upgrade_ = header.substr(header.find("\r\n\r\n") + 4);
        return true;
    }

    void SendText(const std::string& message) {
        static const std::uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
        socket_.SendAll(MaskedFrame(0x1, true, message, mask));
    }

    // 读一条服务端文本帧(不掩码)。close/断/EOF/静默超时给 nullopt。
    // 读到的一切都记进 seen(对账用)。
    std::optional<std::string> ReadText() {
        if (!rest_after_upgrade_.empty()) {
            Feed(rest_after_upgrade_);
            rest_after_upgrade_.clear();
        }
        while (inbox_.empty()) {
            if (failed_) {
                return std::nullopt;
            }
            char buffer[65536];
            const long got = socket_.Recv(buffer, sizeof(buffer));
            if (got <= 0) {
                return std::nullopt;
            }
            Feed(std::string_view(buffer, buffer + got));
        }
        std::string message = std::move(inbox_.front());
        inbox_.pop_front();
        seen.push_back(message);
        return message;
    }

    void HardClose() {
        socket_.Close(); // 不发 close 帧:模拟断线(网络掉/进程被杀)
    }

    app_server::net::Socket socket_;
    std::vector<std::string> seen; // 全部读到的原始消息(事件对账用)

private:
    // 服务端帧解码(不掩码;控制帧当收线处理)。
    void Feed(std::string_view chunk) {
        buffer_.append(chunk.begin(), chunk.end());
        while (true) {
            if (buffer_.size() < 2) {
                return;
            }
            const std::uint8_t first = static_cast<std::uint8_t>(buffer_[0]);
            const std::uint8_t second = static_cast<std::uint8_t>(buffer_[1]);
            const bool fin = (first & 0x80) != 0;
            const std::uint8_t opcode = first & 0x0F;
            if ((first & 0x70) != 0) {
                failed_ = true;
                return;
            }
            std::size_t header_size = 2;
            std::uint64_t length = second & 0x7F;
            if (length == 126) {
                if (buffer_.size() < 4) {
                    return;
                }
                length = (static_cast<std::uint8_t>(buffer_[2]) << 8) |
                         static_cast<std::uint8_t>(buffer_[3]);
                header_size = 4;
            } else if (length == 127) {
                if (buffer_.size() < 10) {
                    return;
                }
                length = 0;
                for (int i = 0; i < 8; ++i) {
                    length = (length << 8) | static_cast<std::uint8_t>(buffer_[2 + i]);
                }
                header_size = 10;
            }
            if (buffer_.size() < header_size + length) {
                return;
            }
            const std::string payload = buffer_.substr(header_size, static_cast<std::size_t>(length));
            buffer_.erase(0, header_size + static_cast<std::size_t>(length));
            if (opcode >= 0x8) {
                if (opcode == 0x8 && fin) {
                    failed_ = true; // 对端收线
                }
                continue;
            }
            if (!fin || opcode != 0x1) {
                failed_ = true; // 测试里服务端不该发这些
                return;
            }
            inbox_.push_back(payload);
        }
    }

    std::string rest_after_upgrade_; // 升级应答后的残余字节(应为空)
    std::string buffer_;
    std::deque<std::string> inbox_;
    bool failed_ = false;
};

// ---- 假后端(与 turn 册同款:按脚本吐事件) ----

class ScriptedBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;

    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        if (call_count >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "ScriptedBackend: 脚本用完了", 0});
        }
        for (const api::StreamEvent& event : scripts[call_count]) {
            on_event(event);
        }
        ++call_count;
        return {};
    }
    std::size_t call_count = 0;
};

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "fake-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{10, 5, 0, 0, 0}},
    };
}

std::string MakeTempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

// 一台 WS 模式的测试服务:假后端 + 自持 transport,连接一条条喂。
app_server::WsOptions WsOptionsFor(const std::string& token) {
    app_server::WsOptions ws;
    ws.bind_host = "127.0.0.1";
    ws.port = 0; // 系统分配,测试互不撞端口
    ws.token = token;
    return ws;
}

struct WsHarness {
    std::shared_ptr<ScriptedBackend> backend = std::make_shared<ScriptedBackend>();
    std::unique_ptr<app_server::Server> server;
    app_server::WsTransport transport;

    explicit WsHarness(const std::string& token = "")
        : transport(WsOptionsFor(token)) {
        app_server::ServerOptions options;
        options.cwd = "/test/cwd";
        options.outbox_capacity = 256;
        server = std::make_unique<app_server::Server>(
            std::move(options),
            [this]() -> std::unique_ptr<api::Backend> {
                auto fresh = std::make_unique<ScriptedBackend>();
                fresh->scripts = backend->scripts;
                return fresh;
            },
            nullptr);
        REQUIRE(transport.Start());
    }

    ~WsHarness() {
        transport.Stop();
    }

    int port() const { return transport.actual_port(); }

    // 起一条连接的服务线程(阻塞到连接收线)。
    std::thread Serve() {
        return std::thread([this] {
            outcome_.store(static_cast<int>(server->ServeWsConnection(transport)));
        });
    }

    bool exit_requested() const {
        return static_cast<app_server::Server::WsServeOutcome>(outcome_.load()) ==
               app_server::Server::WsServeOutcome::ExitRequested;
    }
    bool disconnected() const {
        return static_cast<app_server::Server::WsServeOutcome>(outcome_.load()) ==
               app_server::Server::WsServeOutcome::Disconnected;
    }

private:
    std::atomic<int> outcome_{static_cast<int>(app_server::Server::WsServeOutcome::Disconnected)};
};

// 收事件直到谓词命中或超时。
std::optional<nlohmann::json> WaitForMessage(TestWsClient& client,
                                             const std::function<bool(const nlohmann::json&)>& match,
                                             int tries = 200) {
    for (int i = 0; i < tries; ++i) {
        while (const std::optional<std::string> message = client.ReadText()) {
            const nlohmann::json parsed = nlohmann::json::parse(*message, nullptr, false);
            if (!parsed.is_discarded() && match(parsed)) {
                return parsed;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// 纯函数:握手算料
// ---------------------------------------------------------------------------

TEST_CASE("ws 握手算料:SHA-1/base64/accept(RFC 向量)") {
    CHECK(Hex(std::string_view(reinterpret_cast<const char*>(
                  app_server::ws::Sha1Digest("abc").data()), 20)) ==
          "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(Hex(std::string_view(reinterpret_cast<const char*>(
                  app_server::ws::Sha1Digest("").data()), 20)) ==
          "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(app_server::ws::Base64Encode("abc") == "YWJj");
    CHECK(app_server::ws::Base64Encode("ab") == "YWI=");
    CHECK(app_server::ws::Base64Encode("a") == "YQ==");
    CHECK(app_server::ws::Base64Encode("Man") == "TWFu");
    // RFC 6455 §1.3 的样例:dGhlIHNhbXBsZSBub25jZQ== -> s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
    CHECK(app_server::ws::ComputeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") ==
          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("ws 升级请求解析:认得起就给键,认不起就给人话") {
    const std::string request =
        "GET /chat HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "UPGRADE: WebSocket\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "sec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    const auto parsed = app_server::ws::ParseUpgradeRequest(request);
    REQUIRE(parsed.valid);
    CHECK(parsed.websocket_key == "dGhlIHNhbXBsZSBub25jZQ==");
    const std::string response =
        app_server::ws::MakeUpgradeResponse(app_server::ws::ComputeAcceptKey(parsed.websocket_key));
    CHECK(response.find("HTTP/1.1 101 Switching Protocols") == 0);
    CHECK(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    CHECK(response.find("permessage") == std::string::npos);

    CHECK_FALSE(app_server::ws::ParseUpgradeRequest("POST /x HTTP/1.1\r\n\r\n").valid);
    CHECK_FALSE(app_server::ws::ParseUpgradeRequest("GET /x HTTP/1.1\r\nUpgrade: h2c\r\n\r\n").valid);
    CHECK_FALSE(app_server::ws::ParseUpgradeRequest(
                    "GET /x HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n")
                    .valid);
    CHECK_FALSE(app_server::ws::ParseUpgradeRequest(
                    "GET /x HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Key: k\r\nSec-WebSocket-Version: 8\r\n\r\n")
                    .valid);
}

// ---------------------------------------------------------------------------
// 纯函数:帧编解码
// ---------------------------------------------------------------------------

TEST_CASE("ws 出帧:文本帧字节钉死(小/中/大三档长度)") {
    CHECK(Hex(app_server::ws::MakeTextFrame("Hello")) == "810548656c6c6f");
    const std::string medium(200, 'x');
    const std::string medium_frame = app_server::ws::MakeTextFrame(medium);
    CHECK(static_cast<std::uint8_t>(medium_frame[1]) == 126);
    CHECK(static_cast<std::uint8_t>(medium_frame[2]) == 0x00);
    CHECK(static_cast<std::uint8_t>(medium_frame[3]) == 200);
    CHECK(medium_frame.size() == 4 + 200);
    const std::string large(70000, 'y');
    const std::string large_frame = app_server::ws::MakeTextFrame(large);
    CHECK(static_cast<std::uint8_t>(large_frame[1]) == 127);
    CHECK(large_frame.size() == 10 + 70000);
    // close 帧:FIN+close(0x88)、载荷 2 字节(0x02)、码 1000(0x03e8)。
    CHECK(Hex(app_server::ws::MakeCloseFrame(1000)) == "880203e8");
}

TEST_CASE("ws 入帧:掩码解开,一条文本一条消息") {
    static const std::uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    // RFC 6455 §5.7 的样例:掩码 "Hello"。
    const std::string frame = Bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58});
    app_server::ws::FrameDecoder decoder;
    const auto events = decoder.Feed(frame);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == app_server::ws::FrameEvent::Kind::Text);
    CHECK(events[0].payload == "Hello");
}

TEST_CASE("ws 入帧:劈包与挤包") {
    static const std::uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    const std::string frame = MaskedFrame(0x1, true, R"({"a":1})", mask);
    // 逐字节喂:残帧不吐,最后一个字节凑齐了才吐一条。
    app_server::ws::FrameDecoder slow;
    std::vector<app_server::ws::FrameEvent> slow_events;
    for (std::size_t i = 0; i < frame.size(); ++i) {
        const auto events = slow.Feed(std::string_view(frame.data() + i, 1));
        slow_events.insert(slow_events.end(), events.begin(), events.end());
        if (i + 1 < frame.size()) {
            CHECK(events.empty()); // 没到齐不许吐
        }
    }
    REQUIRE(slow_events.size() == 1);
    CHECK(slow_events[0].payload == R"({"a":1})");
    // 挤包:两条消息一段到。
    app_server::ws::FrameDecoder fast;
    const std::string two = MaskedFrame(0x1, true, "first", mask) + MaskedFrame(0x1, true, "second", mask);
    const auto events = fast.Feed(two);
    REQUIRE(events.size() == 2);
    CHECK(events[0].payload == "first");
    CHECK(events[1].payload == "second");
}

TEST_CASE("ws 入帧:分片拼装,ping 插队照应") {
    static const std::uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    app_server::ws::FrameDecoder decoder;
    const auto first = decoder.Feed(MaskedFrame(0x1, false, "Hello,", mask));
    CHECK(first.empty()); // 没拼完不吐
    const auto ping = decoder.Feed(MaskedFrame(0x9, true, "ha", mask));
    REQUIRE(ping.size() == 1);
    CHECK(ping[0].kind == app_server::ws::FrameEvent::Kind::Ping);
    CHECK(ping[0].payload == "ha");
    const auto done = decoder.Feed(MaskedFrame(0x0, true, " world", mask));
    REQUIRE(done.size() == 1);
    CHECK(done[0].kind == app_server::ws::FrameEvent::Kind::Text);
    CHECK(done[0].payload == "Hello, world");
}

TEST_CASE("ws 入帧:协议错分型,报了废") {
    static const std::uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    SUBCASE("客户端帧不掩码") {
        app_server::ws::FrameDecoder decoder;
        const auto events = decoder.Feed(Bytes({0x81, 0x02, 'h', 'i'}));
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == app_server::ws::FrameEvent::Kind::Error);
        CHECK(decoder.failed());
    }
    SUBCASE("binary 帧不收") {
        app_server::ws::FrameDecoder decoder;
        const auto events = decoder.Feed(MaskedFrame(0x2, true, "xx", mask));
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == app_server::ws::FrameEvent::Kind::Error);
    }
    SUBCASE("rsv 位不谈") {
        app_server::ws::FrameDecoder decoder;
        std::string frame = MaskedFrame(0x1, true, "xx", mask);
        frame[0] = static_cast<char>(static_cast<std::uint8_t>(frame[0]) | 0x40);
        const auto events = decoder.Feed(frame);
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == app_server::ws::FrameEvent::Kind::Error);
    }
    SUBCASE("没有起帧的续帧") {
        app_server::ws::FrameDecoder decoder;
        const auto events = decoder.Feed(MaskedFrame(0x0, true, "xx", mask));
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == app_server::ws::FrameEvent::Kind::Error);
    }
    SUBCASE("超长声明直接废") {
        app_server::ws::FrameDecoder decoder;
        std::string frame;
        frame.push_back(static_cast<char>(0x81));               // text + fin
        frame.push_back(static_cast<char>(0x80 | 127));         // 掩码 + 64 位长度
        frame.append("\x00\x00\x00\x00\x01\x00\x00\x00", 8);   // 声明 4GB,超上限
        frame.append(4, '\x11');                                // 掩码键
        const auto events = decoder.Feed(frame);
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == app_server::ws::FrameEvent::Kind::Error);
    }
    SUBCASE("close 帧:事件吐出,解码器收摊") {
        app_server::ws::FrameDecoder decoder;
        const auto events = decoder.Feed(MaskedFrame(0x8, true, std::string("\x03\xe8", 2), mask));
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == app_server::ws::FrameEvent::Kind::Close);
        CHECK(decoder.Feed("junk").empty());
    }
}

// ---------------------------------------------------------------------------
// token 门:首帧形状(纯函数)
// ---------------------------------------------------------------------------

TEST_CASE("ws token 门:首帧形状与恒时对账") {
    const std::string good = R"({"method":"app_server/auth","params":{"token":"s3cret"}})";
    CHECK(app_server::CheckAuthTokenFrame(good, "s3cret"));
    CHECK_FALSE(app_server::CheckAuthTokenFrame(good, "s3cretX"));
    CHECK_FALSE(app_server::CheckAuthTokenFrame(good, "s3cre"));
    CHECK_FALSE(app_server::CheckAuthTokenFrame(good, ""));
    // 形状不对:别的方法/缺 token/坏 JSON 一律不过;多带不相干字段不拦。
    CHECK_FALSE(app_server::CheckAuthTokenFrame(R"({"method":"initialize","params":{}})", "s3cret"));
    CHECK_FALSE(app_server::CheckAuthTokenFrame(R"({"method":"app_server/auth"})", "s3cret"));
    CHECK_FALSE(app_server::CheckAuthTokenFrame("not json", "s3cret"));
    CHECK(app_server::CheckAuthTokenFrame(
        R"({"method":"app_server/auth","params":{"token":"s3cret"},"id":1})", "s3cret"));
}

// ---------------------------------------------------------------------------
// 真监听回环:一幕一幕
// ---------------------------------------------------------------------------

TEST_CASE("ws 承载:升级 → initialize → thread/turn → 事件带 seq") {
    WsHarness harness;
    harness.backend->scripts = {TextOnlyScript("你好,WS")};
    std::thread server_thread = harness.Serve();

    TestWsClient client(harness.port());
    REQUIRE(client.Upgrade());

    // 握手:协议版本与能力表。
    client.SendText(R"({"id":1,"method":"initialize","params":{"clientName":"ws-test"}})");
    const auto init = WaitForMessage(
        client, [](const nlohmann::json& message) { return message.contains("id"); });
    REQUIRE(init.has_value());
    CHECK((*init)["id"] == 1);
    CHECK((*init)["result"]["protocolVersion"] == "1.1");
    client.SendText(R"({"method":"initialized"})");

    // thread/start:threadId 发事件 + 回响应。
    client.SendText(R"({"id":2,"method":"thread/start","params":{}})");
    const auto thread_started = WaitForMessage(
        client, [](const nlohmann::json& message) { return message.value("method", "") == "thread/started"; });
    REQUIRE(thread_started.has_value());
    const std::string thread_id = (*thread_started)["params"]["threadId"];
    CHECK((*thread_started)["params"].contains("seq"));
    const auto thread_reply = WaitForMessage(
        client, [](const nlohmann::json& message) { return message.contains("id") && message["id"] == 2; });
    REQUIRE(thread_reply.has_value());
    CHECK((*thread_reply)["result"]["threadId"] == thread_id);

    // turn/start:受理即回,事件流(item/started → delta → completed →
    // turn/completed)逐条带 seq 且单调。
    client.SendText(R"({"id":3,"method":"turn/start","params":{"threadId":")" + thread_id +
                    R"(","text":"问一句"}})");
    const auto turn_done = WaitForMessage(
        client, [](const nlohmann::json& message) {
            return message.value("method", "") == "turn/completed";
        },
        600);
    REQUIRE(turn_done.has_value());
    CHECK((*turn_done)["params"]["status"] == "success");
    CHECK((*turn_done)["params"]["seq"].is_number());

    // 收到的所有事件 seq 单调递增(连接层统一盖,每连接一条单调序列)。
    std::uint64_t last_seq = 0;
    bool seq_monotonic = true;
    int event_count = 0;
    for (const std::string& line : client.seen) {
        const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("method") || !parsed["params"].contains("seq")) {
            continue;
        }
        const std::uint64_t seq = parsed["params"]["seq"];
        if (seq <= last_seq) {
            seq_monotonic = false;
        }
        last_seq = seq;
        ++event_count;
    }
    CHECK(event_count >= 3); // turn/started + item/* + turn/completed 至少
    CHECK(seq_monotonic);

    client.HardClose();
    server_thread.join();
    CHECK(harness.disconnected()); // 断线:连接收线,不是整场
    CHECK_FALSE(harness.exit_requested());
}

TEST_CASE("ws 承载:断线重连,dispatcher 重铸,seq 不回卷") {
    WsHarness harness;
    harness.backend->scripts = {TextOnlyScript("第一回合")};
    std::uint64_t first_max_seq = 0;

    {
        std::thread server_thread = harness.Serve();
        TestWsClient client(harness.port());
        REQUIRE(client.Upgrade());
        client.SendText(R"({"id":1,"method":"initialize","params":{}})");
        REQUIRE(WaitForMessage(client, [](const nlohmann::json& m) { return m.contains("id"); }));
        client.SendText(R"({"method":"initialized"})");
        client.SendText(R"({"id":2,"method":"thread/start","params":{}})");
        const auto started = WaitForMessage(
            client, [](const nlohmann::json& m) { return m.value("method", "") == "thread/started"; });
        REQUIRE(started.has_value());
        first_max_seq = (*started)["params"]["seq"];
        client.HardClose();
        server_thread.join();
    }

    // 第二条连接:握手从头来(每连接自己的 dispatcher——旧连接 initialize
    // 过不影响新连接必须重新 initialize)。
    harness.backend->scripts = {TextOnlyScript("第二回合")};
    std::uint64_t second_max_seq = 0;
    {
        std::thread server_thread = harness.Serve();
        TestWsClient client(harness.port());
        REQUIRE(client.Upgrade());
        // 不先 initialize 就发业务:必须吃 -32002(握手状态机是连接级的)。
        client.SendText(R"({"id":9,"method":"thread/list","params":{}})");
        const auto rejected = WaitForMessage(
            client, [](const nlohmann::json& m) { return m.contains("id") && m["id"] == 9; });
        REQUIRE(rejected.has_value());
        CHECK((*rejected)["error"]["code"] == -32002);
        // 重新握手,业务放行。
        client.SendText(R"({"id":10,"method":"initialize","params":{}})");
        REQUIRE(WaitForMessage(
            client, [](const nlohmann::json& m) { return m.contains("id") && m["id"] == 10; }));
        client.SendText(R"({"method":"initialized"})");
        client.SendText(R"({"id":11,"method":"thread/list","params":{}})");
        const auto listed = WaitForMessage(
            client, [](const nlohmann::json& m) { return m.contains("id") && m["id"] == 11; });
        REQUIRE(listed.has_value());
        CHECK((*listed)["result"].contains("threads"));
        // 再开一场 thread:新事件的 seq 必须越过上一条连接见过的最大值——
        // 进程级发号不回卷,重连凭 cursor 补账永不撞号。
        client.SendText(R"({"id":12,"method":"thread/start","params":{}})");
        const auto started2 = WaitForMessage(
            client, [](const nlohmann::json& m) { return m.value("method", "") == "thread/started"; });
        REQUIRE(started2.has_value());
        second_max_seq = (*started2)["params"]["seq"];
        CHECK(second_max_seq > first_max_seq);
        client.HardClose();
        server_thread.join();
    }
    CHECK(first_max_seq > 0);
    CHECK(second_max_seq > first_max_seq);
}

TEST_CASE("ws 承载:exit 通知收整场") {
    WsHarness harness;
    std::thread server_thread = harness.Serve();
    TestWsClient client(harness.port());
    REQUIRE(client.Upgrade());
    client.SendText(R"({"id":1,"method":"initialize","params":{}})");
    REQUIRE(WaitForMessage(client, [](const nlohmann::json& m) { return m.contains("id"); }));
    client.SendText(R"({"method":"initialized"})");
    client.SendText(R"({"method":"exit"})");
    server_thread.join();
    CHECK(harness.exit_requested()); // exit:整场收线(stdio 同义)
}

TEST_CASE("ws 承载:token 门——错即断,对放行") {
    WsHarness harness("test-token-值");
    std::thread server_thread = harness.Serve();

    // 第一家:不交 token,直接 initialize——首帧不是 auth,断。
    {
        TestWsClient client(harness.port());
        REQUIRE(client.Upgrade());
        client.SendText(R"({"id":1,"method":"initialize","params":{}})");
        CHECK(client.ReadText() == std::nullopt); // 断线:连接被服务端关了
    }
    // 第二家:错 token,断。
    {
        TestWsClient client(harness.port());
        REQUIRE(client.Upgrade());
        client.SendText(R"({"method":"app_server/auth","params":{"token":"wrong"}})");
        CHECK(client.ReadText() == std::nullopt);
    }
    // 第三家:对 token,放行且业务照常。
    {
        TestWsClient client(harness.port());
        REQUIRE(client.Upgrade());
        client.SendText(R"({"method":"app_server/auth","params":{"token":"test-token-值"}})");
        client.SendText(R"({"id":1,"method":"initialize","params":{}})");
        const auto init = WaitForMessage(
            client, [](const nlohmann::json& m) { return m.contains("id") && m["id"] == 1; });
        REQUIRE(init.has_value());
        CHECK((*init)["result"]["protocolVersion"] == "1.1");
        client.HardClose();
    }
    server_thread.join();
    CHECK_FALSE(harness.exit_requested());
}
