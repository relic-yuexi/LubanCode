// 企微进程内适配器端到端册(W1):真 WecombotAdapter(Transport 字节面),
// 网关传输注假(自动应答订阅/回执)。钉:握手能力、入站先落 spool
// 再上报、ACK 清理、重启重投、req_id 锚回话(markdown 透传)、分段、
// 无锚明拒、限流账、Health 投影。零外联。
//
// 两条路数照 QQ 适配器册:manager 集成路径(状态推进)+ 帧级手工路径
//(直接 WriteToSidecar/DrainFromSidecar)。断言一律按谓词计数/检索帧,
// 不拿 drain 的总条数立断言——握手回包与 Status 通知随时会混在管道里。
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
#include "channel/wecombot/wecom_adapter.hpp"
#include "channel/wecombot/wecom_gateway.hpp"
#include "channel/wecombot/wecom_proto.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::channel::wecombot {
namespace {

// 假网关传输:Connect 插订阅回执;SendText 自动应答 respond(req_id
// 透传,平台口径);脚本队列推入站;耗尽后静默(Timeout)。
class ScriptTransport final : public WecomTransport {
public:
    struct Shared {
        std::mutex mutex;
        std::vector<std::string> incoming;
        std::size_t cursor = 0;
        std::vector<std::string> sent;
        bool fail_connect = false;
        std::int64_t subscribe_errcode = 0;
        std::int64_t respond_errcode = 0;  // 0=成功;测试改值测分型
        transport::WsError exhausted_error{transport::WsError::Kind::Timeout, "script exhausted", 0};
    };

    explicit ScriptTransport(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}

    std::expected<void, channel::qq::GatewayConnectError> Connect(const std::string&) override {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        if (shared_->fail_connect) {
            return std::unexpected(channel::qq::GatewayConnectError{
                kStageConnecting, "connect_refused", "connect refused"});
        }
        nlohmann::json ack = nlohmann::json::object();
        ack["headers"] = nlohmann::json::object();
        ack["errcode"] = shared_->subscribe_errcode;
        ack["errmsg"] = shared_->subscribe_errcode == 0 ? "ok" : "err";
        shared_->incoming.insert(shared_->incoming.begin() +
                                     static_cast<std::ptrdiff_t>(
                                         std::min(shared_->cursor, shared_->incoming.size())),
                                 ack.dump());
        return {};
    }

    std::expected<void, std::string> SendText(const std::string& text) override {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->sent.push_back(text);
        const auto frame = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (frame.is_discarded() || !frame.contains("cmd") || !frame.contains("headers")) {
            return {};
        }
        const std::string cmd = frame.at("cmd").get<std::string>();
        const auto req_id = frame.at("headers").at("req_id");
        if (cmd != "aibot_respond_msg") {
            return {};  // ping 应答可省:静默窗由 missed_ack_limit 兜
        }
        nlohmann::json ack = nlohmann::json::object();
        ack["headers"] = nlohmann::json{{"req_id", req_id}};
        ack["errcode"] = shared_->respond_errcode;
        ack["errmsg"] = shared_->respond_errcode == 0 ? "ok" : "err";
        shared_->incoming.push_back(ack.dump());
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

    static void Push(const std::shared_ptr<Shared>& shared, std::string payload) {
        const std::lock_guard<std::mutex> lock(shared->mutex);
        shared->incoming.push_back(std::move(payload));
    }

    static std::vector<std::string> Sent(const std::shared_ptr<Shared>& shared) {
        const std::lock_guard<std::mutex> lock(shared->mutex);
        return shared->sent;
    }

    static std::vector<std::string> RespondFrames(const std::shared_ptr<Shared>& shared) {
        std::vector<std::string> out;
        for (const std::string& text : Sent(shared)) {
            if (text.find("aibot_respond_msg") != std::string::npos) {
                out.push_back(text);
            }
        }
        return out;
    }

private:
    std::shared_ptr<Shared> shared_;
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

struct AdapterHarness {
    std::filesystem::path state_root;
    std::shared_ptr<ScriptTransport::Shared> gateway =
        std::make_shared<ScriptTransport::Shared>();

    explicit AdapterHarness(const char* tag)
        : state_root(std::filesystem::temp_directory_path() /
                     ("lubancode-wecom-adapter-test-" + std::string(tag))) {
        std::error_code ec;
        std::filesystem::remove_all(state_root, ec);
        std::filesystem::create_directories(state_root, ec);
    }

    WecombotAdapter::Options MakeAdapterOptions(const std::string& account = "main") {
        WecombotAdapter::Options options;
        options.channel_id = "wecombot";
        options.account_id = account;
        options.config = MakeWecombotTemplateAccount();
        options.config.app_id = "BOT1";
        options.credential = ResolvedChannelCredential{};
        options.credential.secret = "SECRET1";
        options.credential.source = ResolvedChannelCredential::Source::InlinePlaintext;
        options.state_root = state_root;
        options.transport_factory = [g = gateway]() {
            return std::unique_ptr<WecomTransport>(std::make_unique<ScriptTransport>(g));
        };
        options.now_ms = []() { return platform::WallClockNowMs(); };
        options.ping_interval_ms = 60'000;
        options.drain_poll_ms = 20;
        options.respond_ack_timeout_ms = 2'000;
        options.sender_backoff_base_ms = 10;
        return options;
    }
};

// 测试侧视角:解 adapter 发给宿主的全部帧。
std::vector<nlohmann::json> DecodeDrained(WecombotAdapter* adapter) {
    std::vector<nlohmann::json> out;
    FrameDecoder decoder;
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

std::vector<nlohmann::json> WaitFrames(WecombotAdapter* adapter,
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

// 断言用:按谓词计数/检索(管道里混着握手回包与 Status 通知,总数不作数)。
std::size_t CountFrames(const std::vector<nlohmann::json>& frames,
                        const std::function<bool(const nlohmann::json&)>& pred) {
    return static_cast<std::size_t>(
        std::count_if(frames.begin(), frames.end(), pred));
}

std::vector<nlohmann::json> FilterFrames(const std::vector<nlohmann::json>& frames,
                                         const std::function<bool(const nlohmann::json&)>& pred) {
    std::vector<nlohmann::json> out;
    for (const auto& frame : frames) {
        if (pred(frame)) {
            out.push_back(frame);
        }
    }
    return out;
}

void HostWrite(WecombotAdapter* adapter, const nlohmann::json& frame) {
    const auto encoded = EncodeFrame(frame);
    REQUIRE(encoded.has_value());
    adapter->WriteToSidecar(encoded->data(), encoded->size());
}

nlohmann::json InitializeFrame(std::int64_t id, const std::string& account,
                               const std::string& protocol_version = "2026-08-29") {
    return BuildRequestJson(
        id, BridgeMethod::Initialize,
        nlohmann::json{{"protocol_version", protocol_version},
                       {"channel_id", "wecombot"},
                       {"account_id", account},
                       {"state_dir", "/tmp/wecom-adapter-test-state"},
                       {"host", nlohmann::json{{"name", "test-host"}}}});
}

nlohmann::json StartFrame(std::int64_t id) {
    // params 形状照 manager 实发:只有 transport(未知字段会被形状表拒)。
    return BuildRequestJson(
        id, BridgeMethod::Start, nlohmann::json{{"transport", "websocket"}});
}

nlohmann::json SendFrame(std::int64_t id, const std::string& conversation_id,
                         const std::string& text, const std::string& reply_to = std::string()) {
    nlohmann::json params = nlohmann::json{
        {"conversation", nlohmann::json{{"kind", "direct"}, {"id", conversation_id}}},
        {"parts", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", text}}})}};
    if (!reply_to.empty()) {
        params["reply_to_message_id"] = reply_to;
    }
    return BuildRequestJson(id, BridgeMethod::Send, std::move(params));
}

// 手工路径的标准三步:initialize + start,各自等回包排干管道。
void HostInitializeAndStart(WecombotAdapter* adapter) {
    HostWrite(adapter, InitializeFrame(1, "main"));
    REQUIRE_FALSE(FilterFrames(WaitFrames(adapter, [](const nlohmann::json& frame) {
                                   return frame.contains("id") && frame.at("id") == 1 &&
                                          frame.contains("result");
                               }),
                                [](const nlohmann::json&) { return true; })
                      .empty());
    HostWrite(adapter, StartFrame(2));
    REQUIRE_FALSE(FilterFrames(WaitFrames(adapter, [](const nlohmann::json& frame) {
                                   return frame.contains("id") && frame.at("id") == 2 &&
                                          frame.contains("result");
                               }),
                                [](const nlohmann::json&) { return true; })
                      .empty());
}

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

const std::function<bool(const nlohmann::json&)> kIsInbound = [](const nlohmann::json& frame) {
    return frame.value("method", "") == "channel.inbound";
};

}  // namespace

// ---------------------------------------------------------------------------
// 握手与能力
// ---------------------------------------------------------------------------

TEST_CASE("wecom_adapter: initialize 握手——版本对,能力只宣称首版(不虚报)") {
    AdapterHarness harness("handshake");
    auto adapter = std::make_unique<WecombotAdapter>(harness.MakeAdapterOptions("probe"));
    HostWrite(adapter.get(), InitializeFrame(1, "probe"));
    const auto frames = DecodeDrained(adapter.get());
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].at("id") == 1);
    CHECK(frames[0].at("result").at("protocol_version") == "2026-08-29");
    CHECK(frames[0].at("result").at("adapter").at("name") == "wecombot-inproc");
    const auto& capabilities = frames[0].at("result").at("capabilities");
    CHECK(capabilities.at("streaming") == false);
    CHECK(capabilities.at("interactions") == false);
    CHECK(capabilities.at("media").at("inbound").empty());
    CHECK(capabilities.at("media").at("outbound").empty());

    // 坏版本:明败不重试。
    HostWrite(adapter.get(), InitializeFrame(2, "probe", "1999-01-01"));
    const auto rejected = DecodeDrained(adapter.get());
    REQUIRE(rejected.size() == 1);
    CHECK(rejected[0].at("error").at("message") == "protocol_incompatible");
}

// ---------------------------------------------------------------------------
// 入站:spool 先落再上报;ACK 清理;事件回调只记账
// ---------------------------------------------------------------------------

TEST_CASE("wecom_adapter: start 起线程;入站先落 spool 再上报;ACK 清理") {
    AdapterHarness harness("inbound");
    auto adapter = std::make_unique<WecombotAdapter>(harness.MakeAdapterOptions());
    HostInitializeAndStart(adapter.get());
    CHECK(WaitQuiet([&]() { return adapter->gateway_thread_running(); }));

    ScriptTransport::Push(harness.gateway, MsgCallbackFrame("MSG1", "REQCB1", "在么"));
    const auto frames = WaitFrames(adapter.get(), kIsInbound);
    REQUIRE(CountFrames(frames, kIsInbound) == 1);
    const auto inbound = FilterFrames(frames, kIsInbound).at(0);
    const auto& params = inbound.at("params");
    CHECK(params.at("provider_event_id") == "MSG1");
    CHECK(params.at("conversation").at("id") == "U1");
    CHECK(params.at("parts").at(0).at("text") == "在么");
    CHECK(WaitQuiet([&]() { return adapter->spool_pending_count() == 1; }));

    // ACK:按上报帧的 delivery_id 清账。
    HostWrite(adapter.get(), BuildRequestJson(3, BridgeMethod::InboundAck,
                                              nlohmann::json{{"delivery_id",
                                                              params.at("delivery_id")}}));
    const auto acked = WaitFrames(adapter.get(), [](const nlohmann::json& frame) {
        return frame.contains("id") && frame.at("id") == 3 && frame.contains("result");
    });
    REQUIRE(CountFrames(acked, [](const nlohmann::json& frame) {
                return frame.at("result").at("acked") == true;
            }) == 1);
    CHECK(WaitQuiet([&]() { return adapter->spool_pending_count() == 0; }));
}

TEST_CASE("wecom_adapter: 事件回调只记账不入模型;Health 投影两本账") {
    AdapterHarness harness("events");
    auto adapter = std::make_unique<WecombotAdapter>(harness.MakeAdapterOptions());
    HostInitializeAndStart(adapter.get());
    CHECK(WaitQuiet([&]() { return adapter->gateway_thread_running(); }));
    ScriptTransport::Push(
        harness.gateway,
        nlohmann::json{{"cmd", "aibot_event_callback"},
                       {"headers", {{"req_id", "R1"}}},
                       {"body", {{"msgid", "E1"},
                                 {"event", {{"eventtype", "enter_chat"}}}}}}
            .dump());
    CHECK(WaitQuiet([&]() { return adapter->ignored_event_count() == 1; }));
    // 无 channel.inbound 帧(事件不进模型)。
    const auto frames = DecodeDrained(adapter.get());
    CHECK(CountFrames(frames, kIsInbound) == 0);
    // 认不得的 cmd → unsupported 账。
    ScriptTransport::Push(
        harness.gateway,
        nlohmann::json{{"cmd", "aibot_send_msg"},
                       {"headers", {{"req_id", "R2"}}},
                       {"body", nlohmann::json::object()}}
            .dump());
    CHECK(WaitQuiet([&]() { return adapter->unsupported_frame_count() == 1; }));

    HostWrite(adapter.get(),
              BuildRequestJson(9, BridgeMethod::Health, nlohmann::json::object()));
    const auto health = WaitFrames(adapter.get(), [](const nlohmann::json& frame) {
        return frame.contains("id") && frame.at("id") == 9 && frame.contains("result");
    });
    const auto health_results = FilterFrames(health, [](const nlohmann::json& frame) {
        return frame.contains("result");
    });
    REQUIRE(health_results.size() == 1);
    CHECK(health_results[0].at("result").at("ignored_events") == 1);
    CHECK(health_results[0].at("result").at("unsupported_events") == 1);
    CHECK(health_results[0].at("result").at("connected") == true);
    CHECK(health_results[0].at("result").at("thread_alive") == true);
}

TEST_CASE("wecom_adapter: 解不开的回调 Fatal 留痕(明确终结,不进 spool)") {
    AdapterHarness harness("invalid");
    auto adapter = std::make_unique<WecombotAdapter>(harness.MakeAdapterOptions());
    HostInitializeAndStart(adapter.get());
    CHECK(WaitQuiet([&]() { return adapter->gateway_thread_running(); }));
    // 缺 msgid:映射拒绝。
    ScriptTransport::Push(
        harness.gateway,
        nlohmann::json{{"cmd", "aibot_msg_callback"},
                       {"headers", {{"req_id", "R1"}}},
                       {"body", nlohmann::json{{"chattype", "single"},
                                               {"from", {{"userid", "U1"}}},
                                               {"msgtype", "text"},
                                               {"text", {{"content", "x"}}}}}}
            .dump());
    const auto is_fatal = [](const nlohmann::json& frame) {
        return frame.value("method", "") == "channel.fatal" &&
               frame.at("params").at("reason") == "invalid_frame";
    };
    const auto frames = WaitFrames(adapter.get(), is_fatal);
    REQUIRE(CountFrames(frames, is_fatal) == 1);
    CHECK(adapter->spool_pending_count() == 0);
}

TEST_CASE("wecom_adapter: spool 落盘失败——Fatal 留痕但照常上报(企微无补发路)") {
    AdapterHarness harness("spool_fail");
    auto adapter = std::make_unique<WecombotAdapter>(harness.MakeAdapterOptions());
    HostInitializeAndStart(adapter.get());
    CHECK(WaitQuiet([&]() { return adapter->gateway_thread_running(); }));
    adapter->SetSpoolAppendFaultForTest(true);
    ScriptTransport::Push(harness.gateway, MsgCallbackFrame("MSG1", "REQCB1", "在么"));
    // 照常上报:断线补发不存在,内存路不陪葬。
    const auto frames = WaitFrames(adapter.get(), kIsInbound);
    REQUIRE(CountFrames(frames, kIsInbound) == 1);
    const auto is_spool_fatal = [](const nlohmann::json& frame) {
        return frame.value("method", "") == "channel.fatal" &&
               frame.at("params").at("reason") == "spool_write_failed";
    };
    const auto fatals = WaitFrames(adapter.get(), is_spool_fatal);
    CHECK(CountFrames(fatals, is_spool_fatal) == 1);
}

TEST_CASE("wecom_adapter: 重启重投——历史 pending 重新上报(宿主按 msgid 去重)") {
    AdapterHarness harness("replay");
    {
        auto adapter = std::make_unique<WecombotAdapter>(harness.MakeAdapterOptions());
        HostInitializeAndStart(adapter.get());
        CHECK(WaitQuiet([&]() { return adapter->gateway_thread_running(); }));
        ScriptTransport::Push(harness.gateway, MsgCallbackFrame("MSG1", "REQCB1", "在么"));
        REQUIRE(CountFrames(WaitFrames(adapter.get(), kIsInbound), kIsInbound) == 1);
        CHECK(WaitQuiet([&]() { return adapter->spool_pending_count() == 1; }));
        HostWrite(adapter.get(),
                  BuildRequestJson(3, BridgeMethod::Stop, nlohmann::json::object()));
        CHECK(WaitQuiet([&]() { return !adapter->gateway_thread_running(); }));
    }
    // 新适配器同状态根:重投这笔(不 ACK 就留着)。重投 inbound 与 start
    // 回包的先后不保证——直接等 inbound 本身(它能到就证明 start 过了),
    // 不经会排干管道的 HostInitializeAndStart。
    {
        auto adapter = std::make_unique<WecombotAdapter>(harness.MakeAdapterOptions());
        HostWrite(adapter.get(), InitializeFrame(1, "main"));
        HostWrite(adapter.get(), StartFrame(2));
        const auto frames = WaitFrames(adapter.get(), kIsInbound);
        REQUIRE(CountFrames(frames, kIsInbound) == 1);
        const auto inbound = FilterFrames(frames, kIsInbound).at(0);
        CHECK(inbound.at("params").at("provider_event_id") == "MSG1");
    }
}

// ---------------------------------------------------------------------------
// 出站:req_id 锚 / 分段 / 拒绝面
// ---------------------------------------------------------------------------

TEST_CASE("wecom_adapter: 被动回复透传回调 req_id;markdown 正文直达") {
    AdapterHarness harness("send");
    auto adapter = std::make_unique<WecombotAdapter>(harness.MakeAdapterOptions());
    HostInitializeAndStart(adapter.get());
    CHECK(WaitQuiet([&]() { return adapter->gateway_thread_running(); }));
    ScriptTransport::Push(harness.gateway, MsgCallbackFrame("MSG1", "REQCB1", "在么"));
    REQUIRE(CountFrames(WaitFrames(adapter.get(), kIsInbound), kIsInbound) == 1);

    auto send_frame = SendFrame(5, "U1", "回了", "MSG1");
    send_frame["params"]["client_id"] = "wecom-out-1";  // 宿主 delivery 账
    HostWrite(adapter.get(), send_frame);
    const auto is_result5 = [](const nlohmann::json& frame) {
        return frame.contains("id") && frame.at("id") == 5 && frame.contains("result");
    };
    const auto result = WaitFrames(adapter.get(), is_result5);
    const auto results = FilterFrames(result, is_result5);
    REQUIRE(results.size() == 1);
    CHECK(results[0].at("result").at("accepted") == true);
    CHECK(results[0].at("result").at("provider_message_id") == "wecom-out-1");
    // 回话帧走同一条连接(网关线程写侧):req_id 透传 + markdown 正文。
    CHECK(WaitQuiet([&]() { return !ScriptTransport::RespondFrames(harness.gateway).empty(); }));
    const auto responds = ScriptTransport::RespondFrames(harness.gateway);
    REQUIRE(responds.size() == 1);
    const auto respond = nlohmann::json::parse(responds.at(0));
    CHECK(respond.at("headers").at("req_id") == "REQCB1");
    CHECK(respond.at("body").at("msgtype") == "markdown");
    CHECK(respond.at("body").at("markdown").at("content") == "回了");
}

TEST_CASE("wecom_adapter: 超长正文分段(≤20480 字节 UTF-8,同 req_id 多帧)") {
    AdapterHarness harness("chunk");
    auto adapter = std::make_unique<WecombotAdapter>(harness.MakeAdapterOptions());
    HostInitializeAndStart(adapter.get());
    CHECK(WaitQuiet([&]() { return adapter->gateway_thread_running(); }));
    ScriptTransport::Push(harness.gateway, MsgCallbackFrame("MSG1", "REQCB1", "在么"));
    REQUIRE(CountFrames(WaitFrames(adapter.get(), kIsInbound), kIsInbound) == 1);
    auto send_frame = SendFrame(5, "U1", std::string(20'500, 'a'), "MSG1");
    send_frame["params"]["client_id"] = "wecom-out-9";
    HostWrite(adapter.get(), send_frame);
    const auto is_result5 = [](const nlohmann::json& frame) {
        return frame.contains("id") && frame.at("id") == 5 && frame.contains("result");
    };
    const auto result = WaitFrames(adapter.get(), is_result5);
    const auto results = FilterFrames(result, is_result5);
    REQUIRE(results.size() == 1);
    CHECK(results[0].at("result").at("provider_message_id") == "wecom-out-9");
    CHECK(WaitQuiet([&]() { return ScriptTransport::RespondFrames(harness.gateway).size() == 2; }));
    const auto responds = ScriptTransport::RespondFrames(harness.gateway);
    REQUIRE(responds.size() == 2);
    for (const std::string& text : responds) {
        const auto respond = nlohmann::json::parse(text);
        CHECK(respond.at("headers").at("req_id") == "REQCB1");  // 同锚多帧(官方口径)
        const std::string content =
            respond.at("body").at("markdown").at("content").get<std::string>();
        CHECK(content.size() <= kWecomMarkdownMaxBytes);
    }
}

TEST_CASE("wecom_adapter: 无锚明拒(主动推送归 W2);锚不在 PermanentReject") {
    AdapterHarness harness("noanchor");
    auto adapter = std::make_unique<WecombotAdapter>(harness.MakeAdapterOptions());
    HostInitializeAndStart(adapter.get());
    CHECK(WaitQuiet([&]() { return adapter->gateway_thread_running(); }));

    // 没给 reply_to_message_id:NotCapable(W1 只做被动回复)。
    HostWrite(adapter.get(), SendFrame(5, "U1", "主动推"));
    const auto is_error5 = [](const nlohmann::json& frame) {
        return frame.contains("id") && frame.at("id") == 5 && frame.contains("error");
    };
    const auto refused = WaitFrames(adapter.get(), is_error5);
    const auto errors5 = FilterFrames(refused, is_error5);
    REQUIRE(errors5.size() == 1);
    CHECK(errors5[0].at("error").at("message") == "not_capable");

    // 锚对不上(没收到过这条 msgid):PermanentReject,不发帧。
    HostWrite(adapter.get(), SendFrame(6, "U1", "回了", "MSG-GHOST"));
    const auto is_error6 = [](const nlohmann::json& frame) {
        return frame.contains("id") && frame.at("id") == 6 && frame.contains("error");
    };
    const auto ghost = WaitFrames(adapter.get(), is_error6);
    const auto errors6 = FilterFrames(ghost, is_error6);
    REQUIRE(errors6.size() == 1);
    CHECK(errors6[0].at("error").at("message") == "permanent_reject");
    CHECK(ScriptTransport::RespondFrames(harness.gateway).empty());
}

TEST_CASE("wecom_adapter: 平台拒绝回执——errcode 如实透传(永久拒)") {
    AdapterHarness harness("rejected");
    auto adapter = std::make_unique<WecombotAdapter>(harness.MakeAdapterOptions());
    HostInitializeAndStart(adapter.get());
    CHECK(WaitQuiet([&]() { return adapter->gateway_thread_running(); }));
    ScriptTransport::Push(harness.gateway, MsgCallbackFrame("MSG1", "REQCB1", "在么"));
    REQUIRE(CountFrames(WaitFrames(adapter.get(), kIsInbound), kIsInbound) == 1);
    {
        const std::lock_guard<std::mutex> lock(harness.gateway->mutex);
        harness.gateway->respond_errcode = 40058;  // 未知码 → Rejected(永久)
    }
    HostWrite(adapter.get(), SendFrame(5, "U1", "回了", "MSG1"));
    const auto is_error5 = [](const nlohmann::json& frame) {
        return frame.contains("id") && frame.at("id") == 5 && frame.contains("error");
    };
    const auto result = WaitFrames(adapter.get(), is_error5);
    const auto errors = FilterFrames(result, is_error5);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].at("error").at("message") == "permanent_reject");
    CHECK(errors[0].at("error").at("data").at("detail").get<std::string>().find("40058") !=
          std::string::npos);
}

TEST_CASE("wecom_adapter: 连接断掉后递交耗尽——如实报 TransportFailed") {
    AdapterHarness harness("stopped");
    WecombotAdapter::Options options = harness.MakeAdapterOptions();
    options.respond_ack_timeout_ms = 100;
    auto adapter = std::make_unique<WecombotAdapter>(std::move(options));
    HostInitializeAndStart(adapter.get());
    CHECK(WaitQuiet([&]() { return adapter->gateway_thread_running(); }));
    // 先收一封来信记下锚,再掐连接(读流关死 + 后续连接恒失败)。
    ScriptTransport::Push(harness.gateway, MsgCallbackFrame("MSG1", "REQCB1", "在么"));
    REQUIRE(CountFrames(WaitFrames(adapter.get(), kIsInbound), kIsInbound) == 1);
    {
        const std::lock_guard<std::mutex> lock(harness.gateway->mutex);
        harness.gateway->fail_connect = true;
        harness.gateway->exhausted_error =
            transport::WsError{transport::WsError::Kind::Closed, "peer closed", 1000};
    }
    // 等断线进退避(递交无连接可写,等不到回执)。
    WaitFrames(adapter.get(), [](const nlohmann::json& frame) {
        return frame.value("method", "") == "channel.status" &&
               frame.at("params").value("state", "") == "backoff";
    });
    HostWrite(adapter.get(), SendFrame(5, "U1", "回了", "MSG1"));
    const auto is_error5 = [](const nlohmann::json& frame) {
        return frame.contains("id") && frame.at("id") == 5 && frame.contains("error");
    };
    const auto result = WaitFrames(adapter.get(), is_error5, 10'000);
    const auto errors = FilterFrames(result, is_error5);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].at("error").at("message") == "transport_failed");
    CHECK(ScriptTransport::RespondFrames(harness.gateway).empty());
}

// ---------------------------------------------------------------------------
// 限流账(纯账目,合成时钟)
// ---------------------------------------------------------------------------

TEST_CASE("wecom_adapter: 限流账——分钟窗 30 / 小时窗 1000,滑窗等价") {
    WecomRateLedger ledger(WecomRateLedger::Limits{30, 1000});
    CHECK(ledger.WaitMs("c1", 1'000) == 0);
    for (int i = 0; i < 30; ++i) {
        ledger.Record("c1", 10'000);
    }
    // 分钟窗满:等最老一笔出窗。
    CHECK(ledger.WaitMs("c1", 11'000) == 59'000);
    CHECK(ledger.WaitMs("c1", 70'000) == 0);  // 出窗即放行
    // 别的会话不受牵连。
    CHECK(ledger.WaitMs("c2", 11'000) == 0);
    // 小时窗:1000 条后要等最老一笔出小时窗。
    for (int i = 0; i < 1000; ++i) {
        ledger.Record("c3", 100'000);
    }
    CHECK(ledger.WaitMs("c3", 150'000) > 3'500'000);
    CHECK(ledger.WaitMs("c3", 3'700'100) == 0);
}

}  // namespace lubancode::channel::wecombot
