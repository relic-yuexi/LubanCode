// QQ 进程内适配器端到端册(QQ 机器人接入单 Q1):真 ChannelManager + 真
// QqBotAdapter(Transport 字节面),网关传输与 HTTP 全注假——Q1 验收第二行
// "mock HTTP/事件服务测 C2C 映射、spool 重启重投、ACK 清理、稳定发送载荷;
// 此处不是 QQ 联调"的正主。
//
// 两条路数分开走:manager 集成路径(AddAccount/StartAccount/Pump 全量推进,
// 断言只看状态与账——to_host 字节被 manager 消费,测试不截流);帧级断言走
// 手工路径(直接 WriteToSidecar 宿主帧 + DrainFromSidecar,不经 manager Pump)。
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/bridge_protocol.hpp"
#include "channel/frame.hpp"
#include "channel/manager.hpp"
#include "channel/qq/qq_adapter.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::channel::qq {
namespace {

// ---------------------------------------------------------------------------
// 假网关传输:连接即回 Hello(间隔给大,测试窗内不发心跳),脚本队列推事件。
// ---------------------------------------------------------------------------

class ScriptGatewayTransport final : public IGatewayTransport {
public:
    struct Shared {
        std::mutex mutex;
        std::vector<std::string> incoming;  // 客户端将收到的 payload(按序)
        std::size_t cursor = 0;
        std::vector<std::string> sent;
        bool fail_connect = false;
        // 脚本耗尽后的读错误(默认 Timeout=静默;测试改 Closed 模拟立即断线)。
        WsError exhausted_error{WsError::Kind::Timeout, "script exhausted", 0};
    };

    explicit ScriptGatewayTransport(std::shared_ptr<Shared> shared)
        : shared_(std::move(shared)) {}

    std::expected<void, GatewayConnectError> Connect(const std::string&) override {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->fail_connect) {
            return std::unexpected(GatewayConnectError{
                kStageConnecting, "connect_refused", "connect refused"});
        }
        // 连接即回 Hello——插队头,保证无论测试预置了什么脚本,Hello 总是
        // 客户端连接后读到的第一条(官方语义如此)。字段用官方名
        // heartbeat_interval(A01;event-emit 页示例,2026-09-17 核对)。
        shared_->incoming.insert(
            shared_->incoming.begin(),
            R"({"op":10,"d":{"heartbeat_interval":30000}})");
        return {};
    }

    std::expected<void, std::string> SendText(const std::string& text) override {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->sent.push_back(text);
        return {};
    }

    std::expected<std::string, WsError> ReadMessage(int) override {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->cursor < shared_->incoming.size()) {
            return shared_->incoming[shared_->cursor++];
        }
        return std::unexpected(shared_->exhausted_error);
    }

    void Cancel() override {}
    void Close(std::uint16_t, const std::string&) override {}

    static void Push(const std::shared_ptr<Shared>& shared, std::string payload) {
        const std::lock_guard<std::mutex> lock(shared->mutex);
        shared->incoming.push_back(std::move(payload));
    }

    static std::vector<std::string> Sent(const std::shared_ptr<Shared>& shared) {
        const std::lock_guard<std::mutex> lock(shared->mutex);
        return shared->sent;
    }

private:
    std::shared_ptr<Shared> shared_;
};

// ---------------------------------------------------------------------------
// 假 HTTP:按 URL 分发(token/gateway/send 三路)。
// ---------------------------------------------------------------------------

struct ScriptHttp {
    mutable std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> calls;  // url/body
    // A08 观测:每笔请求的 cancel 指针是否在(适配器的停止章)。
    std::vector<bool> cancel_stamped;
    std::string access_token = "TT1";
    std::string gateway_url = "wss://fake-gw.test/ws";
    // 阻塞开关:置位后 token 请求卡到 cancel 旗被置(停机期限判据),
    // 5s 兜底防挂死。
    bool block_token_until_cancelled = false;

    QqHttpFunc Func() {
        return [this](const QqHttpRequest& request) -> std::expected<QqHttpResponse, std::string> {
            // 停止章观测在入口记(阻塞前)——A08 案里 token 请求卡到取消,
            // 若等返回再记,断言时刻观测账是空的。
            {
                const std::lock_guard<std::mutex> lock(mutex);
                cancel_stamped.push_back(request.cancel != nullptr);
            }
            if (block_token_until_cancelled &&
                request.url.find("/app/getAppAccessToken") != std::string::npos &&
                request.cancel != nullptr) {
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(5'000);
                while (!request.cancel->load() &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            const std::lock_guard<std::mutex> lock(mutex);
            calls.emplace_back(request.url, request.body);
            if (request.url.find("/app/getAppAccessToken") != std::string::npos) {
                return QqHttpResponse{
                    200, R"({"access_token":")" + access_token + R"(","expires_in":7200})"};
            }
            if (request.url.find("/gateway") != std::string::npos) {
                return QqHttpResponse{200, R"({"url":")" + gateway_url + R"("})"};
            }
            if (request.url.find("/messages") != std::string::npos) {
                return QqHttpResponse{200, R"({"id":"ROBOT1.0_sent_1"})"};
            }
            return QqHttpResponse{404, "{}"};
        };
    }
};

struct AdapterHarness {
    std::filesystem::path state_root;
    std::shared_ptr<ScriptGatewayTransport::Shared> gateway =
        std::make_shared<ScriptGatewayTransport::Shared>();
    ScriptHttp http;
    std::unique_ptr<ChannelManager> manager;  // 声明在 adapter 前:析构后于 adapter
    std::unique_ptr<QqBotAdapter> adapter;

    explicit AdapterHarness(const char* tag)
        : state_root(std::filesystem::temp_directory_path() /
                     ("lubancode-qq-adapter-test-" + std::string(tag))) {
        std::error_code ec;
        std::filesystem::remove_all(state_root, ec);
        std::filesystem::create_directories(state_root, ec);
    }

    ~AdapterHarness() {
        if (manager != nullptr) {
            for (const auto& snapshot : manager->Snapshots()) {
                (void)manager->StopAccount(snapshot.channel_id, snapshot.account_id);
            }
            for (int i = 0; i < 150; ++i) {
                bool all_stopped = true;
                for (const auto& snapshot : manager->Snapshots()) {
                    (void)manager->Pump(snapshot.channel_id, snapshot.account_id);
                    if (snapshot.state != ChannelAccountState::Stopped) {
                        all_stopped = false;
                    }
                }
                if (all_stopped) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        manager.reset();
        adapter.reset();
    }

    QqBotAdapter::Options MakeAdapterOptions(const std::string& account = "main") {
        QqBotAdapter::Options options;
        options.channel_id = "qqbot";
        options.account_id = account;
        options.config = MakeQqTemplateAccount();
        options.config.app_id = "APP1";
        options.credential = ResolvedChannelCredential{};
        options.credential.secret = "SECRET1";
        options.credential.source = ResolvedChannelCredential::Source::InlinePlaintext;
        options.state_root = state_root;
        options.http = http.Func();
        options.transport_factory = [g = gateway]() {
            return std::unique_ptr<IGatewayTransport>(
                std::make_unique<ScriptGatewayTransport>(g));
        };
        options.now_ms = []() { return platform::WallClockNowMs(); };
        options.api_base = "https://api.test";
        options.bots_base = "https://bots.test";
        return options;
    }

    // manager 集成路径:Pump 到状态或超时。
    bool PumpUntil(const std::function<bool()>& done, int timeout_ms = 8'000) {
        const auto deadline = platform::WallClockNowMs() + timeout_ms;
        while (platform::WallClockNowMs() < deadline) {
            (void)manager->Pump("qqbot", "main");
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    // 手工路径:直接写宿主帧给 adapter(不经 manager)。
    void HostWrite(const nlohmann::json& frame) {
        const auto encoded = channel::EncodeFrame(frame);
        REQUIRE(encoded.has_value());
        adapter->WriteToSidecar(encoded->data(), encoded->size());
    }

    ChannelAccountState State() {
        const auto snapshot = manager->Snapshot("qqbot", "main");
        REQUIRE(snapshot.has_value());
        return snapshot->state;
    }
};

// 测试侧视角:解 adapter 发给宿主的全部帧。
std::vector<nlohmann::json> DecodeDrained(QqBotAdapter* adapter) {
    std::vector<nlohmann::json> out;
    channel::FrameDecoder decoder;
    const auto bytes = adapter->DrainFromSidecar();
    decoder.Feed(bytes.data(), bytes.size());
    while (true) {
        auto next = decoder.TryDecodeNext();
        if (!next.has_value() || !next->has_value()) {
            break;
        }
        out.push_back(std::move(**next));
    }
    return out;
}

// 等到 drain 出含某谓词的帧(手工路径;drain 由测试独占,一次性快照返回)。
std::vector<nlohmann::json> WaitFrames(QqBotAdapter* adapter,
                                       const std::function<bool(const nlohmann::json&)>& pred,
                                       int timeout_ms = 8'000) {
    const auto deadline = platform::WallClockNowMs() + timeout_ms;
    std::vector<nlohmann::json> all;
    while (platform::WallClockNowMs() < deadline) {
        for (auto& frame : DecodeDrained(adapter)) {
            all.push_back(std::move(frame));
        }
        if (std::any_of(all.begin(), all.end(), pred)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return all;
}

// 手工路径:写 initialize(一次编码一次写)。
nlohmann::json InitializeFrame(std::int64_t id, const std::string& account,
                               const std::string& protocol_version = "2026-08-29");

void HostInitialize(AdapterHarness& harness, const std::string& account = "main",
                    const std::string& protocol_version = "2026-08-29") {
    const auto frame =
        channel::EncodeFrame(InitializeFrame(1, account, protocol_version));
    REQUIRE(frame.has_value());
    harness.adapter->WriteToSidecar(frame->data(), frame->size());
}

// 不带 manager Pump 的等待(手工路径:宿主"没确认"的场景)。
bool WaitQuiet(const std::function<bool()>& done, int timeout_ms = 8'000) {
    const auto deadline = platform::WallClockNowMs() + timeout_ms;
    while (platform::WallClockNowMs() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::string C2cPayload(const char* openid, const char* message_id, const char* content) {
    return std::string(R"({"op":0,"s":2,"t":"C2C_MESSAGE_CREATE","d":{)") +
           R"("id":")" + message_id + R"(",)" + R"("author":{"user_openid":")" + openid +
           R"(","username":"tester","bot":false},)" + R"("content":")" + content +
           R"(","message_type":0,)" +
           R"("message_scene":{"source":"default","ext":["msg_idx=IDX_1"]},)"
           R"("timestamp":"2026-07-21T10:00:00+08:00"}})";
}

nlohmann::json InitializeFrame(std::int64_t id, const std::string& account,
                               const std::string& protocol_version) {
    // bridge-protocol.md §3 的 initialize params 形状(严格表:未知字段即拒):
    // protocol_version/channel_id/account_id/state_dir/host 必填。
    return channel::BuildRequestJson(
        id, channel::BridgeMethod::Initialize,
        nlohmann::json{{"protocol_version", protocol_version},
                       {"channel_id", "qqbot"},
                       {"account_id", account},
                       {"state_dir", "/tmp/qq-adapter-test-state"},
                       {"host", nlohmann::json{{"name", "test-host"}}}});
}

}  // namespace

// ---------------------------------------------------------------------------
// 帧级路径:握手帧/capabilities/坏版本
// ---------------------------------------------------------------------------

TEST_CASE("qq_adapter: initialize 握手帧——版本对、能力只宣称首版(不虚报)") {
    AdapterHarness harness("handshake_frames");
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions("probe"));
    HostInitialize(harness, "probe");
    const auto frames = DecodeDrained(harness.adapter.get());
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].at("id") == 1);
    CHECK(frames[0].at("result").at("protocol_version") == "2026-08-29");
    const auto& caps = frames[0].at("result").at("capabilities");
    CHECK(caps.at("transports") == nlohmann::json::array({"websocket", "direct"}));
    CHECK(caps.at("delivery") == nlohmann::json::array({"send"}));
    CHECK(caps.at("streaming") == false);
    CHECK(frames[0].at("result").at("adapter").at("name") == "qqbot-inproc");
}

TEST_CASE("qq_adapter: 坏 protocol_version 明败(protocol_incompatible)") {
    AdapterHarness harness("bad_version");
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions("probe"));
    const auto frame = channel::EncodeFrame(InitializeFrame(7, "probe", "1999-01-01"));
    harness.adapter->WriteToSidecar(frame->data(), frame->size());
    const auto frames = DecodeDrained(harness.adapter.get());
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].at("id") == 7);
    CHECK(frames[0].at("error").at("message") == "protocol_incompatible");
}

// ---------------------------------------------------------------------------
// manager 集成路径:状态机推进 / C2C 水路 / stop 收尾
// ---------------------------------------------------------------------------

TEST_CASE("qq_adapter: manager 全链——AddAccount/Start 推进到 Running,Identify 真发") {
    AdapterHarness harness("manager_flow");
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions());
    harness.manager = std::make_unique<ChannelManager>([&]() {
        ChannelManagerOptions options;
        options.state_root = harness.state_root;
        options.now_ms = []() { return platform::WallClockNowMs(); };
        return options;
    }());
    REQUIRE(harness.manager->AddAccount("qqbot", "main", MakeQqTemplateAccount(),
                                        harness.adapter.get())
                .status == ChannelManager::AddAccountResult::Status::Ok);
    (void)harness.manager->StartAccount("qqbot", "main");
    REQUIRE(harness.PumpUntil([&harness]() {
        return harness.State() == ChannelAccountState::Running;
    }));
    // 网关线程真起:Identify(op=2)帧在假网关账上,token 带前缀。
    REQUIRE(harness.PumpUntil([&harness]() {
        for (const std::string& text : ScriptGatewayTransport::Sent(harness.gateway)) {
            if (text.find(R"("op":2)") != std::string::npos &&
                text.find("QQBot TT1") != std::string::npos) {
                return true;
            }
        }
        return false;
    }));
    CHECK(harness.adapter->gateway_thread_running());
}

TEST_CASE("qq_adapter: C2C 事件 spool 先落再报;宿主 ACK 后清理(端到端)") {
    AdapterHarness harness("c2c_flow");
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions());
    harness.manager = std::make_unique<ChannelManager>([&]() {
        ChannelManagerOptions options;
        options.state_root = harness.state_root;
        options.now_ms = []() { return platform::WallClockNowMs(); };
        return options;
    }());
    REQUIRE(harness.manager->AddAccount("qqbot", "main", MakeQqTemplateAccount(),
                                        harness.adapter.get())
                .status == ChannelManager::AddAccountResult::Status::Ok);
    (void)harness.manager->StartAccount("qqbot", "main");
    REQUIRE(harness.PumpUntil([&harness]() {
        return harness.State() == ChannelAccountState::Running;
    }));

    ScriptGatewayTransport::Push(
        harness.gateway,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-c2c","user":{"id":"bot-1"}}})");
    ScriptGatewayTransport::Push(harness.gateway,
                                 C2cPayload("OPEN9", "ROBOT1.0_m1", "hello qq"));
    // 宿主 Pump:inbound 进 manager → durable → 自动 ack → adapter 清 spool。
    // (dm_policy=pairing:首条消息 PendingPairing → rejected 入账,但 durable
    // 与 ACK 已发生——这正是要验的水路。)等"ingress 有账"而不是 spool==0
    //(初始即 0,等它等于没等)。
    REQUIRE(harness.PumpUntil([&harness]() {
        const auto snapshot = harness.manager->Snapshot("qqbot", "main");
        return snapshot.has_value() && !snapshot->ingress_state_counts.empty();
    }));
    CHECK(harness.adapter->spool_pending_count() == 0);  // durable+ack 后清理
}

TEST_CASE("qq_adapter: spool 重启重投——pending 在新实例上重新上报") {
    std::filesystem::path state_root;
    {
        AdapterHarness harness("restart_phase1");
        state_root = harness.state_root;
        harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions());
        // 手工路径(不经 manager):宿主没确认就"崩",spool 才会留 pending。
        HostInitialize(harness);
        ScriptGatewayTransport::Push(
            harness.gateway,
            R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-r1","user":{"id":"bot-1"}}})");
        const auto start = channel::EncodeFrame(
            channel::BuildRequestJson(2, channel::BridgeMethod::Start,
                                      nlohmann::json{{"transport", "websocket"}}));
        harness.adapter->WriteToSidecar(start->data(), start->size());
        ScriptGatewayTransport::Push(harness.gateway,
                                     C2cPayload("OPEN9", "ROBOT1.0_m2", "before crash"));
        REQUIRE(WaitQuiet([&harness]() {
            return harness.adapter->spool_pending_count() == 1;
        }));
        // harness 析构:adapter 析构收线程;spool 落盘保留。
    }
    // 第二只实例:同 state_root,新 adapter;start 时 pending 全量重投。
    {
        AdapterHarness second("restart_phase2");
        auto options = second.MakeAdapterOptions();
        options.state_root = state_root;
        second.adapter = std::make_unique<QqBotAdapter>(std::move(options));
        HostInitialize(second);
        const auto start = channel::EncodeFrame(
            channel::BuildRequestJson(2, channel::BridgeMethod::Start,
                                      nlohmann::json{{"transport", "websocket"}}));
        second.adapter->WriteToSidecar(start->data(), start->size());
        bool saw_pending_inbound = false;
        for (const auto& frame :
             WaitFrames(second.adapter.get(), [](const nlohmann::json& frame) {
                 return frame.contains("method") && frame.at("method") == "channel.inbound";
             })) {
            if (frame.contains("method") && frame.at("method") == "channel.inbound") {
                saw_pending_inbound = true;
                const auto& params = frame.at("params");
                CHECK(params.at("message_id") == "ROBOT1.0_m2");
                CHECK(params.at("provider_event_id") == "ROBOT1.0_m2|IDX_1");
                CHECK(params.at("channel_id") == "qqbot");
            }
        }
        CHECK(saw_pending_inbound);
        CHECK(second.adapter->spool_pending_count() == 1);  // 未 ack 仍在册
    }
}

// ---------------------------------------------------------------------------
// 发送链(手工路径:send 帧 → sender 线程 → mock HTTP → result 帧)
// ---------------------------------------------------------------------------

TEST_CASE("qq_adapter: channel.send 全链——载荷稳定、result 回宿主") {
    AdapterHarness harness("send_flow");
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions());
    HostInitialize(harness);
    const auto start = channel::EncodeFrame(
        channel::BuildRequestJson(2, channel::BridgeMethod::Start,
                                  nlohmann::json{{"transport", "websocket"}}));
    harness.adapter->WriteToSidecar(start->data(), start->size());

    // 宿主侧发 channel.send(bridge-protocol.md §4 形状)。
    harness.HostWrite(channel::BuildRequestJson(
        77, channel::BridgeMethod::Send,
        nlohmann::json{
            {"conversation", nlohmann::json{{"kind", "direct"}, {"id", "OPEN9"}}},
            {"parts", nlohmann::json::array({nlohmann::json{{"type", "text"},
                                                            {"text", "answer here"}}})},
            {"reply_to_message_id", "ROBOT1.0_m3"},
            {"client_id", "out-42"}}));
    const auto frames = WaitFrames(
        harness.adapter.get(), [](const nlohmann::json& frame) {
            return frame.contains("id") && frame.at("id") == 77 && frame.contains("result");
        });
    bool saw_result = false;
    for (const auto& frame : frames) {
        if (frame.contains("id") && frame.at("id") == 77 && frame.contains("result")) {
            saw_result = true;
            CHECK(frame.at("result").at("provider_message_id") == "ROBOT1.0_sent_1");
            CHECK(frame.at("result").at("accepted") == true);
        }
    }
    REQUIRE(saw_result);
    // HTTP 面:url/载荷(msg_seq=1;msg_id/content 照官方请求示例)。
    std::string send_body;
    {
        std::lock_guard<std::mutex> lock(harness.http.mutex);
        for (const auto& [url, body] : harness.http.calls) {
            if (url.find("/v2/users/OPEN9/messages") != std::string::npos) {
                send_body = body;
            }
        }
    }
    REQUIRE_FALSE(send_body.empty());
    const auto payload = nlohmann::json::parse(send_body);
    CHECK(payload.at("msg_seq") == 1);
    CHECK(payload.at("msg_id") == "ROBOT1.0_m3");
    CHECK(payload.at("content") == "answer here");
    CHECK(payload.at("msg_type") == 0);
}

TEST_CASE("qq_adapter: stop 停网关线程,未 ACK spool 保留(bridge stop 帧)") {
    AdapterHarness harness("stop_flow");
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions());
    // 手工路径:宿主直接发 initialize/start/stop,不 drain(不触发 ACK)。
    HostInitialize(harness);
    ScriptGatewayTransport::Push(
        harness.gateway,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-s1","user":{"id":"bot-1"}}})");
    const auto start = channel::EncodeFrame(
        channel::BuildRequestJson(2, channel::BridgeMethod::Start,
                                  nlohmann::json{{"transport", "websocket"}}));
    harness.adapter->WriteToSidecar(start->data(), start->size());
    REQUIRE(WaitQuiet([&harness]() { return harness.adapter->gateway_thread_running(); }));

    ScriptGatewayTransport::Push(harness.gateway,
                                 C2cPayload("OPEN9", "ROBOT1.0_m4", "pending one"));
    REQUIRE(WaitQuiet(
        [&harness]() { return harness.adapter->spool_pending_count() == 1; }));
    const auto stop = channel::EncodeFrame(
        channel::BuildRequestJson(3, channel::BridgeMethod::Stop, nlohmann::json{}));
    harness.adapter->WriteToSidecar(stop->data(), stop->size());
    REQUIRE(WaitQuiet([&harness]() { return !harness.adapter->gateway_thread_running(); }));
    CHECK(harness.adapter->spool_pending_count() == 1);  // 未 ACK 不丢
    // stop 的 result 帧在 drain 里(bridge-protocol.md §4 形状)。
    bool saw_stopped = false;
    for (const auto& frame : DecodeDrained(harness.adapter.get())) {
        if (frame.contains("id") && frame.at("id") == 3 && frame.contains("result")) {
            saw_stopped = true;
            CHECK(frame.at("result").at("stopped") == true);
        }
    }
    CHECK(saw_stopped);
}

// ---------------------------------------------------------------------------
// 连接状态快照(连接状态单 §三):connected 只认 READY/RESUMED;失败保留
// 根因、退避不覆盖;成功后错误归档清当前;Health 响应带真实 connected。
// ---------------------------------------------------------------------------

TEST_CASE("qq_adapter: ConnectionState——connected 只在 READY 后成立,断线立即 false") {
    AdapterHarness harness("conn_state");
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions());
    HostInitialize(harness);
    // 未 start:线程不活,connected=false,stage=idle。
    {
        const ConnectionSnapshot before = harness.adapter->ConnectionState();
        CHECK_FALSE(before.thread_alive);
        CHECK_FALSE(before.connected);
        CHECK(before.stage == "idle");
    }
    const auto start = channel::EncodeFrame(
        channel::BuildRequestJson(2, channel::BridgeMethod::Start,
                                  nlohmann::json{{"transport", "websocket"}}));
    harness.adapter->WriteToSidecar(start->data(), start->size());
    REQUIRE(WaitQuiet([&harness]() { return harness.adapter->gateway_thread_running(); }));
    // 已起线程但未 READY:thread_alive=true 而 connected 仍 false(§三
    // "不用线程存活冒充在线")。
    REQUIRE(WaitQuiet([&harness]() {
        const ConnectionSnapshot mid = harness.adapter->ConnectionState();
        return mid.thread_alive && !mid.connected;
    }));

    // READY → connected=true。
    ScriptGatewayTransport::Push(
        harness.gateway,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-cs","user":{"id":"b"}}})");
    REQUIRE(WaitQuiet([&harness]() { return harness.adapter->ConnectionState().connected; }));
    {
        const ConnectionSnapshot live = harness.adapter->ConnectionState();
        CHECK(live.stage == kStageConnected);
        CHECK(live.connected_since_ms > 0);
        CHECK_FALSE(live.last_failure.has_value());
    }

    // 断线(脚本耗尽改 Closed:运行循环 read 立即断):connected 立即
    // false,last_failure 保留根因与阶段(§三 Disconnected 不丢 detail)。
    {
        const std::lock_guard<std::mutex> lock(harness.gateway->mutex);
        harness.gateway->exhausted_error =
            WsError{WsError::Kind::Closed, "peer closed", 1000};
    }
    REQUIRE(WaitQuiet([&harness]() {
        const ConnectionSnapshot dead = harness.adapter->ConnectionState();
        return !dead.connected && dead.last_failure.has_value();
    }));
    {
        const ConnectionSnapshot dead = harness.adapter->ConnectionState();
        CHECK(dead.last_failure->stage == kStageConnected);
        CHECK_FALSE(dead.last_failure->error_code.empty());
    }
}

TEST_CASE("qq_adapter: 连接失败记根因;成功后错误移入历史、当前清空") {
    AdapterHarness harness("conn_failure");
    // token 接口直接拒:provider 阶段 ConnectFailed(fetching_token)。
    {
        std::lock_guard<std::mutex> lock(harness.http.mutex);
        harness.http.access_token.clear();  // 响应缺 access_token → InvalidCredentials
    }
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions());
    HostInitialize(harness);
    const auto start = channel::EncodeFrame(
        channel::BuildRequestJson(2, channel::BridgeMethod::Start,
                                  nlohmann::json{{"transport", "websocket"}}));
    harness.adapter->WriteToSidecar(start->data(), start->size());
    REQUIRE(WaitQuiet([&harness]() {
        const ConnectionSnapshot failing = harness.adapter->ConnectionState();
        return failing.last_failure.has_value() &&
               failing.last_failure->stage == kStageFetchingToken && failing.retry_count >= 1;
    }));
    {
        const ConnectionSnapshot failing = harness.adapter->ConnectionState();
        CHECK(failing.last_failure->error_code == "token_invalid_credentials");
        CHECK_FALSE(failing.connected);
        CHECK(failing.retry_count >= 1);          // BackoffScheduled 记了账
        CHECK(failing.next_retry_at_ms > 0);
    }
    // 修好 token:下一轮连接成功 → 错误移入历史、当前清空、重试账清零。
    {
        std::lock_guard<std::mutex> lock(harness.http.mutex);
        harness.http.access_token = "TT2";
    }
    ScriptGatewayTransport::Push(
        harness.gateway,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-ok","user":{"id":"b"}}})");
    REQUIRE(WaitQuiet([&harness]() { return harness.adapter->ConnectionState().connected; }));
    {
        const ConnectionSnapshot healed = harness.adapter->ConnectionState();
        CHECK_FALSE(healed.last_failure.has_value());
        REQUIRE_FALSE(healed.failure_history.empty());
        CHECK(healed.failure_history.back().error_code == "token_invalid_credentials");
        CHECK(healed.retry_count == 0);
        CHECK(healed.next_retry_at_ms == 0);
    }
}

TEST_CASE("qq_adapter: Health 响应带真实 connected 与 last_error(修三处现状病)") {
    AdapterHarness harness("health_fix");
    {
        std::lock_guard<std::mutex> lock(harness.http.mutex);
        harness.http.access_token.clear();
    }
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions());
    HostInitialize(harness);
    const auto start = channel::EncodeFrame(
        channel::BuildRequestJson(2, channel::BridgeMethod::Start,
                                  nlohmann::json{{"transport", "websocket"}}));
    harness.adapter->WriteToSidecar(start->data(), start->size());
    // 先等失败账立起来(token 失败已入账、阶段停在取令牌),再发 Health——
    // 否则网关线程首事件未发,快照阶段还是默认填充值。
    REQUIRE(WaitQuiet([&harness]() {
        const ConnectionSnapshot failing = harness.adapter->ConnectionState();
        return failing.last_failure.has_value() &&
               failing.last_failure->error_code == "token_invalid_credentials";
    }));
    // 线程活着但没连上:Health 的 connected 必须 false(不再用线程存活冒充)。
    harness.HostWrite(channel::BuildRequestJson(9, channel::BridgeMethod::Health,
                                                nlohmann::json{}));
    bool saw_health = false;
    for (const auto& frame :
         WaitFrames(harness.adapter.get(), [](const nlohmann::json& frame) {
             return frame.contains("id") && frame.at("id") == 9 && frame.contains("result");
         })) {
        if (frame.contains("id") && frame.at("id") == 9 && frame.contains("result")) {
            saw_health = true;
            const auto& result = frame.at("result");
            CHECK(result.at("connected") == false);
            CHECK(result.at("thread_alive") == true);
            CHECK(result.at("last_error").is_object());
            CHECK(result.at("last_error").at("error_code") == "token_invalid_credentials");
            CHECK(result.at("stage") == kStageFetchingToken);
        }
    }
    REQUIRE(saw_health);
}

// ---------------------------------------------------------------------------
// A04/A05/A08 第二波:落盘游标回执、msg_seq 穿桥、停止章、未建模事件账
// ---------------------------------------------------------------------------

TEST_CASE("qq_adapter: A04 spool 落盘失败——PersistFailed 断线留根因,Resume 补发不丢信") {
    AdapterHarness harness("spool_fault");
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions());
    HostInitialize(harness);
    const auto start = channel::EncodeFrame(
        channel::BuildRequestJson(2, channel::BridgeMethod::Start,
                                  nlohmann::json{{"transport", "websocket"}}));
    harness.adapter->WriteToSidecar(start->data(), start->size());
    REQUIRE(WaitQuiet([&harness]() { return harness.adapter->gateway_thread_running(); }));

    // 磁盘满注入:READY 后的来信落不了盘。
    harness.adapter->SetSpoolAppendFaultForTest(true);
    ScriptGatewayTransport::Push(
        harness.gateway,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-sf","user":{"id":"bot-1"}}})");
    ScriptGatewayTransport::Push(
        harness.gateway, C2cPayload("OPENF", "ROBOT1.0_mf1", "must not be lost"));
    // Fatal 留痕(spool_write_failed)——宿主 Degraded 可见。
    REQUIRE_FALSE(WaitFrames(harness.adapter.get(), [](const nlohmann::json& frame) {
                       return frame.value("method", "") == "channel.fatal" &&
                              frame.value("params", nlohmann::json::object())
                                  .value("reason", "") == "spool_write_failed";
                   })
                      .empty());
    // 连接按可恢复故障断线:根因 = event_persist_failed(durable 游标没
    // 推进,不跨过失败事件)。
    REQUIRE(WaitQuiet([&harness]() {
        const ConnectionSnapshot snapshot = harness.adapter->ConnectionState();
        return snapshot.last_failure.has_value() &&
               snapshot.last_failure->error_code == "event_persist_failed";
    }));

    // 磁盘恢复:第二轮 Resume 补发同一事件——Inbound 恰一次,不丢信。
    // (假传输的游标不随"连接插队 Hello"移位,第二轮要自己补推一条 Hello。)
    harness.adapter->SetSpoolAppendFaultForTest(false);
    ScriptGatewayTransport::Push(
        harness.gateway, R"({"op":10,"d":{"heartbeat_interval_ms":30000}})");
    ScriptGatewayTransport::Push(harness.gateway,
                                 R"({"op":0,"s":2,"t":"RESUMED","d":{}})");
    ScriptGatewayTransport::Push(
        harness.gateway, C2cPayload("OPENF", "ROBOT1.0_mf1", "must not be lost"));
    const auto frames = WaitFrames(harness.adapter.get(), [](const nlohmann::json& frame) {
        return frame.value("method", "") == "channel.inbound" &&
               frame.value("params", nlohmann::json::object())
                   .value("provider_event_id", "")
                   .find("ROBOT1.0_mf1") != std::string::npos;
    });
    int inbound_count = 0;
    for (const auto& frame : frames) {
        if (frame.value("method", "") == "channel.inbound" &&
            frame.value("params", nlohmann::json::object())
                    .value("provider_event_id", "")
                    .find("ROBOT1.0_mf1") != std::string::npos) {
            ++inbound_count;
        }
    }
    CHECK(inbound_count == 1);  // 失败轮没报,补发轮恰报一次
}

TEST_CASE("qq_adapter: A05 msg_seq 穿桥直达——宿主冻结号原样进平台载荷") {
    AdapterHarness harness("msg_seq_bridge");
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions());
    HostInitialize(harness);
    const auto start = channel::EncodeFrame(
        channel::BuildRequestJson(2, channel::BridgeMethod::Start,
                                  nlohmann::json{{"transport", "websocket"}}));
    harness.adapter->WriteToSidecar(start->data(), start->size());
    harness.HostWrite(channel::BuildRequestJson(
        81, channel::BridgeMethod::Send,
        nlohmann::json{
            {"conversation", nlohmann::json{{"kind", "direct"}, {"id", "OPENSEQ"}}},
            {"parts", nlohmann::json::array({nlohmann::json{{"type", "text"},
                                                            {"text", "frozen seq"}}})},
            {"reply_to_message_id", "ROBOT1.0_m7"},
            {"client_id", "out-seq7"},
            {"msg_seq", 7}}));
    REQUIRE_FALSE(WaitFrames(harness.adapter.get(), [](const nlohmann::json& frame) {
                      return frame.contains("id") && frame.at("id") == 81 &&
                             frame.contains("result");
                  })
                      .empty());
    std::string send_body;
    {
        std::lock_guard<std::mutex> lock(harness.http.mutex);
        for (const auto& [url, body] : harness.http.calls) {
            if (url.find("/v2/users/OPENSEQ/messages") != std::string::npos) {
                send_body = body;
            }
        }
    }
    REQUIRE_FALSE(send_body.empty());
    const auto payload = nlohmann::json::parse(send_body);
    CHECK(payload.at("msg_seq") == 7);  // 宿主冻结号,适配器不再重选号
    CHECK(payload.at("msg_id") == "ROBOT1.0_m7");
}

TEST_CASE("qq_adapter: A04 未建模 Dispatch 有明确终结记录——Health 投影计数") {
    AdapterHarness harness("unsupported_events");
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions());
    HostInitialize(harness);
    const auto start = channel::EncodeFrame(
        channel::BuildRequestJson(2, channel::BridgeMethod::Start,
                                  nlohmann::json{{"transport", "websocket"}}));
    harness.adapter->WriteToSidecar(start->data(), start->size());
    REQUIRE(WaitQuiet([&harness]() { return harness.adapter->gateway_thread_running(); }));
    ScriptGatewayTransport::Push(
        harness.gateway,
        R"({"op":0,"s":1,"t":"READY","d":{"session_id":"sess-ue","user":{"id":"bot-1"}}})");
    ScriptGatewayTransport::Push(harness.gateway,
                                 R"({"op":0,"s":2,"t":"FRIEND_ADD","d":{"openid":"O9"}})");
    REQUIRE(WaitQuiet(
        [&harness]() { return harness.adapter->unsupported_dispatch_count() == 1; }));
    harness.HostWrite(
        channel::BuildRequestJson(10, channel::BridgeMethod::Health, nlohmann::json{}));
    bool saw_health = false;
    for (const auto& frame :
         WaitFrames(harness.adapter.get(), [](const nlohmann::json& frame) {
             return frame.contains("id") && frame.at("id") == 10 && frame.contains("result");
         })) {
        if (frame.contains("id") && frame.at("id") == 10 && frame.contains("result")) {
            saw_health = true;
            CHECK(frame.at("result").at("unsupported_events") == 1);
        }
    }
    REQUIRE(saw_health);
}

TEST_CASE("qq_adapter: A08 停止章——出站 HTTP 全带 cancel;停机期限内收口") {
    AdapterHarness harness("stop_cancel");
    harness.http.block_token_until_cancelled = true;  // 先置位再起线程
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeAdapterOptions());
    HostInitialize(harness);
    const auto start = channel::EncodeFrame(
        channel::BuildRequestJson(2, channel::BridgeMethod::Start,
                                  nlohmann::json{{"transport", "websocket"}}));
    harness.adapter->WriteToSidecar(start->data(), start->size());
    REQUIRE(WaitQuiet([&harness]() { return harness.adapter->gateway_thread_running(); }));
    // 已发过的 token 请求盖了停止章(cancel 指针非空;gateway url 请求
    // 还卡在 token 后面,不在此刻的账上)。
    {
        std::lock_guard<std::mutex> lock(harness.http.mutex);
        REQUIRE(harness.http.cancel_stamped.size() >= 1);
        for (const bool stamped : harness.http.cancel_stamped) {
            CHECK(stamped);
        }
    }
    // 阻塞中的 token 请求:Stop 后停止旗点亮 → HTTP 掐流 → 线程有界收口
    //(StopGatewayLocked 在宿主线程 join,凭的就是取消章而不是干等)。
    const std::int64_t stop_at = platform::WallClockNowMs();
    const auto stop = channel::EncodeFrame(
        channel::BuildRequestJson(3, channel::BridgeMethod::Stop, nlohmann::json{}));
    harness.adapter->WriteToSidecar(stop->data(), stop->size());
    REQUIRE(WaitQuiet([&harness]() { return !harness.adapter->gateway_thread_running(); }));
    const std::int64_t stopped_at = platform::WallClockNowMs();
    // 总停机期限(文档口径 5s;HTTP 硬墙 30s——不靠硬墙,靠取消章)。
    CHECK(stopped_at - stop_at < 5'000);
}

}  // namespace lubancode::channel::qq
