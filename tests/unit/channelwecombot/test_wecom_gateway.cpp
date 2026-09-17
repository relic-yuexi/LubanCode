// 企微网关状态机册(W1,设计单 §六):订阅→心跳→回调分发→退避重连;
// 凭据错即止;disconnected_event 让位;递交口(req_id 回执匹配/超时/
// 收口失败)。FakeTransport 注入假流(零网络)钉状态迁移;末尾一案用
// MockWsServer 真socket 真帧过一遍(生产传输工厂,回环 ws://)。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/qq/qq_gateway.hpp"
#include "channel/wecombot/wecom_gateway.hpp"
#include "channel/wecombot/wecom_proto.hpp"
#include "mock_ws_server.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::channel::wecombot {
namespace {

// 假传输:连接脚本化。Connect 即在游标处插一发订阅回执(官方语义:
// 订阅应答是连接后客户端读到的第一类帧;插队头保证重连脚本顺序不乱),
// errcode 由 Shared.subscribe_errcode 控制(silent = 不插,测订阅超时)。
// ReadMessage 按队列吐,耗尽后恒回 exhausted_error(默认 Timeout=静默)。
class FakeTransport final : public WecomTransport {
public:
    struct Shared {
        std::mutex mutex;
        std::vector<std::string> sent;
        std::vector<std::string> connect_urls;
        std::vector<std::string> incoming;
        std::size_t incoming_cursor = 0;
        transport::WsError exhausted_error{transport::WsError::Kind::Timeout, "script exhausted", 0};
        bool fail_connect = false;
        std::int64_t subscribe_errcode = 0;
        bool subscribe_silent = false;
    };

    explicit FakeTransport(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}

    std::expected<void, channel::qq::GatewayConnectError> Connect(
        const std::string& url) override {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->connect_urls.push_back(url);
        if (shared_->fail_connect) {
            return std::unexpected(channel::qq::GatewayConnectError{
                kStageConnecting, "connect_refused", "connect refused"});
        }
        if (!shared_->subscribe_silent) {
            nlohmann::json ack = nlohmann::json::object();
            ack["headers"] = nlohmann::json::object();
            ack["errcode"] = shared_->subscribe_errcode;
            ack["errmsg"] = shared_->subscribe_errcode == 0 ? "ok" : "err";
            shared_->incoming.insert(
                shared_->incoming.begin() + static_cast<std::ptrdiff_t>(
                                                std::min(shared_->incoming_cursor,
                                                         shared_->incoming.size())),
                ack.dump());
        }
        return {};
    }

    std::expected<void, std::string> SendText(const std::string& text) override {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->sent.push_back(text);
        return {};
    }

    std::expected<std::string, transport::WsError> ReadMessage(int) override {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->incoming_cursor < shared_->incoming.size()) {
            return shared_->incoming[shared_->incoming_cursor++];
        }
        return std::unexpected(shared_->exhausted_error);
    }

    void Cancel() override {}
    void Close(std::uint16_t, const std::string&) override {}

    static bool WaitForSent(const std::shared_ptr<Shared>& shared, const std::string& needle,
                            int attempts = 300) {
        for (int i = 0; i < attempts; ++i) {
            {
                const std::lock_guard<std::mutex> lock(shared->mutex);
                for (const std::string& text : shared->sent) {
                    if (text.find(needle) != std::string::npos) {
                        return true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    static void PushIncoming(const std::shared_ptr<Shared>& shared, std::string text) {
        const std::lock_guard<std::mutex> lock(shared->mutex);
        shared->incoming.push_back(std::move(text));
    }

    // 按最新已发帧生成回执(req_id 透传,模拟平台应答 ping/respond)。
    static void AckLast(const std::shared_ptr<Shared>& shared, std::int64_t errcode) {
        std::string req_id;
        {
            const std::lock_guard<std::mutex> lock(shared->mutex);
            REQUIRE_FALSE(shared->sent.empty());
            const auto frame = nlohmann::json::parse(shared->sent.back());
            req_id = frame.at("headers").at("req_id").get<std::string>();
        }
        nlohmann::json ack = nlohmann::json::object();
        ack["headers"] = nlohmann::json{{"req_id", req_id}};
        ack["errcode"] = errcode;
        ack["errmsg"] = errcode == 0 ? "ok" : "err";
        PushIncoming(shared, ack.dump());
    }

    static std::vector<std::string> Sent(const std::shared_ptr<Shared>& shared) {
        const std::lock_guard<std::mutex> lock(shared->mutex);
        return shared->sent;
    }

    static int CountSent(const std::shared_ptr<Shared>& shared, const std::string& needle) {
        int count = 0;
        for (const std::string& text : Sent(shared)) {
            if (text.find(needle) != std::string::npos) {
                ++count;
            }
        }
        return count;
    }

private:
    std::shared_ptr<Shared> shared_;
};

struct Harness {
    std::shared_ptr<FakeTransport::Shared> shared = std::make_shared<FakeTransport::Shared>();
    std::mutex events_mutex;
    std::vector<WecomGatewayEvent> events;
    std::atomic<bool> stop{false};
    std::unique_ptr<WecomGatewaySession> session;
    std::unique_ptr<std::thread> thread;
    std::int64_t ping_interval_ms = 60'000;  // 默认压长:心跳显式案才调小
    int subscribe_timeout_ms = 2'000;

    WecomGatewaySession::Options MakeOptions() {
        WecomGatewaySession::Options options;
        options.transport_factory = [s = shared]() {
            return std::unique_ptr<WecomTransport>(std::make_unique<FakeTransport>(s));
        };
        options.endpoint = "wss://wecom.test/ws";
        options.bot_id = "BOT1";
        options.secret = "SECRET1";
        options.on_event = [this](const WecomGatewayEvent& event) {
            const std::lock_guard<std::mutex> lock(events_mutex);
            events.push_back(event);
        };
        options.now_ms = []() { return platform::WallClockNowMs(); };
        options.ping_interval_ms = ping_interval_ms;
        options.subscribe_timeout_ms = subscribe_timeout_ms;
        options.backoff_scale = 0.001;
        options.drain_poll_ms = 20;
        return options;
    }

    void Start() {
        session = std::make_unique<WecomGatewaySession>(MakeOptions());
        thread = std::make_unique<std::thread>([this]() { session->RunLoop(&stop); });
    }

    ~Harness() {
        stop.store(true);
        if (session != nullptr) {
            session->CancelInFlight();
        }
        if (thread != nullptr && thread->joinable()) {
            thread->join();
        }
    }

    template <typename Pred>
    bool WaitForEvent(Pred pred, int attempts = 400) {
        for (int i = 0; i < attempts; ++i) {
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

    std::vector<WecomGatewayEvent> EventsSince(std::size_t mark) {
        const std::lock_guard<std::mutex> lock(events_mutex);
        if (mark >= events.size()) {
            return {};
        }
        return std::vector<WecomGatewayEvent>(events.begin() + static_cast<std::ptrdiff_t>(mark),
                                              events.end());
    }
};

std::string MsgCallbackFrame(const char* msgid, const char* req_id, const char* content) {
    // json 现建现 dump:手拼原始串在 CI 上翻过车(引号错一枚整串作废)。
    return nlohmann::json{
        {"cmd", "aibot_msg_callback"},
        {"headers", {{"req_id", req_id}}},
        {"body", nlohmann::json{{"msgid", msgid},
                                {"aibotid", "BOT1"},
                                {"chattype", "single"},
                                {"from", {{"userid", "U1"}}},
                                {"msgtype", "text"},
                                {"text", {{"content", content}}}}},
    }.dump();
}

}  // namespace

// ---------------------------------------------------------------------------
// 订阅→在线→回调分发
// ---------------------------------------------------------------------------

TEST_CASE("wecom_gateway: 连上即发 subscribe(一次),errcode=0 → SessionReady/connected") {
    Harness harness;
    harness.Start();

    // 第一帧就是 aibot_subscribe,带 bot_id + secret + 非空 req_id。
    REQUIRE(FakeTransport::WaitForSent(harness.shared, R"("cmd":"aibot_subscribe")"));
    {
        const auto sent = FakeTransport::Sent(harness.shared);
        const auto frame = nlohmann::json::parse(sent.at(0));
        CHECK(frame.at("body").at("bot_id") == "BOT1");
        CHECK(frame.at("body").at("secret") == "SECRET1");
        CHECK_FALSE(frame.at("headers").at("req_id").get<std::string>().empty());
    }
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::SessionReady;
    }));
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::StageChanged && e.stage == kStageConnected;
    }));
    CHECK(harness.session->connection_active());
    // 阶段序:subscribing 在 connected 前。
    const auto events = harness.EventsSince(0);
    auto position = [&events](const char* stage) {
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (events[i].kind == WecomGatewayEvent::Kind::StageChanged &&
                events[i].stage == stage) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    CHECK(position(kStageSubscribing) >= 0);
    CHECK(position(kStageConnected) > position(kStageSubscribing));
    // 勿重发:在线窗内 subscribe 只一发(ping 间隔压长,测试窗内无 ping)。
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(FakeTransport::CountSent(harness.shared, "aibot_subscribe") == 1);
}

TEST_CASE("wecom_gateway: msg_callback 转 MessageCallback(body+req_id 锚)") {
    Harness harness;
    harness.Start();
    // 等到 connected 阶段再记账(SessionReady 与 StageChanged 是两笔事件)。
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::StageChanged && e.stage == kStageConnected;
    }));
    const std::size_t mark = harness.EventCount();
    FakeTransport::PushIncoming(harness.shared, MsgCallbackFrame("MSG1", "REQCB1", "在么"));
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::MessageCallback;
    }));
    const auto events = harness.EventsSince(mark);
    REQUIRE(events.size() == 1);
    CHECK(events[0].body.at("msgid") == "MSG1");
    CHECK(events[0].req_id == "REQCB1");
}

TEST_CASE("wecom_gateway: event_callback 只转交;disconnected_event 让位即止(不退避)") {
    Harness harness;
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"cmd":"aibot_event_callback","headers":{"req_id":"R1"},"body":{"msgid":"E1",)"
        R"("event":{"eventtype":"enter_chat"}}})");
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"cmd":"aibot_event_callback","headers":{"req_id":"R2"},"body":{)"
        R"("event":{"eventtype":")" + std::string(kWecomEventDisconnected) + R"("}}})");
    harness.Start();

    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::EventCallback;
    }));
    // disconnected_event:Disconnected(displaced)+ RunLoop 终止(Stopped),
    // 之后不再 BackoffScheduled——重连会反踢新连,两头乒乓。
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::Disconnected &&
               e.error_code == "displaced_by_new_connection";
    }));
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::Stopped;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool saw_backoff_after_displaced = false;
    const auto events = harness.EventsSince(0);
    bool displaced = false;
    for (const auto& event : events) {
        if (event.kind == WecomGatewayEvent::Kind::Disconnected &&
            event.error_code == "displaced_by_new_connection") {
            displaced = true;
        }
        if (displaced && event.kind == WecomGatewayEvent::Kind::BackoffScheduled) {
            saw_backoff_after_displaced = true;
        }
    }
    CHECK_FALSE(saw_backoff_after_displaced);
    // 只转交了一笔事件回调(enter_chat);disconnected 那笔被网关吃掉。
    int event_callback_count = 0;
    for (const auto& event : events) {
        if (event.kind == WecomGatewayEvent::Kind::EventCallback) {
            ++event_callback_count;
        }
    }
    CHECK(event_callback_count == 1);
}

// ---------------------------------------------------------------------------
// 心跳与死线
// ---------------------------------------------------------------------------

TEST_CASE("wecom_gateway: 心跳按间隔发;回执清账,无人应答判死线重连") {
    Harness harness;
    // 拍长 150ms:死线要 3 拍无入站(≈450ms),CI 满载 runner 的线程停顿
    // (百毫秒级)不至于在应答期误判;60ms 在慢机上翻过车。
    harness.ping_interval_ms = 150;
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::SessionReady;
    }));
    // 应答即清账:逐拍应答前两枚 ping(按计数等新拍,不误认旧帧),
    // 连接持续在线(无 Disconnected)。
    const auto wait_ping_count = [&harness](int want) {
        const auto deadline = platform::WallClockNowMs() + 3'000;
        while (platform::WallClockNowMs() < deadline) {
            if (FakeTransport::CountSent(harness.shared, R"("cmd":"ping")") >= want) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    REQUIRE(wait_ping_count(1));
    FakeTransport::AckLast(harness.shared, 0);
    REQUIRE(wait_ping_count(2));
    FakeTransport::AckLast(harness.shared, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        const std::lock_guard<std::mutex> lock(harness.events_mutex);
        for (const auto& event : harness.events) {
            CHECK(event.kind != WecomGatewayEvent::Kind::Disconnected);
        }
    }
    // 停止应答:连续 missed_ack_limit+1 拍无任何入站 → 死线断线 → 退避重连。
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::Disconnected &&
               e.error_code == "heartbeat_ack_missed";
    }));
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::BackoffScheduled;
    }));
    // 重连各发一次 subscribe(第二连插队回执仍是 0)。
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::SessionReady;
    }));
    CHECK(FakeTransport::CountSent(harness.shared, "aibot_subscribe") >= 2);
}

// ---------------------------------------------------------------------------
// 订阅失败分型
// ---------------------------------------------------------------------------

TEST_CASE("wecom_gateway: 凭据错即止——RunLoop 终止,不退避") {
    Harness harness;
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        harness.shared->subscribe_errcode = 40014;  // 不合法 secret(凭据族)
    }
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::ConnectFailed &&
               e.stage == kStageSubscribing &&
               e.error_code == "subscribe_invalid_credentials";
    }));
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::Stopped;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(FakeTransport::CountSent(harness.shared, "aibot_subscribe") == 1);  // 没有第二轮
}

TEST_CASE("wecom_gateway: 非凭据订阅拒绝走退避重连") {
    Harness harness;
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        harness.shared->subscribe_errcode = 45009;  // Transient(限流族)
    }
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::ConnectFailed &&
               e.error_code == "subscribe_rejected";
    }));
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::BackoffScheduled;
    }));
    // 改回成功码:后续哪一轮重连赶上都行(退避压成毫秒,窗内多轮重试),
    // Connect 插队的回执按当下值生成。
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        harness.shared->subscribe_errcode = 0;
    }
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::SessionReady;
    }));
}

TEST_CASE("wecom_gateway: 订阅窗内静默超时 ConnectFailed(subscribe_timeout)") {
    Harness harness;
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        harness.shared->subscribe_silent = true;
    }
    harness.subscribe_timeout_ms = 200;
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::ConnectFailed &&
               e.error_code == "subscribe_timeout";
    }));
}

// ---------------------------------------------------------------------------
// 杂帧与递交口
// ---------------------------------------------------------------------------

TEST_CASE("wecom_gateway: 认不得的帧记账不断连(UnsupportedFrame)") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared,
                                R"({"cmd":"aibot_send_msg","headers":{"req_id":"R"},"body":{}})");
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::UnsupportedFrame;
    }));
    CHECK(harness.session->unexpected_frame_count() >= 1);
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::SessionReady;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    {
        const std::lock_guard<std::mutex> lock(harness.events_mutex);
        for (const auto& event : harness.events) {
            CHECK(event.kind != WecomGatewayEvent::Kind::Disconnected);
        }
    }
}

TEST_CASE("wecom_gateway: SubmitFrame 走网关写侧,req_id 回执匹配/超时/Stopped") {
    Harness harness;
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::SessionReady;
    }));

    // 1) 递交 respond:帧由网关线程写出(发送线程不碰 socket),平台回执
    //    (透传 req_id)抵达后 SubmitFrame 返回 Acked。
    std::atomic<bool> submit_done{false};
    std::atomic<std::int64_t> ack_errcode{-999};
    std::thread submitter([&]() {
        const auto outcome =
            harness.session->SubmitFrame(BuildRespondFrame("REQCB1", "回话"), "REQCB1", 5'000);
        ack_errcode.store(outcome.status == WecomSubmitOutcome::Status::Acked
                              ? outcome.errcode
                              : -777);
        submit_done.store(true);
    });
    REQUIRE(FakeTransport::WaitForSent(harness.shared, R"("cmd":"aibot_respond_msg")"));
    FakeTransport::AckLast(harness.shared, 0);
    const auto deadline = platform::WallClockNowMs() + 3'000;
    while (!submit_done.load() && platform::WallClockNowMs() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(submit_done.load());
    CHECK(ack_errcode.load() == 0);
    {
        const auto sent = FakeTransport::Sent(harness.shared);
        bool saw_req_passthrough = false;
        for (const std::string& text : sent) {
            if (text.find("aibot_respond_msg") == std::string::npos) {
                continue;
            }
            const auto frame = nlohmann::json::parse(text);
            if (frame.at("headers").at("req_id") == "REQCB1" &&
                frame.at("body").at("markdown").at("content") == "回话") {
                saw_req_passthrough = true;
            }
        }
        CHECK(saw_req_passthrough);
    }
    submitter.join();

    // 2) 无回执:超时如实报(不冒充送达)。
    const auto timeout_outcome =
        harness.session->SubmitFrame(BuildRespondFrame("REQCB2", "无应答"), "REQCB2", 100);
    CHECK(timeout_outcome.status == WecomSubmitOutcome::Status::Timeout);

    // 3) RunLoop 收口后:Stopped(递交口立即返回,不空等)。
    harness.stop.store(true);
    harness.session->CancelInFlight();
    REQUIRE(harness.WaitForEvent([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::Stopped;
    }));
    const auto stopped_outcome =
        harness.session->SubmitFrame(BuildRespondFrame("REQCB3", "收口"), "REQCB3", 1'000);
    CHECK(stopped_outcome.status == WecomSubmitOutcome::Status::Stopped);
    if (harness.thread != nullptr && harness.thread->joinable()) {
        harness.thread->join();
        harness.thread.reset();
    }
}

// ---------------------------------------------------------------------------
// 真socket一案:MockWsServer + 生产传输工厂(回环 ws://)
// ---------------------------------------------------------------------------

TEST_CASE("wecom_gateway: 真WS全链——subscribe/msg_callback/respond/断线(真帧)") {
    test_support::MockWsServer server;
    const auto port = server.Start();
    REQUIRE(port.has_value());

    std::mutex events_mutex;
    std::vector<WecomGatewayEvent> events;
    std::atomic<bool> stop{false};
    WecomGatewaySession::Options options;
    // 生产工厂(明文 ws:// 同一路;回环无 TLS 面)。
    options.transport_factory =
        channel::qq::MakeWsTransportFactory("", transport::TlsTrustMode::ExplicitCa);
    options.endpoint = "ws://127.0.0.1:" + std::to_string(*port) + "/ws";
    options.bot_id = "BOT1";
    options.secret = "SECRET1";
    options.on_event = [&events, &events_mutex](const WecomGatewayEvent& event) {
        const std::lock_guard<std::mutex> lock(events_mutex);
        events.push_back(event);
    };
    options.now_ms = []() { return platform::WallClockNowMs(); };
    options.ping_interval_ms = 60'000;
    options.subscribe_timeout_ms = 5'000;
    options.drain_poll_ms = 20;
    WecomGatewaySession session(std::move(options));
    std::thread runner([&]() { session.RunLoop(&stop); });

    auto wait_for_event = [&](auto pred, int attempts = 600) {
        for (int i = 0; i < attempts; ++i) {
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
    };

    // 1) 服务端收升级,回订阅 ack(透传 req_id)。
    auto connection = server.AcceptNext(5'000);
    REQUIRE(connection.has_value());
    REQUIRE(connection->AcceptUpgrade(5'000).has_value());
    const auto subscribe_text = connection->ReadText(5'000);
    REQUIRE(subscribe_text.has_value());
    const auto subscribe = nlohmann::json::parse(*subscribe_text);
    REQUIRE(subscribe.at("cmd") == "aibot_subscribe");
    REQUIRE(subscribe.at("body").at("bot_id") == "BOT1");
    const std::string subscribe_req_id = subscribe.at("headers").at("req_id");
    REQUIRE(connection
                ->SendText(R"({"headers":{"req_id":")" + subscribe_req_id +
                           R"("},"errcode":0,"errmsg":"ok"})")
                .has_value());
    REQUIRE(wait_for_event([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::SessionReady;
    }));

    // 2) 推一条 msg_callback。
    REQUIRE(connection->SendText(MsgCallbackFrame("MSG-REAL1", "REQCB-REAL1", "真socket来信"))
                .has_value());
    REQUIRE(wait_for_event([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::MessageCallback &&
               e.req_id == "REQCB-REAL1";
    }));

    // 3) 递交 respond:服务端读到帧、回执,SubmitFrame Acked。
    std::atomic<bool> submit_done{false};
    std::atomic<std::int64_t> ack_errcode{-999};
    std::thread submitter([&]() {
        const auto outcome = session.SubmitFrame(
            BuildRespondFrame("REQCB-REAL1", "真socket回话"), "REQCB-REAL1", 5'000);
        ack_errcode.store(outcome.status == WecomSubmitOutcome::Status::Acked
                              ? outcome.errcode
                              : -777);
        submit_done.store(true);
    });
    const auto respond_text = connection->ReadText(5'000);
    REQUIRE(respond_text.has_value());
    const auto respond = nlohmann::json::parse(*respond_text);
    REQUIRE(respond.at("cmd") == "aibot_respond_msg");
    REQUIRE(respond.at("headers").at("req_id") == "REQCB-REAL1");
    REQUIRE(respond.at("body").at("markdown").at("content") == "真socket回话");
    REQUIRE(connection
                ->SendText(R"({"headers":{"req_id":"REQCB-REAL1"},"errcode":0,"errmsg":"ok"})")
                .has_value());
    {
        const auto deadline = platform::WallClockNowMs() + 3'000;
        while (!submit_done.load() && platform::WallClockNowMs() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    REQUIRE(submit_done.load());
    CHECK(ack_errcode.load() == 0);
    submitter.join();

    // 4) 服务端断 TCP → 客户端 Disconnected(重连由服务端侧收口,本案到此)。
    connection->Drop();
    REQUIRE(wait_for_event([](const WecomGatewayEvent& e) {
        return e.kind == WecomGatewayEvent::Kind::Disconnected;
    }));

    stop.store(true);
    session.CancelInFlight();
    runner.join();
}

}  // namespace lubancode::channel::wecombot
