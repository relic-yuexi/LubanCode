// QQ 网关状态机册(QQ 机器人接入单 Q1,§十五 gateway-events 模块)。
// IGatewayTransport 注入假流(零网络),状态迁移显式断言:
//   Hello→Identify→READY→Dispatch(seq 记账)→Heartbeat/ACK→断线退避;
//   Resume(session_id+seq)/Invalid Session 两条重建路;Reconnect 主动重连;
//   心跳饥饿(missed_ack_limit)判死线。退避 scale 压成毫秒,CI 秒级收场。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/qq/qq_gateway.hpp"
#include "channel/qq/qq_proto.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::channel::qq {
namespace {

// 假传输:连接脚本化。ReadMessage 按队列吐;耗尽后恒回 override 错误
// (默认 Timeout——模拟"连接静默",让心跳路径可走)。
class FakeTransport final : public IGatewayTransport {
public:
    struct Shared {
        std::mutex mutex;
        std::vector<std::string> sent;              // 客户端发出(Identify/心跳…)
        std::vector<std::string> connect_urls;
        std::vector<std::string> incoming;          // 待吐脚本(测试可动态追加)
        std::size_t incoming_cursor = 0;
        WsError exhausted_error{WsError::Kind::Timeout, "script exhausted", 0};
        bool fail_connect = false;
    };

    explicit FakeTransport(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}

    std::expected<void, GatewayConnectError> Connect(const std::string& url) override {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->connect_urls.push_back(url);
        if (shared_->fail_connect) {
            return std::unexpected(GatewayConnectError{
                kStageConnecting, "connect_refused", "connect refused"});
        }
        return {};
    }

    std::expected<void, std::string> SendText(const std::string& text) override {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->sent.push_back(text);
        return {};
    }

    std::expected<std::string, WsError> ReadMessage(int timeout_ms) override {
        (void)timeout_ms;
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->incoming_cursor < shared_->incoming.size()) {
            return shared_->incoming[shared_->incoming_cursor++];
        }
        return std::unexpected(shared_->exhausted_error);
    }

    void Cancel() override {}
    void Close(std::uint16_t, const std::string&) override {}

    // 测试侧工具:等客户端发出第 n 条(index)包含 substr 的文本。
    static bool WaitForSent(const std::shared_ptr<Shared>& shared, const std::string& needle,
                            int attempts = 200) {
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

private:
    std::shared_ptr<Shared> shared_;
};

struct Harness {
    std::shared_ptr<FakeTransport::Shared> shared =
        std::make_shared<FakeTransport::Shared>();
    std::mutex events_mutex;
    std::vector<GatewayEvent> events;
    std::atomic<bool> stop{false};
    std::unique_ptr<QqGatewaySession> session;
    std::unique_ptr<std::thread> thread;
    // url provider 可换(测 provider 侧失败:取令牌/查地址阶段)。测试线程
    // 在网关线程跑动中换,上锁防竞态。
    std::mutex url_mutex;
    std::function<std::expected<std::string, GatewayConnectError>()> url_provider_impl =
        []() -> std::expected<std::string, GatewayConnectError> {
        return std::string("wss://gateway.test/ws");
    };
    // A02:鉴权循环总期限;默认生产值,超时案调小换 CI 速度。
    int ready_timeout_ms = 10'000;

    void SetUrlProvider(
        std::function<std::expected<std::string, GatewayConnectError>()> provider) {
        const std::lock_guard<std::mutex> lock(url_mutex);
        url_provider_impl = std::move(provider);
    }

    QqGatewaySession::Options MakeOptions() {
        QqGatewaySession::Options options;
        options.transport_factory = [s = shared]() {
            return std::unique_ptr<IGatewayTransport>(std::make_unique<FakeTransport>(s));
        };
        options.gateway_url_provider = [this]() {
            const std::lock_guard<std::mutex> lock(url_mutex);
            return url_provider_impl();
        };
        options.token_provider = []() -> std::expected<std::string, GatewayConnectError> {
            return std::string("TOKEN");
        };
        options.on_event = [this](const GatewayEvent& event) {
            const std::lock_guard<std::mutex> lock(events_mutex);
            events.push_back(event);
        };
        options.now_ms = []() { return platform::WallClockNowMs(); };
        options.backoff_scale = 0.001;  // 秒级阶梯压成毫秒
        options.missed_ack_limit = 2;
        options.ready_timeout_ms = ready_timeout_ms;
        return options;
    }

    void Start() {
        session = std::make_unique<QqGatewaySession>(MakeOptions());
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

    std::vector<GatewayEvent> SnapshotEvents() {
        const std::lock_guard<std::mutex> lock(events_mutex);
        return events;
    }
};

// HELLO fixture:官方字段 heartbeat_interval(A01;来源:
// bot.q.qq.com event-emit 页,核对 2026-09-17)。测试按需取不同间隔值,
// 报文形状与官方示例一致——不经产品构造器合成。
const char* HelloPayload(int interval_ms) {
    static std::string buffer;
    buffer = R"({"op":10,"d":{"heartbeat_interval":)" + std::to_string(interval_ms) +
             "}}";
    return buffer.c_str();
}

// C2C 补发事件(官方 c2c_message_create 事件形状的最小例;补发流/早到
// 派发共用)。
std::string C2cDispatch(int seq, const char* message_id) {
    return std::string(R"({"op":0,"s":)") + std::to_string(seq) +
           R"(,"t":"C2C_MESSAGE_CREATE","d":{"id":")" + message_id +
           R"(","author":{"user_openid":"OPEN1"},"content":"replay",)"
           R"("message_type":0}})";
}

}  // namespace

TEST_CASE("qq_gateway: Hello→Identify→READY→C2C Dispatch 全链;seq 记账") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-1","user":{"id":"bot-1"}}})");
    FakeTransport::PushIncoming(harness.shared,
                                R"({"op":0,"s":2,"t":"C2C_MESSAGE_CREATE","d":{"id":"M1",)"
                                R"("author":{"user_openid":"OPEN1"},"content":"hi",)"
                                R"("message_type":0,"timestamp":"2026-07-21T10:00:00+08:00"}})");
    harness.Start();

    // Identify:op=2,token 带 QQBot 前缀,intents = 单聊位|互动位(Q6 起
    // 默认订阅两枚)。
    REQUIRE(FakeTransport::WaitForSent(harness.shared, R"("op":2)"));
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        REQUIRE_FALSE(harness.shared->sent.empty());
        const auto identify = nlohmann::json::parse(harness.shared->sent[0]);
        CHECK(identify.at("d").at("token") == "QQBot TOKEN");
        CHECK(identify.at("d").at("intents") == ((1u << 25) | (1u << 26)));
    }
    // READY 事件 + C2C 事件。
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::SessionReady;
    }));
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::C2cMessageCreate;
    }));
    CHECK(harness.session->session_id() == "sess-1");
    CHECK(harness.session->last_seq() == 2);
    // 脚本耗尽后静默(Timeout)→ 状态 running;心跳间隔 30s,测试窗内不发。
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(harness.session->state_name() == "running");
}

// A01:来源独立的官方 HELLO fixture(逐字节抄官方 event-emit 页示例,核对
// 2026-09-17)必须把网关推入 Identify——mock 不许靠产品构造器自证。
TEST_CASE("qq_gateway: 官方 HELLO fixture(原文)驱动 Identify(A01)") {
    Harness harness;
    // 官方示例原文:{"op":10,"d":{"heartbeat_interval":45000}}
    FakeTransport::PushIncoming(harness.shared, R"({"op":10,"d":{"heartbeat_interval":45000}})");
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-fx","user":{"id":"bot-fx"}}})");
    harness.Start();
    REQUIRE(FakeTransport::WaitForSent(harness.shared, R"("op":2)"));
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        const auto identify = nlohmann::json::parse(harness.shared->sent[0]);
        CHECK(identify.at("op") == 2);
        CHECK(identify.at("d").at("token") == "QQBot TOKEN");
    }
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::SessionReady;
    }));
}

// A01:缺 heartbeat_interval(只带旧误读字段名以外的坏形状)按协议错误断线,
// 错误明细指向官方字段;不带默认间隔硬跑。
TEST_CASE("qq_gateway: HELLO 坏载荷断线,明细指向官方字段(A01)") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, R"({"op":10,"d":{}})");
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::ConnectFailed && e.stage == kStageConnecting &&
               e.error_code == "hello_bad_payload" &&
               e.detail.find("heartbeat_interval") != std::string::npos;
    }));
    // 零/负数/超帽同样拒绝(ParseHelloInterval 矩阵在 test_qq_proto 钉)。
    Harness zero_harness;
    FakeTransport::PushIncoming(zero_harness.shared, R"({"op":10,"d":{"heartbeat_interval":0}})");
    zero_harness.Start();
    REQUIRE(zero_harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::ConnectFailed &&
               e.error_code == "hello_bad_payload";
    }));
}

// ---------------------------------------------------------------------------
// A02:鉴权循环——Resume 收补发、等 RESUMED 才算恢复完成。
// ---------------------------------------------------------------------------

TEST_CASE("qq_gateway: Resume 补发流——业务事件先行派发,RESUMED 后才 connected(A02)") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":5,"t":"READY","d":{"session_id":"sess-rp","user":{"id":"b"}}})");
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        harness.shared->exhausted_error = WsError{WsError::Kind::Closed, "peer closed", 1000};
    }
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::Disconnected;
    }));
    // 第二轮:HELLO→Resume→两条补发业务消息(尚无 RESUMED)。
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(harness.shared, C2cDispatch(6, "M-R1"));
    FakeTransport::PushIncoming(harness.shared, C2cDispatch(7, "M-R2"));
    REQUIRE(FakeTransport::WaitForSent(harness.shared, R"("op":6)"));
    // 补发事件逐条派发(A04 的落盘在适配器消费侧,这里验事件交出)。
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::C2cMessageCreate;
    }));
    // 关键断言:收到补发不等于恢复完成——此刻不得报 connected/SessionResumed。
    CHECK(harness.session->state_name() == "resuming");
    {
        const std::vector<GatewayEvent> events = harness.SnapshotEvents();
        for (const auto& event : events) {
            CHECK_FALSE(event.kind == GatewayEvent::Kind::SessionResumed);
            CHECK_FALSE(event.kind == GatewayEvent::Kind::StageChanged &&
                        event.stage == kStageConnected);
        }
    }
    // RESUMED 到达:恢复完成,connected。
    FakeTransport::PushIncoming(harness.shared, R"({"op":0,"s":8,"t":"RESUMED","d":{}})");
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::SessionResumed;
    }));
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::StageChanged && e.stage == kStageConnected;
    }));
    CHECK(harness.session->last_seq() == 8);
}

TEST_CASE("qq_gateway: 只有补发没有 RESUMED——总期限超时,不误报上线(A02)") {
    Harness harness;
    harness.ready_timeout_ms = 400;  // CI 速度
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":3,"t":"READY","d":{"session_id":"sess-nr","user":{"id":"b"}}})");
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        harness.shared->exhausted_error = WsError{WsError::Kind::Closed, "peer closed", 1000};
    }
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::Disconnected;
    }));
    // 第二轮:HELLO→Resume→补发一条,然后静默。脚本耗尽错误换回 Timeout
    //(第一轮用 Closed 断线):鉴权窗静默必须折成总期限超时,不是读断线。
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        harness.shared->exhausted_error = WsError{WsError::Kind::Timeout, "script exhausted", 0};
    }
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(harness.shared, C2cDispatch(4, "M-NR"));
    REQUIRE(FakeTransport::WaitForSent(harness.shared, R"("op":6)"));
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::C2cMessageCreate;
    }));
    // 总期限到:ConnectFailed(ready_timeout),绝无 SessionResumed/connected。
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::ConnectFailed &&
               e.error_code == "ready_timeout";
    }));
    const std::vector<GatewayEvent> events = harness.SnapshotEvents();
    for (const auto& event : events) {
        CHECK_FALSE(event.kind == GatewayEvent::Kind::SessionResumed);
        CHECK_FALSE(event.kind == GatewayEvent::Kind::StageChanged &&
                    event.stage == kStageConnected);
    }
}

// Identify 同款:READY 前来的业务事件照常派发,但鉴权仍只认有效 READY。
TEST_CASE("qq_gateway: Identify 窗内早到业务事件派发,READY 仍是唯一完成条件(A02)") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(harness.shared, C2cDispatch(1, "M-EARLY"));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":2,"t":"READY","d":{"session_id":"sess-early","user":{"id":"b"}}})");
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::C2cMessageCreate;
    }));
    // 早到事件不冒充完成:SessionReady 只跟 READY 走。
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::SessionReady;
    }));
    CHECK(harness.session->last_seq() == 2);
}

// ---------------------------------------------------------------------------
// A09:服务端心跳(官方 opcode 表 op1 双向)与未知 op。
// ---------------------------------------------------------------------------

TEST_CASE("qq_gateway: 服务端心跳在鉴权窗被立即应答(A09)") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(harness.shared, R"({"op":1,"d":null})");  // 服务端心跳
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-hb","user":{"id":"b"}}})");
    harness.Start();
    // 服务端 op1 → 客户端立即回 op1(此时未收过事件,d=null)。
    REQUIRE(FakeTransport::WaitForSent(harness.shared, R"("op":1)"));
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::SessionReady;
    }));
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        bool replied_null_d = false;
        for (const std::string& text : harness.shared->sent) {
            if (text.find(R"("op":1)") != std::string::npos &&
                text.find(R"("d":null)") != std::string::npos) {
                replied_null_d = true;
            }
        }
        CHECK(replied_null_d);
    }
    CHECK(harness.session->unexpected_op_count() == 0);  // op1 双向,不是异常
}

TEST_CASE("qq_gateway: 在线时服务端心跳应答并入 ACK 账,连接不断(A09)") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-hb2","user":{"id":"b"}}})");
    FakeTransport::PushIncoming(harness.shared, R"({"op":1,"d":1})");  // 服务端心跳
    FakeTransport::PushIncoming(harness.shared, R"({"op":11,"d":null})");  // ACK
    FakeTransport::PushIncoming(harness.shared, C2cDispatch(2, "M-AFTER"));
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::C2cMessageCreate;
    }));
    // 应答的心跳携带最新 s=1;ACK 清了计数;业务照常收。
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        bool beat_with_seq = false;
        for (const std::string& text : harness.shared->sent) {
            if (text.find(R"("op":1)") != std::string::npos &&
                text.find(R"("d":1)") != std::string::npos) {
                beat_with_seq = true;
            }
        }
        CHECK(beat_with_seq);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(harness.session->state_name() == "running");  // 无误判断线
    CHECK(harness.session->unexpected_op_count() == 0);
}

TEST_CASE("qq_gateway: 未知 op 独立处置——记账不崩,不冒充心跳(A09)") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-unk","user":{"id":"b"}}})");
    // op12(HTTP 回调 ACK,官方表存在但 WS 模式不该来)与裸陌生 op99。
    FakeTransport::PushIncoming(harness.shared, R"({"op":12,"d":null})");
    FakeTransport::PushIncoming(harness.shared, R"({"op":99,"d":{}})");
    FakeTransport::PushIncoming(harness.shared, C2cDispatch(2, "M-UNK"));
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::C2cMessageCreate;
    }));
    // 旧实现 value_or(Heartbeat) 把陌生 op 当心跳放行且无账;现在:计数、
    // 不触发心跳应答(间隔 30s,测试窗内不应发出任何 op1)。
    CHECK(harness.session->unexpected_op_count() == 2);
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        for (const std::string& text : harness.shared->sent) {
            CHECK(text.find(R"("op":1)") == std::string::npos);
        }
    }
    CHECK(harness.session->state_name() == "running");  // 不崩不断
}

TEST_CASE("qq_gateway: 连续 ACK 维持在线;停喂后按限断线(A09)") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(40));  // 40ms 心跳
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-ack"}})");
    harness.Start();
    // 心跳计数门:WaitForSent 是全量扫描,这里按"第 k 次心跳"等。
    const auto count_beats = [&harness]() {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        int count = 0;
        for (const std::string& text : harness.shared->sent) {
            if (text.find(R"("op":1)") != std::string::npos) {
                ++count;
            }
        }
        return count;
    };
    const auto wait_beats = [&](int target) {
        for (int i = 0; i < 400; ++i) {
            if (count_beats() >= target) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    // 每次心跳后喂 ACK:连续 ACK 不断线。
    for (int round = 1; round <= 3; ++round) {
        REQUIRE(wait_beats(round));
        FakeTransport::PushIncoming(harness.shared, R"({"op":11,"d":null})");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(harness.session->state_name() == "running");
    // 停喂 ACK:连续超限断线(死线账没有被误清零)。
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::Disconnected &&
               e.detail.find("heartbeat") != std::string::npos;
    }));
}

TEST_CASE("qq_gateway: 心跳饥饿——连续无 ACK 超限判死线断开") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(40));  // 40ms 心跳
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-1"}})");
    harness.Start();
    // 脚本耗尽恒 Timeout:不发 ACK。missed_ack_limit=2 → 第三次心跳前断线。
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::Disconnected &&
               e.detail.find("heartbeat") != std::string::npos;
    }));
    // 心跳至少发出过 limit 次,携带最新 s。
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        int heartbeats = 0;
        for (const std::string& text : harness.shared->sent) {
            if (text.find(R"("op":1)") != std::string::npos) {
                ++heartbeats;
                CHECK(text.find("\"d\":1") != std::string::npos);  // 携带 seq
            }
        }
        CHECK(heartbeats >= 2);
    }
}

TEST_CASE("qq_gateway: 断线退避后 Resume 携带 session_id+seq;RESUMED 事件") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":5,"t":"READY","d":{"session_id":"sess-r","user":{"id":"b"}}})");
    // 然后直接断线(exhausted_error 默认 Timeout 改成 Closed)。
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        harness.shared->exhausted_error = WsError{WsError::Kind::Closed, "peer closed", 1000};
    }
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::Disconnected;
    }));
    // 退避后第二轮连接:客户端应发 Resume(op=6, session_id, seq=5)。
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(harness.shared, R"({"op":0,"s":6,"t":"RESUMED","d":{}})");
    REQUIRE(FakeTransport::WaitForSent(harness.shared, R"("op":6)"));
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        // 找 Resume 帧(在 sent 里,op=6)。
        bool found = false;
        for (const std::string& text : harness.shared->sent) {
            if (text.find(R"("op":6)") != std::string::npos) {
                const auto resume = nlohmann::json::parse(text);
                CHECK(resume.at("d").at("session_id") == "sess-r");
                CHECK(resume.at("d").at("seq") == 5);
                found = true;
            }
        }
        CHECK(found);
    }
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::SessionResumed;
    }));
}

// A09:Invalid Session 三分——d=true 会话保留(下轮仍 Resume);d 缺失按
// 不可恢复处置(清 session 重新 Identify)。与 WS close code 的区分:close
// 走 read_closed 稳定码(现有断线案已覆盖),不经 invalid_session。
TEST_CASE("qq_gateway: Invalid Session d=true 保留会话再 Resume;d 缺失清会话(A09)") {
    // d=true:会话仍可信,断线重连后继续 Resume。
    {
        Harness harness;
        FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
        FakeTransport::PushIncoming(
            harness.shared,
            R"({"op":0,"s":2,"t":"READY","d":{"session_id":"sess-i9"}})");
        {
            const std::lock_guard<std::mutex> lock(harness.shared->mutex);
            harness.shared->exhausted_error =
                WsError{WsError::Kind::Closed, "peer closed", 1000};
        }
        harness.Start();
        REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
            return e.kind == GatewayEvent::Kind::Disconnected;
        }));
        // 第二轮:op9 d=true → 断线但不清 session;第三轮仍发 Resume。
        FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
        FakeTransport::PushIncoming(harness.shared, R"({"op":9,"d":true})");
        REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
            return e.kind == GatewayEvent::Kind::ConnectFailed &&
                   e.error_code == "invalid_session";
        }));
        {
            const std::lock_guard<std::mutex> lock(harness.shared->mutex);
            harness.shared->sent.clear();
        }
        FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
        FakeTransport::PushIncoming(harness.shared, R"({"op":0,"s":3,"t":"RESUMED","d":{}})");
        REQUIRE(FakeTransport::WaitForSent(harness.shared, R"("op":6)"));
        REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
            return e.kind == GatewayEvent::Kind::SessionResumed;
        }));
        // 未发 SessionInvalidated。
        const std::vector<GatewayEvent> events = harness.SnapshotEvents();
        for (const auto& event : events) {
            CHECK_FALSE(event.kind == GatewayEvent::Kind::SessionInvalidated);
        }
    }
    // d 缺失:官方合同里不存在,按不可恢复处置——清 session,下轮 Identify。
    {
        Harness harness;
        FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
        FakeTransport::PushIncoming(
            harness.shared,
            R"({"op":0,"s":2,"t":"READY","d":{"session_id":"sess-i9b"}})");
        {
            const std::lock_guard<std::mutex> lock(harness.shared->mutex);
            harness.shared->exhausted_error =
                WsError{WsError::Kind::Closed, "peer closed", 1000};
        }
        harness.Start();
        REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
            return e.kind == GatewayEvent::Kind::Disconnected;
        }));
        FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
        FakeTransport::PushIncoming(harness.shared, R"({"op":9})");
        REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
            return e.kind == GatewayEvent::Kind::SessionInvalidated;
        }));
        CHECK(harness.session->session_id().empty());
    }
}

TEST_CASE("qq_gateway: INTERACTION_CREATE 分发为互动事件,载荷原样带出") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-1","user":{"id":"bot-1"}}})");
    // 官方互动事件页单聊例(type=11 消息按钮)。
    FakeTransport::PushIncoming(harness.shared,
                                R"({"op":0,"s":2,"t":"INTERACTION_CREATE","d":)"
                                R"({"application_id":"1904842048","chat_type":2,)"
                                R"("data":{"resolved":{"button_data":"qai:tok:1"},)"
                                R"("type":11},"id":"inter-1","scene":"c2c",)"
                                R"("timestamp":"2026-07-20T21:53:54+08:00","type":11,)"
                                R"("user_openid":"OPENID1","version":1}})");
    harness.Start();

    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::InteractionCreate;
    }));
    const auto events = harness.SnapshotEvents();
    bool seen = false;
    for (const auto& event : events) {
        if (event.kind != GatewayEvent::Kind::InteractionCreate) {
            continue;
        }
        seen = true;
        CHECK(event.interaction_d.at("id").get<std::string>() == "inter-1");
        CHECK(event.interaction_d.at("type") == 11);
        CHECK(event.interaction_d.at("data").at("resolved").at("button_data") == "qai:tok:1");
    }
    CHECK(seen);
    CHECK(harness.session->last_seq() == 2);
}

TEST_CASE("qq_gateway: Invalid Session 不可恢复——清 session,下轮重新 Identify") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-i"}})");
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        harness.shared->exhausted_error =
            WsError{WsError::Kind::Closed, "peer closed", 1000};
    }
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::Disconnected;
    }));
    // 第二轮:Hello 后服务端回 op9 d=false(不可恢复)。
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(harness.shared, R"({"op":9,"d":false})");
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::SessionInvalidated;
    }));
    CHECK(harness.session->session_id().empty());
    // 第三轮:重新 Identify(op=2)而非 Resume。
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-new"}})");
    {
        // 第三轮的 Identify 是该轮 sent 的首条;记录断线时刻的 sent 数。
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        harness.shared->sent.clear();
    }
    REQUIRE(FakeTransport::WaitForSent(harness.shared, R"("op":2)"));
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::SessionReady;
    }));
}

TEST_CASE("qq_gateway: 服务端 Reconnect(op=7)主动断开并保留 session 走 Resume") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":3,"t":"READY","d":{"session_id":"sess-rc"}})");
    FakeTransport::PushIncoming(harness.shared, R"({"op":7,"d":{}})");
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::Disconnected &&
               e.detail.find("reconnect") != std::string::npos;
    }));
    CHECK(harness.session->session_id() == "sess-rc");  // 保 session
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(harness.shared, R"({"op":0,"s":4,"t":"RESUMED","d":{}})");
    REQUIRE(FakeTransport::WaitForSent(harness.shared, R"("op":6)"));
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::SessionResumed;
    }));
}

TEST_CASE("qq_gateway: 连接失败沿退避阶梯重试,connect 次数递增") {
    Harness harness;
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        harness.shared->fail_connect = true;
    }
    harness.Start();
    // 从未到 READY 的失败发 ConnectFailed(不是 Disconnected),带阶段与稳定码。
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::ConnectFailed && e.stage == kStageConnecting &&
               e.error_code == "connect_refused";
    }));
    // 失败后是退避排程事件(不带根因,不覆盖)。
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::BackoffScheduled && e.attempt >= 1;
    }));
    // scale 0.001:秒级阶梯变毫秒级,几百 ms 内应累计多次连接尝试。
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::size_t connects = 0;
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        connects = harness.shared->connect_urls.size();
    }
    CHECK(connects >= 3);
    CHECK(harness.session->connect_attempts() >= 1);
}

TEST_CASE("qq_gateway: 首条消息不是 Hello 即断线") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, R"({"op":0,"t":"READY","d":{}})");
    harness.Start();
    // hello 阶段失败(未到 READY)→ ConnectFailed + 阶段码。
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::ConnectFailed && e.stage == kStageConnecting &&
               e.error_code == "hello_bad_payload" &&
               e.detail.find("HELLO") != std::string::npos;
    }));
}

// ---------------------------------------------------------------------------
// 连接状态单 §三:阶段事件、失败/退避分家、在线后断线带根因、停止事件。
// ---------------------------------------------------------------------------

TEST_CASE("qq_gateway: 阶段推进事件——connecting→identifying→connected") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-st","user":{"id":"b"}}})");
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::SessionReady;
    }));
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::StageChanged && e.stage == kStageConnected;
    }));
    // 事件序:connecting 在 identifying 前,identifying 在 connected 前。
    const std::vector<GatewayEvent> events = harness.SnapshotEvents();
    auto position = [&events](const char* stage) {
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (events[i].kind == GatewayEvent::Kind::StageChanged &&
                events[i].stage == stage) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    const int connecting = position(kStageConnecting);
    const int identifying = position(kStageIdentifying);
    const int connected = position(kStageConnected);
    CHECK(connecting >= 0);
    CHECK(identifying > connecting);
    CHECK(connected > identifying);
}

TEST_CASE("qq_gateway: READY 前失败发 ConnectFailed;在线后断线发 Disconnected 带根因") {
    Harness harness;
    // provider 返回失败:阶段 fetching_token、稳定码 token_invalid_credentials。
    harness.SetUrlProvider(
        []() -> std::expected<std::string, GatewayConnectError> {
            return std::unexpected(GatewayConnectError{
                kStageFetchingToken, "token_invalid_credentials",
                "token: invalid credentials"});
        });
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::ConnectFailed &&
               e.stage == kStageFetchingToken &&
               e.error_code == "token_invalid_credentials";
    }));
    // 退避事件跟在失败后,不带根因。
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::BackoffScheduled;
    }));

    // 换回正常 provider:在线后断线(运行期 read 失败)→ Disconnected
    // 带根因与阶段。
    harness.SetUrlProvider(
        []() -> std::expected<std::string, GatewayConnectError> {
            return std::string("wss://gateway.test/ws");
        });
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-x"}})");
    {
        const std::lock_guard<std::mutex> lock(harness.shared->mutex);
        harness.shared->exhausted_error =
            WsError{WsError::Kind::Closed, "peer closed", 1000};
    }
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::Disconnected && e.stage == kStageConnected &&
               e.error_code == "read_closed" &&
               e.detail.find("peer closed") != std::string::npos;
    }));
}

TEST_CASE("qq_gateway: 停止时发 Stopped 事件(RunLoop 收口)") {
    Harness harness;
    FakeTransport::PushIncoming(harness.shared, HelloPayload(30'000));
    FakeTransport::PushIncoming(
        harness.shared,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-s"}})");
    harness.Start();
    REQUIRE(harness.WaitForEvent([](const GatewayEvent& e) {
        return e.kind == GatewayEvent::Kind::StageChanged && e.stage == kStageConnected;
    }));
    harness.stop.store(true);
    harness.session->CancelInFlight();
    REQUIRE(harness.WaitForEvent(
        [](const GatewayEvent& e) { return e.kind == GatewayEvent::Kind::Stopped; }));
    CHECK(harness.session->state_name() == "stopped");
}

}  // namespace lubancode::channel::qq
