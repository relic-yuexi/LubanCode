// 飞书进程内适配器端到端册(飞书/企微设计单 F1):真 ChannelManager + 真
// FeishuBotAdapter(Transport 字节面),网关传输与 HTTP 全注假——零外联。
// 照 test_qq_adapter 的两条路数:manager 集成路径断状态与账(to_host 字节
// 被 manager 消费,不截流);帧级断言走手工路径(initialize/start 手写,
// 直接 WriteToSidecar/DrainFromSidecar)。
// 演的幕:握手能力 → 事件先落 spool 再 emit → inbound.ack 清 spool →
// channel.send 走 reply(锚 message_id + Bearer + msg_type=text)→ 无锚/
// 纯附件明拒 → spool 落盘失败回 ACK 500 → 连接状态快照 → spool 重启重投。
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
#include "channel/feishu/feishu_adapter.hpp"
#include "channel/feishu/feishu_frame.hpp"
#include "channel/frame.hpp"
#include "channel/manager.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::channel::feishu {
namespace {

// ---------------------------------------------------------------------------
// 假网关传输:连接即过;读按脚本队列吐(帧字节),耗尽静默超时。
// ---------------------------------------------------------------------------

class ScriptGatewayTransport final : public IFeishuGatewayTransport {
public:
    struct Shared {
        std::mutex mutex;
        std::vector<std::string> incoming;  // 客户端将收到的帧字节(按序)
        std::size_t cursor = 0;
        std::vector<std::string> sent;      // 客户端发出的帧字节(ping/ACK)
        transport::WsError exhausted_error{transport::WsError::Kind::Timeout, "script exhausted", 0};
    };

    explicit ScriptGatewayTransport(std::shared_ptr<Shared> shared)
        : shared_(std::move(shared)) {}

    std::expected<void, FeishuConnectError> Connect(const std::string&) override {
        return {};
    }
    std::expected<void, std::string> SendBinary(const std::string& bytes) override {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->sent.push_back(bytes);
        return {};
    }
    std::expected<std::string, transport::WsError> ReadMessage(int) override {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->cursor < shared_->incoming.size()) {
            return shared_->incoming[shared_->cursor++];
        }
        return std::unexpected(shared_->exhausted_error);
    }
    void Cancel() override {}
    void Close(std::uint16_t, const std::string&) override {}

    static void Push(const std::shared_ptr<Shared>& shared, std::string frame_bytes) {
        const std::lock_guard<std::mutex> lock(shared->mutex);
        shared->incoming.push_back(std::move(frame_bytes));
    }

    static std::vector<std::string> Sent(const std::shared_ptr<Shared>& shared) {
        const std::lock_guard<std::mutex> lock(shared->mutex);
        return shared->sent;
    }

private:
    std::shared_ptr<Shared> shared_;
};

// ---------------------------------------------------------------------------
// 假 HTTP:按 URL 分发(引导/令牌/回话三路)。
// ---------------------------------------------------------------------------

struct ScriptHttp {
    mutable std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> calls;  // url/body
    std::vector<std::vector<std::pair<std::string, std::string>>> header_log;
    std::string tenant_token = "t-FAKE1";

    FeishuHttpFunc Func() {
        return [this](const FeishuHttpRequest& request)
                   -> std::expected<FeishuHttpResponse, std::string> {
            const std::lock_guard<std::mutex> lock(mutex);
            calls.emplace_back(request.url, request.body);
            header_log.push_back(request.headers);
            if (request.url.find("/callback/ws/endpoint") != std::string::npos) {
                return FeishuHttpResponse{
                    200, R"({"code":0,"msg":"","data":{"URL":"ws://127.0.0.1:9/ws",)"
                         R"("ClientConfig":{"ReconnectCount":-1,"ReconnectInterval":120,)"
                         R"("ReconnectNonce":30,"PingInterval":120}}})"};
            }
            if (request.url.find("tenant_access_token") != std::string::npos) {
                return FeishuHttpResponse{
                    200, R"({"code":0,"expire":7200,"tenant_access_token":")" +
                             tenant_token + R"("})"};
            }
            if (request.url.find("/reply") != std::string::npos) {
                return FeishuHttpResponse{
                    200, R"({"code":0,"msg":"success","data":{"message_id":"om_replied_1"}})"};
            }
            return FeishuHttpResponse{404, "{}"};
        };
    }

    std::size_t CountUrl(const std::string& needle) {
        const std::lock_guard<std::mutex> lock(mutex);
        std::size_t count = 0;
        for (const auto& [url, body] : calls) {
            if (url.find(needle) != std::string::npos) {
                ++count;
            }
        }
        return count;
    }
};

// 一枚 im.message.receive_v1 事件帧(服务端形状)。
std::string EventFrameBytes(std::uint64_t seq, const char* event_id = "ev_adapter_1",
                            const char* text = "在吗") {
    FeishuFrame frame;
    frame.seq_id = seq;
    frame.log_id = seq;
    frame.service = 25;
    frame.method = kFrameMethodData;
    frame.headers.push_back(FeishuFrameHeader{kHeaderType, kHeaderValueEvent});
    frame.headers.push_back(FeishuFrameHeader{kHeaderMessageId, "om_in_1"});
    frame.payload = std::string(R"({"schema":"2.0","header":{"event_id":")") + event_id +
                              R"(","event_type":"im.message.receive_v1","app_id":"APP1",)"
                              R"("create_time":"1726500000000"},"event":{"message":)"
                              R"({"message_id":"om_in_1","chat_id":"oc_1","chat_type":"p2p",)"
                              R"("message_type":"text","content":"{\"text\":")" + text +
                              R"("}"},"sender":)"
                              R"({"sender_id":{"open_id":"ou_user9"},"sender_type":"user"}}})");
    return EncodeFeishuFrame(frame);
}

struct AdapterHarness {
    std::filesystem::path state_root;
    std::shared_ptr<ScriptGatewayTransport::Shared> gateway =
        std::make_shared<ScriptGatewayTransport::Shared>();
    ScriptHttp http;
    std::unique_ptr<ChannelManager> manager;  // 声明在 adapter 前:析构后于 adapter
    std::unique_ptr<FeishuBotAdapter> adapter;

    explicit AdapterHarness(const char* tag)
        : state_root(std::filesystem::temp_directory_path() /
                     ("lubancode-feishu-adapter-test-" + std::string(tag))) {
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

    FeishuBotAdapter::Options MakeAdapterOptions() {
        FeishuBotAdapter::Options options;
        options.channel_id = "feishu";
        options.account_id = "main";
        options.config = MakeFeishuTemplateAccount();
        options.config.app_id = "APP1";
        options.credential = ResolvedChannelCredential{};
        options.credential.secret = "SECRET1";
        options.credential.source = ResolvedChannelCredential::Source::InlinePlaintext;
        options.state_root = state_root;
        options.http = http.Func();
        options.transport_factory = [g = gateway]() {
            return std::unique_ptr<IFeishuGatewayTransport>(
                std::make_unique<ScriptGatewayTransport>(g));
        };
        options.now_ms = []() { return platform::WallClockNowMs(); };
        return options;
    }

    // 手工路径:initialize + start(bridge-protocol.md §3/§4 的严格形状)。
    void HostInitialize(std::int64_t id = 1) {
        const auto frame = channel::EncodeFrame(channel::BuildRequestJson(
            id, channel::BridgeMethod::Initialize,
            nlohmann::json{{"protocol_version", "2026-08-29"},
                           {"channel_id", "feishu"},
                           {"account_id", "main"},
                           {"state_dir", "/tmp/feishu-adapter-test-state"},
                           {"host", nlohmann::json{{"name", "test-host"}}}}));
        REQUIRE(frame.has_value());
        adapter->WriteToSidecar(frame->data(), frame->size());
    }

    void HostStart(std::int64_t id = 2) {
        const auto frame = channel::EncodeFrame(channel::BuildRequestJson(
            id, channel::BridgeMethod::Start,
            nlohmann::json{{"transport", "websocket"}}));
        REQUIRE(frame.has_value());
        adapter->WriteToSidecar(frame->data(), frame->size());
    }

    void HostWrite(const nlohmann::json& frame) {
        const auto encoded = channel::EncodeFrame(frame);
        REQUIRE(encoded.has_value());
        adapter->WriteToSidecar(encoded->data(), encoded->size());
    }

    // manager 集成路径。
    void AttachManager() {
        ChannelManagerOptions manager_options;
        manager_options.state_root = state_root / "manager";
        manager_options.now_ms = []() { return platform::WallClockNowMs(); };
        manager = std::make_unique<ChannelManager>(std::move(manager_options));
        const auto added = manager->AddAccount("feishu", "main",
                                               MakeFeishuTemplateAccount(), adapter.get());
        REQUIRE(added.status == ChannelManager::AddAccountResult::Status::Ok);
    }

    bool PumpUntil(const std::function<bool()>& done, int timeout_ms = 8'000) {
        const auto deadline = platform::WallClockNowMs() + timeout_ms;
        while (platform::WallClockNowMs() < deadline) {
            (void)manager->Pump("feishu", "main");
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
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

    ChannelAccountState State() {
        const auto snapshot = manager->Snapshot("feishu", "main");
        REQUIRE(snapshot.has_value());
        return snapshot->state;
    }
};

// 测试侧视角:解 adapter 发给宿主的全部帧(手工路径;drain 由测试独占)。
std::vector<nlohmann::json> DecodeDrained(FeishuBotAdapter* adapter) {
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

std::vector<nlohmann::json> WaitFrames(FeishuBotAdapter* adapter,
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

// 在假传输的 sent 账里找一枚 ACK 帧并解出载荷 JSON。
std::optional<nlohmann::json> FindAckPayload(
    const std::shared_ptr<ScriptGatewayTransport::Shared>& shared) {
    for (const std::string& bytes : ScriptGatewayTransport::Sent(shared)) {
        std::string error;
        const auto frame = DecodeFeishuFrame(bytes, &error);
        if (!frame.has_value() || !frame->payload.has_value()) {
            continue;
        }
        if (frame->method != kFrameMethodData) {
            continue;  // ping 控制帧
        }
        const auto payload =
            nlohmann::json::parse(*frame->payload, nullptr, /*allow_exceptions=*/false);
        if (payload.is_object() && payload.contains("code")) {
            return payload;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 帧级路径:握手帧/capabilities/坏版本
// ---------------------------------------------------------------------------

TEST_CASE("feishu_adapter: initialize 握手帧——版本对、能力只宣称首版(不虚报)") {
    AdapterHarness harness("handshake_frames");
    harness.adapter = std::make_unique<FeishuBotAdapter>(harness.MakeAdapterOptions());
    harness.HostInitialize(1);
    const auto frames = DecodeDrained(harness.adapter.get());
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].at("id") == 1);
    CHECK(frames[0].at("result").at("protocol_version") == "2026-08-29");
    const auto& caps = frames[0].at("result").at("capabilities");
    CHECK(caps.at("transports") == nlohmann::json::array({"websocket", "direct"}));
    CHECK(caps.at("delivery") == nlohmann::json::array({"send"}));
    CHECK(caps.at("streaming") == false);
    CHECK(caps.at("interactions") == false);
    CHECK_FALSE(caps.contains("media"));  // 首版无媒体管道,不虚报
    CHECK(frames[0].at("result").at("adapter").at("name") == "feishu-inproc");
}

TEST_CASE("feishu_adapter: 坏 protocol_version 明败(protocol_incompatible)") {
    AdapterHarness harness("bad_version");
    harness.adapter = std::make_unique<FeishuBotAdapter>(harness.MakeAdapterOptions());
    harness.HostInitialize(7);
    // HostInitialize 钉了正确版本;这里补一发坏版本的完整形状帧。
    const auto frame = channel::EncodeFrame(channel::BuildRequestJson(
        8, channel::BridgeMethod::Initialize,
        nlohmann::json{{"protocol_version", "1999-01-01"},
                       {"channel_id", "feishu"},
                       {"account_id", "main"},
                       {"state_dir", "/tmp/x"},
                       {"host", nlohmann::json{{"name", "test-host"}}}}));
    harness.adapter->WriteToSidecar(frame->data(), frame->size());
    const auto frames = WaitFrames(harness.adapter.get(), [](const nlohmann::json& f) {
        return f.value("id", 0) == 8 && f.contains("error");
    });
    REQUIRE_FALSE(frames.empty());
    CHECK(frames[0].at("error").at("message") == "protocol_incompatible");
}

// ---------------------------------------------------------------------------
// 手工路径:事件水路 + spool
// ---------------------------------------------------------------------------

TEST_CASE("feishu_adapter: 事件先落 spool 再 emit;inbound.ack 清理;ACK 帧回 200") {
    AdapterHarness harness("spool_flow");
    harness.adapter = std::make_unique<FeishuBotAdapter>(harness.MakeAdapterOptions());
    harness.HostInitialize();
    harness.HostStart();
    // 网关线程起跑(拨号 + 激活 ping)。
    REQUIRE(harness.WaitQuiet([&] { return harness.adapter->gateway_thread_running(); }));

    ScriptGatewayTransport::Push(harness.gateway, EventFrameBytes(500));
    const auto frames = WaitFrames(harness.adapter.get(), [](const nlohmann::json& frame) {
        return frame.value("method", "") == "channel.inbound";
    });
    REQUIRE_FALSE(frames.empty());
    const nlohmann::json& event = frames[0].at("params");
    CHECK(event.at("channel_id") == "feishu");
    CHECK(event.at("account_id") == "main");
    CHECK(event.at("provider_event_id") == "ev_adapter_1");
    CHECK(event.at("message_id") == "om_in_1");
    CHECK(event.at("conversation").at("kind") == "direct");
    REQUIRE(event.at("parts").size() == 1);
    CHECK(event.at("parts").at(0).at("text") == "在吗");
    REQUIRE(harness.WaitQuiet([&] { return harness.adapter->spool_pending_count() == 1; }));

    // ACK 帧(200)已从假传输发出。
    const auto ack = FindAckPayload(harness.gateway);
    REQUIRE(ack.has_value());
    CHECK(ack->at("code") == 200);

    // 宿主回 inbound.ack → spool 清空。
    harness.HostWrite(channel::BuildRequestJson(
        10, channel::BridgeMethod::InboundAck,
        nlohmann::json{{"delivery_id", event.at("delivery_id")}}));
    REQUIRE(harness.WaitQuiet([&] { return harness.adapter->spool_pending_count() == 0; }));
}

TEST_CASE("feishu_adapter: spool 落盘失败 → Fatal + ACK 500(平台重推路)") {
    AdapterHarness harness("spool_fail");
    harness.adapter = std::make_unique<FeishuBotAdapter>(harness.MakeAdapterOptions());
    harness.HostInitialize();
    harness.HostStart();
    REQUIRE(harness.WaitQuiet([&] { return harness.adapter->gateway_thread_running(); }));
    harness.adapter->SetSpoolAppendFaultForTest(true);

    ScriptGatewayTransport::Push(harness.gateway, EventFrameBytes(501));
    const auto fatal = WaitFrames(harness.adapter.get(), [](const nlohmann::json& frame) {
        return frame.value("method", "") == "channel.fatal";
    });
    REQUIRE_FALSE(fatal.empty());
    CHECK(fatal[0].at("params").at("reason") == "spool_write_failed");
    // ACK 500:没接住,让平台重推(宿主 ingress 去重兜底)。
    REQUIRE(harness.WaitQuiet([&] {
        const auto ack = FindAckPayload(harness.gateway);
        return ack.has_value() && ack->at("code") == 500;
    }));
}

TEST_CASE("feishu_adapter: spool 重启重投——pending 在新实例上重新上报") {
    std::filesystem::path state_root;
    {
        AdapterHarness harness("restart_phase1");
        state_root = harness.state_root;
        harness.adapter = std::make_unique<FeishuBotAdapter>(harness.MakeAdapterOptions());
        harness.HostInitialize();
        harness.HostStart();
        ScriptGatewayTransport::Push(harness.gateway, EventFrameBytes(502, "ev_restart_1"));
        REQUIRE(harness.WaitQuiet([&] { return harness.adapter->spool_pending_count() == 1; }));
        // harness 析构:adapter 析构收线程;spool 落盘保留。
    }
    {
        AdapterHarness second("restart_phase2");
        auto options = second.MakeAdapterOptions();
        options.state_root = state_root;
        second.adapter = std::make_unique<FeishuBotAdapter>(std::move(options));
        second.HostInitialize();
        second.HostStart();
        const auto frames = WaitFrames(second.adapter.get(), [](const nlohmann::json& frame) {
            return frame.value("method", "") == "channel.inbound";
        });
        REQUIRE_FALSE(frames.empty());
        const nlohmann::json& event = frames[0].at("params");
        CHECK(event.at("provider_event_id") == "ev_restart_1");
        CHECK(second.adapter->spool_pending_count() == 1);  // 重投后仍在账(等 ack)
    }
}

// ---------------------------------------------------------------------------
// 手工路径:发送水路
// ---------------------------------------------------------------------------

TEST_CASE("feishu_adapter: channel.send 走 reply(锚进 URL + Bearer + msg_type=text)") {
    AdapterHarness harness("send");
    harness.adapter = std::make_unique<FeishuBotAdapter>(harness.MakeAdapterOptions());
    harness.HostInitialize();
    harness.HostStart();
    REQUIRE(harness.WaitQuiet([&] { return harness.adapter->gateway_thread_running(); }));

    nlohmann::json params;
    params["conversation"] = {{"kind", "direct"}, {"id", "oc_1"}};
    params["parts"] = nlohmann::json::array(
        {nlohmann::json{{"type", "text"}, {"text", "回复正文"}}});
    params["reply_to_message_id"] = "om_in_1";
    params["client_id"] = "out-42";
    harness.HostWrite(channel::BuildRequestJson(3, channel::BridgeMethod::Send, params));

    const auto results = WaitFrames(harness.adapter.get(), [](const nlohmann::json& frame) {
        return frame.value("id", 0) == 3 && frame.contains("result");
    });
    REQUIRE_FALSE(results.empty());
    CHECK(results[0].at("result").at("provider_message_id") == "om_replied_1");
    CHECK(results[0].at("result").at("accepted") == true);

    // HTTP 账:令牌一次 + reply 一次;锚进 URL,Bearer 进头,text 进体。
    REQUIRE(harness.WaitQuiet([&] {
        return harness.http.CountUrl("/open-apis/im/v1/messages/om_in_1/reply") == 1;
    }));
    CHECK(harness.http.CountUrl("tenant_access_token") >= 1);
    {
        const std::lock_guard<std::mutex> lock(harness.http.mutex);
        nlohmann::json reply_body;
        std::vector<std::pair<std::string, std::string>> reply_headers;
        for (std::size_t i = 0; i < harness.http.calls.size(); ++i) {
            if (harness.http.calls[i].first.find("/reply") != std::string::npos) {
                reply_body = nlohmann::json::parse(harness.http.calls[i].second, nullptr,
                                                   false);
                reply_headers = harness.http.header_log[i];
                break;
            }
        }
        REQUIRE(reply_body.is_object());
        CHECK(reply_body.at("msg_type") == "text");
        REQUIRE(reply_body.at("content").is_string());
        CHECK(reply_body.at("content").get<std::string>().find("回复正文") !=
              std::string::npos);
        bool saw_bearer = false;
        for (const auto& [name, value] : reply_headers) {
            if (name == "Authorization" && value == "Bearer t-FAKE1") {
                saw_bearer = true;
            }
        }
        CHECK(saw_bearer);
    }

    // 无锚(主动推送):首版明拒 not_capable,不起 HTTP。
    nlohmann::json proactive = params;
    proactive.erase("reply_to_message_id");
    harness.HostWrite(channel::BuildRequestJson(4, channel::BridgeMethod::Send, proactive));
    const auto rejects = WaitFrames(harness.adapter.get(), [](const nlohmann::json& frame) {
        return frame.value("id", 0) == 4 && frame.contains("error");
    });
    REQUIRE_FALSE(rejects.empty());
    CHECK(rejects[0].at("error").at("message") == "not_capable");

    // 非文本 part:首版只发文本,明拒。
    nlohmann::json file_params = params;
    file_params["parts"] = nlohmann::json::array(
        {nlohmann::json{{"type", "file"}, {"local_path", "C:/x/y.png"}}});
    harness.HostWrite(channel::BuildRequestJson(5, channel::BridgeMethod::Send, file_params));
    const auto file_rejects = WaitFrames(harness.adapter.get(), [](const nlohmann::json& frame) {
        return frame.value("id", 0) == 5 && frame.contains("error");
    });
    REQUIRE_FALSE(file_rejects.empty());
    CHECK(file_rejects[0].at("error").at("message") == "not_capable");
    CHECK(harness.http.CountUrl("/reply") == 1);  // 没多出一笔
}

// ---------------------------------------------------------------------------
// manager 集成路径与连接状态
// ---------------------------------------------------------------------------

TEST_CASE("feishu_adapter: manager 全链——AddAccount/Start 推进到 Running") {
    AdapterHarness harness("manager_flow");
    harness.adapter = std::make_unique<FeishuBotAdapter>(harness.MakeAdapterOptions());
    harness.AttachManager();
    (void)harness.manager->StartAccount("feishu", "main");
    REQUIRE(harness.PumpUntil([&] { return harness.State() == ChannelAccountState::Running; }));
    CHECK(harness.adapter->gateway_thread_running());

    // 事件 → manager ingress 有账 → 自动 durable+ack → spool 清。
    ScriptGatewayTransport::Push(harness.gateway, EventFrameBytes(503, "ev_manager_1"));
    REQUIRE(harness.PumpUntil([&] {
        const auto snapshot = harness.manager->Snapshot("feishu", "main");
        return snapshot.has_value() && !snapshot->ingress_state_counts.empty();
    }));
    CHECK(harness.adapter->spool_pending_count() == 0);
}

TEST_CASE("feishu_adapter: 连接状态快照——idle→connected;stage 用飞书稳定名") {
    AdapterHarness harness("state");
    harness.adapter = std::make_unique<FeishuBotAdapter>(harness.MakeAdapterOptions());
    CHECK_FALSE(harness.adapter->ConnectionState().thread_alive);
    CHECK(harness.adapter->ConnectionState().stage == "idle");
    harness.HostInitialize();
    harness.HostStart();
    REQUIRE(harness.WaitQuiet([&] {
        const auto snapshot = harness.adapter->ConnectionState();
        return snapshot.thread_alive && snapshot.connected;
    }));
    const auto snapshot = harness.adapter->ConnectionState();
    CHECK(snapshot.stage == kStageConnected);
    CHECK(harness.adapter->gateway_state() == "running");
}

}  // namespace
}  // namespace lubancode::channel::feishu
