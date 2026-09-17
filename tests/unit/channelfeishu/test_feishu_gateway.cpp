// 飞书网关状态机册(飞书/企微设计单 F1,§5):MockWsServer 演真 WS 对端
// (明文 ws://,真升级握手,BinaryMessage 收发),生产 FeishuWsTransport 直
// 接上——零外联。演的幕:引导 → 拨号 → 激活 ping → ping/pong(含
// ClientConfig 覆盖 PingInterval)→ 事件 → ACK(复用原帧 + code)→ 拆包
// 重组(sum>1)→ 缺片到期弃 → 断线重连(重新引导)→ 读超时判死 →
// 凭据类错不重试。退避 scale 压成毫秒,CI 秒级收场。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/feishu/feishu_frame.hpp"
#include "channel/feishu/feishu_gateway.hpp"
#include "platform/wall_clock.hpp"
#include "mock_ws_server.hpp"

namespace lubancode::channel::feishu {
namespace {

// ---------------------------------------------------------------------------
// 服务端侧帧工坊(测试视角的"平台")
// ---------------------------------------------------------------------------

FeishuFrame MakeServerFrame(std::uint64_t seq, std::int32_t service,
                            std::int32_t method,
                            std::vector<FeishuFrameHeader> headers,
                            std::optional<std::string> payload = std::nullopt) {
    FeishuFrame frame;
    frame.seq_id = seq;
    frame.log_id = seq;
    frame.service = service;
    frame.method = method;
    frame.headers = std::move(headers);
    frame.payload = std::move(payload);
    return frame;
}

// 数据帧(事件):type=message_id/sum/seq 头齐全(§5.5)。
FeishuFrame MakeEventFrame(std::uint64_t seq, const std::string& payload_json,
                           const std::string& message_id, std::int64_t sum = 1,
                           std::int64_t seq_no = 1) {
    std::vector<FeishuFrameHeader> headers;
    headers.push_back(FeishuFrameHeader{kHeaderType, kHeaderValueEvent});
    headers.push_back(FeishuFrameHeader{kHeaderMessageId, message_id});
    if (sum > 1) {
        headers.push_back(FeishuFrameHeader{kHeaderSum, std::to_string(sum)});
        headers.push_back(FeishuFrameHeader{kHeaderSeq, std::to_string(seq_no)});
    }
    return MakeServerFrame(seq, 25, kFrameMethodData, std::move(headers),
                           payload_json);
}

// 控制帧(pong):载荷空=纯保活;非空=ClientConfig JSON(§5.4)。
FeishuFrame MakePongFrame(std::uint64_t seq,
                          const std::optional<std::string>& payload = std::nullopt) {
    std::vector<FeishuFrameHeader> headers;
    headers.push_back(FeishuFrameHeader{kHeaderType, kHeaderValuePong});
    return MakeServerFrame(seq, 25, kFrameMethodControl, std::move(headers), payload);
}

const char* kEventPayload = R"({"schema":"2.0","header":{"event_id":"ev_1","event_type":"im.message.receive_v1"},"event":{"message":{"message_id":"om_1","chat_id":"oc_1","chat_type":"p2p","message_type":"text","content":"{\"text\":\"hi\"}"}}})";

// ---------------------------------------------------------------------------
// 网关测试台:endpoint provider 记账 + on_event 收账。
// ---------------------------------------------------------------------------

struct GatewayHarness {
    std::mutex events_mutex;
    std::vector<FeishuGatewayEvent> events;
    std::atomic<bool> stop{false};
    std::atomic<int> bootstrap_count{0};
    // handler 对 MessageReceive 的回执(默认 Ok;测试换 Retry 演 ACK 500)。
    std::atomic<bool> handler_retry{false};
    std::unique_ptr<FeishuGatewaySession> session;
    std::unique_ptr<std::thread> thread;
    // 引导地址(测试注入 mock 服务端;每次重连重新引导——provider 每轮现调)。
    std::string endpoint_url = "ws://127.0.0.1:1/callback?device_id=d1&service_id=25";
    std::int64_t ping_interval_secs = 60;  // 测试窗内不发定时心跳(读超时另案)
    std::int64_t min_ping_interval_secs = 5;
    double backoff_scale = 0.001;  // 秒级阶梯压毫秒
    std::int64_t fragment_window_ms = 5'000;
    // 传输工厂(默认生产件;分型/致命案注 Fake)。
    std::function<std::unique_ptr<IFeishuGatewayTransport>()> transport_factory;

    FeishuGatewaySession::Options MakeOptions() {
        FeishuGatewaySession::Options options;
        options.transport_factory = transport_factory
                                        ? transport_factory
                                        : MakeFeishuWsTransportFactory("", transport::TlsTrustMode::ExplicitCa);
        options.endpoint_provider = [this]() -> std::expected<FeishuEndpoint, FeishuConnectError> {
            bootstrap_count.fetch_add(1);
            FeishuEndpoint endpoint;
            endpoint.url = endpoint_url;
            endpoint.ping_interval_secs = ping_interval_secs;
            return endpoint;
        };
        options.on_event = [this](const FeishuGatewayEvent& event) -> FeishuGatewayAck {
            const std::lock_guard<std::mutex> lock(events_mutex);
            events.push_back(event);
            if (event.kind == FeishuGatewayEvent::Kind::MessageReceive &&
                handler_retry.load()) {
                return FeishuGatewayAck::Retry;
            }
            return FeishuGatewayAck::Ok;
        };
        options.now_ms = []() { return platform::WallClockNowMs(); };
        options.min_ping_interval_secs = min_ping_interval_secs;
        options.fragment_window_ms = fragment_window_ms;
        options.backoff_scale = backoff_scale;
        return options;
    }

    void Start() {
        session = std::make_unique<FeishuGatewaySession>(MakeOptions());
        thread = std::make_unique<std::thread>([this]() { session->RunLoop(&stop); });
    }

    ~GatewayHarness() {
        stop.store(true);
        if (session != nullptr) {
            session->CancelInFlight();
        }
        if (thread != nullptr && thread->joinable()) {
            thread->join();
        }
    }

    template <typename Pred>
    bool WaitForEvent(Pred pred, int timeout_ms = 6'000) {
        const auto deadline = platform::WallClockNowMs() + timeout_ms;
        while (platform::WallClockNowMs() < deadline) {
            {
                const std::lock_guard<std::mutex> lock(events_mutex);
                for (const auto& event : events) {
                    if (pred(event)) {
                        return true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    std::size_t EventCount() {
        const std::lock_guard<std::mutex> lock(events_mutex);
        return events.size();
    }

    std::size_t CountKind(FeishuGatewayEvent::Kind kind) {
        const std::lock_guard<std::mutex> lock(events_mutex);
        std::size_t count = 0;
        for (const auto& event : events) {
            if (event.kind == kind) {
                ++count;
            }
        }
        return count;
    }
};

// 读服务端收到的一条二进制帧并解出 pbbp2(超时给人话)。
std::optional<FeishuFrame> ReadFrame(test_support::MockWsServer::Connection* connection,
                                     int timeout_ms = 5'000) {
    const auto payload = connection->ReadText(timeout_ms);
    if (!payload.has_value()) {
        return std::nullopt;
    }
    std::string error;
    auto frame = DecodeFeishuFrame(*payload, &error);
    if (!frame.has_value()) {
        return std::nullopt;
    }
    return frame;
}

}  // namespace

// ---------------------------------------------------------------------------
// 幕一:引导 → 拨号 → 激活 ping → pong → 事件 → ACK 200
// ---------------------------------------------------------------------------

TEST_CASE("feishu_gateway: 激活即发 ping;事件回 ACK(复用原帧 + code 200 + biz_rt)") {
    test_support::MockWsServer server;
    const int port = *server.Start();
    GatewayHarness harness;
    harness.endpoint_url = "ws://127.0.0.1:" + std::to_string(port) +
                           "/callback?device_id=d1&service_id=25";
    harness.Start();
    // 先收连接、回 101——客户端的 Connect 在升级握手读上阻塞,服务端不
    // 应答就永远到不了 Connected(不能先等 Connected 再 accept)。
    auto connection = *server.AcceptNext(5'000);
    REQUIRE(connection.AcceptUpgrade(5'000).has_value());
    REQUIRE(harness.WaitForEvent([](const FeishuGatewayEvent& event) {
        return event.kind == FeishuGatewayEvent::Kind::Connected;
    }));
    CHECK(harness.bootstrap_count.load() == 1);

    // 激活 ping:连接后第一条,控制帧,type=ping,service=URL 的 service_id。
    const auto ping = ReadFrame(&connection);
    REQUIRE(ping.has_value());
    CHECK(ping->method == kFrameMethodControl);
    CHECK(ping->service == 25);
    CHECK(FeishuFrameHeaderValue(*ping, kHeaderType) == kHeaderValuePing);

    // pong(空载荷=纯保活)。
    REQUIRE(connection.SendBinary(EncodeFeishuFrame(MakePongFrame(100))).has_value());

    // 事件数据帧 → handler 收到 MessageReceive。
    const std::string event_bytes = EncodeFeishuFrame(MakeEventFrame(200, kEventPayload, "om_1"));
    REQUIRE(connection.SendBinary(event_bytes).has_value());
    REQUIRE(harness.WaitForEvent([](const FeishuGatewayEvent& event) {
        return event.kind == FeishuGatewayEvent::Kind::MessageReceive;
    }));
    // ACK:复用原帧(SeqID/service/method 保留)+ biz_rt + payload code 200。
    const auto ack = ReadFrame(&connection);
    REQUIRE(ack.has_value());
    CHECK(ack->seq_id == 200);  // 原帧 SeqID
    CHECK(ack->service == 25);
    CHECK(ack->method == kFrameMethodData);
    CHECK_FALSE(FeishuFrameHeaderValue(*ack, kHeaderBizRt).empty());
    REQUIRE(ack->payload.has_value());
    const auto ack_payload = nlohmann::json::parse(*ack->payload, nullptr, false);
    REQUIRE(ack_payload.is_object());
    CHECK(ack_payload.at("code") == 200);
    CHECK(ack_payload.at("data").is_null());
}

TEST_CASE("feishu_gateway: handler 没接住回 ACK 500(平台重推路)") {
    test_support::MockWsServer server;
    const int port = *server.Start();
    GatewayHarness harness;
    harness.endpoint_url = "ws://127.0.0.1:" + std::to_string(port) + "/cb?service_id=25";
    harness.handler_retry.store(true);
    harness.Start();
    auto connection = *server.AcceptNext(5'000);
    REQUIRE(connection.AcceptUpgrade(5'000).has_value());
    (void)ReadFrame(&connection);  // 激活 ping

    REQUIRE(connection.SendBinary(EncodeFeishuFrame(MakeEventFrame(201, kEventPayload, "om_2"))).has_value());
    REQUIRE(harness.WaitForEvent([](const FeishuGatewayEvent& event) {
        return event.kind == FeishuGatewayEvent::Kind::MessageReceive;
    }));
    const auto ack = ReadFrame(&connection);
    REQUIRE(ack.has_value());
    const auto ack_payload =
        nlohmann::json::parse(*ack->payload, nullptr, false);
    CHECK(ack_payload.at("code") == 500);
}

TEST_CASE("feishu_gateway: card 帧静默丢弃记账,仍回 ACK 200(防重推)") {
    test_support::MockWsServer server;
    const int port = *server.Start();
    GatewayHarness harness;
    harness.endpoint_url = "ws://127.0.0.1:" + std::to_string(port) + "/cb?service_id=25";
    harness.Start();
    auto connection = *server.AcceptNext(5'000);
    REQUIRE(connection.AcceptUpgrade(5'000).has_value());
    (void)ReadFrame(&connection);

    std::vector<FeishuFrameHeader> headers;
    headers.push_back(FeishuFrameHeader{kHeaderType, kHeaderValueCard});
    REQUIRE(connection.SendBinary(EncodeFeishuFrame(
                 MakeServerFrame(300, 25, kFrameMethodData, std::move(headers), "{}")))
                 .has_value());
    // 不进 MessageReceive;UnsupportedEvent 留痕。
    REQUIRE(harness.WaitForEvent([](const FeishuGatewayEvent& event) {
        return event.kind == FeishuGatewayEvent::Kind::UnsupportedEvent;
    }));
    CHECK(harness.CountKind(FeishuGatewayEvent::Kind::MessageReceive) == 0);
    const auto ack = ReadFrame(&connection);
    REQUIRE(ack.has_value());
    const auto ack_payload =
        nlohmann::json::parse(*ack->payload, nullptr, false);
    CHECK(ack_payload.at("code") == 200);
    // 无效载荷(非 JSON)同路:明确终结 + ACK 200。
    REQUIRE(connection.SendBinary(EncodeFeishuFrame(MakeEventFrame(301, "not json", "om_3"))).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(harness.CountKind(FeishuGatewayEvent::Kind::MessageReceive) == 0);
}

// ---------------------------------------------------------------------------
// 幕二:拆包重组(§5.5)
// ---------------------------------------------------------------------------

TEST_CASE("feishu_gateway: sum=2 两片拼齐才派发,单片不 ACK") {
    test_support::MockWsServer server;
    const int port = *server.Start();
    GatewayHarness harness;
    harness.endpoint_url = "ws://127.0.0.1:" + std::to_string(port) + "/cb?service_id=25";
    harness.Start();
    auto connection = *server.AcceptNext(5'000);
    REQUIRE(connection.AcceptUpgrade(5'000).has_value());
    (void)ReadFrame(&connection);

    // 事件载荷劈两半:前半 {"schema":"2.0"…后半。拼齐才是一条完整 JSON。
    const std::string whole = std::string(R"({"schema":"2.0","header":{"event_id":"ev_f","event_type":"im.message.receive_v1"}})");
    const std::string first = whole.substr(0, whole.size() / 2);
    const std::string second = whole.substr(whole.size() / 2);
    REQUIRE(connection.SendBinary(EncodeFeishuFrame(
                 MakeEventFrame(400, first, "om_frag", /*sum=*/2, /*seq_no=*/1)))
                 .has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(harness.CountKind(FeishuGatewayEvent::Kind::MessageReceive) == 0);

    REQUIRE(connection.SendBinary(EncodeFeishuFrame(
                 MakeEventFrame(401, second, "om_frag", /*sum=*/2, /*seq_no=*/2)))
                 .has_value());
    REQUIRE(harness.WaitForEvent([&](const FeishuGatewayEvent& event) {
        return event.kind == FeishuGatewayEvent::Kind::MessageReceive &&
               event.event_payload.at("header").at("event_id") == "ev_f";
    }));
    // 只回一枚 ACK(第二片后)。
    const auto ack = ReadFrame(&connection);
    REQUIRE(ack.has_value());
    CHECK(ack->seq_id == 401);  // 复用收尾片原帧
    const auto ack_payload =
        nlohmann::json::parse(*ack->payload, nullptr, false);
    CHECK(ack_payload.at("code") == 200);
}

TEST_CASE("feishu_gateway: 缺片到期静默弃(无 ACK,平台重推兜底)") {
    test_support::MockWsServer server;
    const int port = *server.Start();
    GatewayHarness harness;
    harness.endpoint_url = "ws://127.0.0.1:" + std::to_string(port) + "/cb?service_id=25";
    harness.fragment_window_ms = 300;  // 压窗换 CI 速度
    harness.Start();
    auto connection = *server.AcceptNext(5'000);
    REQUIRE(connection.AcceptUpgrade(5'000).has_value());
    (void)ReadFrame(&connection);

    REQUIRE(connection.SendBinary(EncodeFeishuFrame(
                 MakeEventFrame(500, R"({"half":)", "om_missing", /*sum=*/2, /*seq_no=*/1)))
                 .has_value());
    // 窗内不 ACK、不派发;到期弃组。
    const auto deadline = platform::WallClockNowMs() + 4'000;
    while (platform::WallClockNowMs() < deadline &&
           harness.session->dropped_fragment_groups() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(harness.session->dropped_fragment_groups() == 1);
    CHECK(harness.CountKind(FeishuGatewayEvent::Kind::MessageReceive) == 0);
    CHECK_FALSE(connection.ReadText(300).has_value());  // 没有 ACK 帧到来
}

// ---------------------------------------------------------------------------
// 幕三:心跳(§5.4)
// ---------------------------------------------------------------------------

TEST_CASE("feishu_gateway: pong 载荷的 ClientConfig 覆盖 PingInterval") {
    test_support::MockWsServer server;
    const int port = *server.Start();
    GatewayHarness harness;
    harness.endpoint_url = "ws://127.0.0.1:" + std::to_string(port) + "/cb?service_id=25";
    harness.ping_interval_secs = 60;  // 初始长间隔(测试窗内不会有定时 ping)
    harness.min_ping_interval_secs = 1;
    harness.Start();
    auto connection = *server.AcceptNext(5'000);
    REQUIRE(connection.AcceptUpgrade(5'000).has_value());
    (void)ReadFrame(&connection);  // 激活 ping

    // pong 载荷下发新配置:PingInterval=1s(夹到 min=1)。
    REQUIRE(connection.SendBinary(EncodeFeishuFrame(
                 MakePongFrame(600, std::string(R"({"PingInterval":1})"))))
                 .has_value());
    // 配置生效后 ~1s 应见下一枚 ping(60s 间隔不会在这个窗内到)。
    const auto next_ping = ReadFrame(&connection, 3'000);
    REQUIRE(next_ping.has_value());
    CHECK(FeishuFrameHeaderValue(*next_ping, kHeaderType) == kHeaderValuePing);
}

TEST_CASE("feishu_gateway: 读超时 2×interval+5s 判死线(半开连接拆除)") {
    test_support::MockWsServer server;
    const int port = *server.Start();
    GatewayHarness harness;
    harness.endpoint_url = "ws://127.0.0.1:" + std::to_string(port) + "/cb?service_id=25";
    harness.ping_interval_secs = 1;
    harness.min_ping_interval_secs = 1;
    harness.backoff_scale = 0.001;
    harness.Start();
    auto connection = *server.AcceptNext(5'000);
    REQUIRE(connection.AcceptUpgrade(5'000).has_value());
    (void)ReadFrame(&connection);  // 激活 ping

    // 服务端从此闭嘴(不回 pong、不发任何帧)→ 读超时 = 2*1s + 5s = 7s。
    REQUIRE(harness.WaitForEvent([](const FeishuGatewayEvent& event) {
        return event.kind == FeishuGatewayEvent::Kind::Disconnected &&
               event.error_code == "read_timeout";
    }, 12'000));
    CHECK(harness.bootstrap_count.load() >= 2);  // 断线重连重新引导
}

// ---------------------------------------------------------------------------
// 幕四:断线重连(§5.9)
// ---------------------------------------------------------------------------

TEST_CASE("feishu_gateway: 服务端掐线 → Disconnected → 退避后重连重新引导") {
    test_support::MockWsServer server;
    const int port = *server.Start();
    GatewayHarness harness;
    harness.endpoint_url = "ws://127.0.0.1:" + std::to_string(port) + "/cb?service_id=25";
    harness.Start();
    auto first = *server.AcceptNext(5'000);
    REQUIRE(first.AcceptUpgrade(5'000).has_value());
    (void)ReadFrame(&first);

    // 在线后掐线。
    REQUIRE(harness.WaitForEvent([](const FeishuGatewayEvent& event) {
        return event.kind == FeishuGatewayEvent::Kind::Connected;
    }));
    first.Drop();
    REQUIRE(harness.WaitForEvent([](const FeishuGatewayEvent& event) {
        return event.kind == FeishuGatewayEvent::Kind::Disconnected;
    }));
    // 退避(压成毫秒)后第二轮:重新引导 + 新连接 + 新激活 ping。
    auto second = *server.AcceptNext(5'000);
    REQUIRE(second.AcceptUpgrade(5'000).has_value());
    const auto ping = ReadFrame(&second);
    REQUIRE(ping.has_value());
    CHECK(FeishuFrameHeaderValue(*ping, kHeaderType) == kHeaderValuePing);
    CHECK(harness.bootstrap_count.load() >= 2);
    REQUIRE(harness.WaitForEvent([](const FeishuGatewayEvent& event) {
        return event.kind == FeishuGatewayEvent::Kind::BackoffScheduled;
    }));
}

// ---------------------------------------------------------------------------
// 幕五:凭据类错不重试(§5.2/§5.9)——Fake 传输注入
// ---------------------------------------------------------------------------

// 脚本假传输:Connect 回注入的连接错误;读恒超时(静默)。
class FakeConnectTransport final : public IFeishuGatewayTransport {
public:
    explicit FakeConnectTransport(FeishuConnectError error)
        : connect_error_(std::move(error)) {}

    std::expected<void, FeishuConnectError> Connect(const std::string&) override {
        return std::unexpected(connect_error_);
    }
    std::expected<void, std::string> SendBinary(const std::string&) override { return {}; }
    std::expected<std::string, transport::WsError> ReadMessage(int) override {
        return std::unexpected(transport::WsError{transport::WsError::Kind::Timeout,
                                                  "silent", 0});
    }
    void Cancel() override {}
    void Close(std::uint16_t, const std::string&) override {}

private:
    FeishuConnectError connect_error_;
};

TEST_CASE("feishu_gateway: 凭据类错不重试——RunLoop 收口,不再二连") {
    GatewayHarness harness;
    std::atomic<int> connects{0};
    harness.transport_factory = [&connects]() {
        connects.fetch_add(1);
        return std::unique_ptr<IFeishuGatewayTransport>(
            std::make_unique<FakeConnectTransport>(FeishuConnectError{
                kStageConnecting, "feishu_handshake_auth_failed",
                "handshake auth failed(514)", /*non_retryable=*/true}));
    };
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const FeishuGatewayEvent& event) {
        return event.kind == FeishuGatewayEvent::Kind::ConnectFailed;
    }));
    REQUIRE(harness.WaitForEvent([](const FeishuGatewayEvent& event) {
        return event.kind == FeishuGatewayEvent::Kind::Stopped;
    }, 4'000));
    // 不重试:传输只造了一次,退避排程不该出现。
    CHECK(connects.load() == 1);
    CHECK(harness.CountKind(FeishuGatewayEvent::Kind::BackoffScheduled) == 0);
    {
        const std::lock_guard<std::mutex> lock(harness.events_mutex);
        bool saw_non_retryable = false;
        for (const auto& event : harness.events) {
            if (event.kind == FeishuGatewayEvent::Kind::ConnectFailed && event.non_retryable) {
                saw_non_retryable = true;
            }
        }
        CHECK(saw_non_retryable);
    }
}

TEST_CASE("feishu_gateway: 服务端类错照常退避重连") {
    GatewayHarness harness;
    std::atomic<int> connects{0};
    harness.transport_factory = [&connects]() {
        connects.fetch_add(1);
        return std::unique_ptr<IFeishuGatewayTransport>(
            std::make_unique<FakeConnectTransport>(FeishuConnectError{
                kStageConnecting, "feishu_handshake_server_error",
                "handshake server error", /*non_retryable=*/false}));
    };
    harness.Start();
    // 退避阶梯(压毫秒)滚几轮:Connect 多次、BackoffScheduled 出现。
    REQUIRE(harness.WaitForEvent([](const FeishuGatewayEvent& event) {
        return event.kind == FeishuGatewayEvent::Kind::BackoffScheduled;
    }));
    const auto deadline = platform::WallClockNowMs() + 3'000;
    while (platform::WallClockNowMs() < deadline && connects.load() < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(connects.load() >= 3);
}

// ---------------------------------------------------------------------------
// 握手失败分型纯函数表(§5.2)
// ---------------------------------------------------------------------------

TEST_CASE("feishu_gateway: 握手失败分型——Handshake-Status/Autherrcode 裁决表") {
    const auto make = [](const std::string& headers) {
        return transport::WsError{transport::WsError::Kind::Protocol,
                                  "upgrade not accepted", 0, {}, headers};
    };
    // 514 = AuthFailed:凭据类,不重试。
    FeishuConnectError auth = ClassifyFeishuHandshakeFailure(
        make("HTTP/1.1 514 Internal Error\r\nHandshake-Status: 514\r\n"
             "Handshake-Msg: app_id or app_secret wrong\r\n\r\n"));
    CHECK(auth.error_code == "feishu_handshake_auth_failed");
    CHECK(auth.non_retryable);
    // 403 = Forbidden:凭据类。
    FeishuConnectError forbidden = ClassifyFeishuHandshakeFailure(
        make("HTTP/1.1 403 Forbidden\r\nHandshake-Status: 403\r\n\r\n"));
    CHECK(forbidden.error_code == "feishu_handshake_forbidden");
    CHECK(forbidden.non_retryable);
    // ExceedConnLimit(1000040350):凭据类(连接数超限)。
    FeishuConnectError exceed = ClassifyFeishuHandshakeFailure(
        make("HTTP/1.1 403\r\nHandshake-Status: 403\r\n"
             "Handshake-Autherrcode: 1000040350\r\n\r\n"));
    CHECK(exceed.error_code == "feishu_exceed_conn_limit");
    CHECK(exceed.non_retryable);
    // 服务端错:可重试。
    FeishuConnectError server_error = ClassifyFeishuHandshakeFailure(
        make("HTTP/1.1 500\r\nHandshake-Status: 500\r\n\r\n"));
    CHECK(server_error.error_code == "feishu_handshake_server_error");
    CHECK_FALSE(server_error.non_retryable);
    // Handshake-Status 缺省:回退 HTTP 码(§5.2);HTTP 也读不出按拒绝。
    FeishuConnectError fallback = ClassifyFeishuHandshakeFailure(
        make("HTTP/1.1 503 Service Unavailable\r\n\r\n"));
    CHECK(fallback.error_code == "feishu_handshake_server_error");
    CHECK_FALSE(fallback.non_retryable);
    // 无握手响应头(网络/TLS/超时):按 kind 分,可重试。
    FeishuConnectError no_headers = ClassifyFeishuHandshakeFailure(
        transport::WsError{transport::WsError::Kind::Timeout, "connect timeout", 0});
    CHECK(no_headers.error_code == "connect_timeout");
    CHECK_FALSE(no_headers.non_retryable);
}

}  // namespace
