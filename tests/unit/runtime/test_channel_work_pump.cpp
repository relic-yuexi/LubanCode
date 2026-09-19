// QQ 接入单 Q2 总装册:假 provider + 真桥(fake sidecar 走真 Bridge 帧)
// ——渠道消息进 Session V3 与 outbox 的纵向链。
//
// 验收对齐 todo §八 Q2 三行:
//   1) 连续两条消息共享上下文;不同账号/用户/workspace 不串场;停机恢复
//      仍接上上文(resume-as-new + 映射更新);
//   2) 写盘失败/队列满/来信重放/权限撤销/三个执行回复窗口崩溃注入——
//      模型与工具调用计数不多一次;
//   3) QQ 超时未知/明确拒绝/重复回执/限频;发送重试不重跑 Agent。
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "api/anthropic/client.hpp"
#include "channel/manager.hpp"
#include "channel/types.hpp"
#include "fake_channel_sidecar.hpp"
#include "gateway/profile.hpp"
#include "gateway/reply_outbox.hpp"
#include "runtime/channel_work_pump.hpp"
#include "runtime/headless_executor.hpp"
#include "runtime/headless_progress.hpp"
#include "tools/path_utils.hpp"
#include "tools/tool.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"
#include "workspace/identity.hpp"
#include "workspace/index.hpp"

using namespace lubancode;
using lubancode::test_support::FakeChannelSidecar;

namespace {

struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

// 盘上调用计数:进程"重启"(装配销毁重建)后仍在——不多调用的判据。
void CountCall(const std::filesystem::path& counter, const std::string& who) {
    std::error_code ec;
    std::filesystem::create_directories(counter.parent_path(), ec);
    std::ofstream stream(counter, std::ios::binary | std::ios::app);
    stream << who << "\n";
}

std::size_t CountOf(const std::filesystem::path& counter, const std::string& who) {
    std::ifstream stream(counter, std::ios::binary);
    std::string line;
    std::size_t count = 0;
    while (std::getline(stream, line)) {
        if (line == who) ++count;
    }
    return count;
}

// 假 provider:脚本吐事件;每次调用把请求里的对话历史落 dump(上下文
// 共享/隔离的判据——不猜,看真请求里有什么)。
class CaptureBackend : public api::Backend {
public:
    CaptureBackend(std::filesystem::path counter, std::vector<std::vector<api::StreamEvent>> scripts)
        : counter_(std::move(counter)), scripts_(std::move(scripts)) {}

    // 指定 wire 序列化器(P0 刀一案:四家 wire 历史回传形状;空 = 旧行为,
    // SerializeForDiagnostics 回空、adapter 预算闸不启用)。
    std::function<nlohmann::json(const api::Request&)> wire_json;
    std::string SerializeForDiagnostics(const api::Request& request) const override {
        return wire_json ? wire_json(request).dump() : std::string();
    }

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        if (cancel != nullptr && cancel->load()) {
            return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled"});
        }
        CountCall(counter_, "model");
        systems_.push_back(request.system);
        std::string dump = "SYSTEM:" + request.system + "\n";
        for (const auto& message : request.messages) {
            dump += message.role == api::Role::User ? "U:" : "A:";
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                    dump += text->text;
                }
            }
            dump += "\n";
        }
        dumps_.push_back(std::move(dump));
        if (calls_ >= scripts_.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "script exhausted"});
        }
        for (const auto& event : scripts_[calls_]) {
            on_event(event);
        }
        ++calls_;
        return {};
    }

    const std::vector<std::string>& dumps() const { return dumps_; }
    const std::vector<std::string>& systems() const { return systems_; }

private:
    std::filesystem::path counter_;
    std::vector<std::vector<api::StreamEvent>> scripts_;
    std::size_t calls_ = 0;
    std::vector<std::string> dumps_;
    std::vector<std::string> systems_;
};

class ControlledTool : public tools::Tool {
public:
    ControlledTool(std::filesystem::path counter, std::string name)
        : counter_(std::move(counter)), name_(std::move(name)) {}
    std::string name() const override { return name_; }
    std::string description() const override { return "受控测试工具"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }
    tools::Tool::Result execute(const nlohmann::json& /*input*/) override {
        CountCall(counter_, "tool");
        return tools::Tool::Result{"工具观测:仓库干净,无新提交。", false};
    }

private:
    std::filesystem::path counter_;
    std::string name_;
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

// P0 刀一案:带签名的 thinking + 正文(anthropic wire 续会话回传的形状;
// 思考正文用固定占位串,不留真实思考内容)。
std::vector<api::StreamEvent> ThinkingTextScript(const std::string& sig, const std::string& text) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::ThinkingDelta{"[脱敏思考占位]", sig},
        api::ContentBlockDone{0},
        api::TextDelta{text},
        api::ContentBlockDone{1},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

// 真无法预算的输入:加密思考块(anthropic redacted_thinking)。
std::vector<api::StreamEvent> RedactedThinkingTextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::RedactedThinking{"opaque-redacted-payload"},
        api::TextDelta{text},
        api::ContentBlockDone{1},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& name) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::ToolUseStart{0, tool_id, name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

class FakeTransport final : public channel::ChannelBridgeTransport {
public:
    explicit FakeTransport(FakeChannelSidecar& sidecar) : sidecar_(sidecar) {}
    void WriteToSidecar(const std::byte* data, std::size_t size) override {
        sidecar_.FeedFromHost(data, size);
    }
    std::vector<std::byte> DrainFromSidecar() override { return sidecar_.DrainToHost(); }

private:
    FakeChannelSidecar& sidecar_;
};

channel::ChannelInboundEvent MakeDm(const std::string& delivery_id,
                                    const std::string& provider_event_id,
                                    const std::string& conversation, const std::string& text,
                                    const std::string& message_id) {
    channel::ChannelInboundEvent event;
    event.delivery_id = delivery_id;
    event.provider_event_id = provider_event_id;
    event.channel_id = "qqbot";
    event.account_id = "main";
    event.received_at_ms = 1724700000000;
    event.provider_at_ms = 1724699999000;
    event.conversation.kind = channel::ConversationKind::Direct;
    event.conversation.id = conversation;
    event.sender.id = "sender-" + conversation;
    event.sender.display_name = "用户";
    event.message_id = message_id;
    channel::ChannelPart part;
    part.type = channel::ChannelPartType::Text;
    part.text = text;
    event.parts.push_back(part);
    return event;
}

// 夹具参数(独立于 Q2Fixture:类内默认实参里用带 NSDMI 的嵌套类是
// clang 禁例"NSDMI 默认实参",故放 namespace 域)。
struct Q2Params {
    std::int64_t send_timeout_ms = 600;
    std::int64_t send_retry_backoff_ms = 200;
    int max_send_attempts = 3;
    std::size_t max_pending_total = 256;
    // 换 workspace 身份(不串场案):不同 ws 目录 → 不同 workspace_key。
    std::filesystem::path ws_subdir = "ws";
    // Q1b 配对案:dm_policy=Pairing(未知 sender 走 PendingPairing 水路)。
    channel::DmPolicy dm_policy = channel::DmPolicy::Open;
};

// 纵向装配:manager(真 Bridge 帧)+ outbox + 渠道 work 泵。重建即"进程
// 重启"(盘上账:ingress/session map/work ledger/outbox/workspaces 全保留)。
struct Q2Fixture {
    using Params = Q2Params;
    std::filesystem::path root;
    gateway::GatewayProfilePaths paths;
    std::filesystem::path workspaces_root;
    std::filesystem::path channels_root;
    std::filesystem::path ws_root;
    std::filesystem::path counter_file;
    std::int64_t now = 1724700000000;
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<std::string> progress_lines;
    std::size_t context_window_tokens = 0;

    FakeChannelSidecar sidecar;
    FakeTransport transport{sidecar};
    std::unique_ptr<CaptureBackend> backend;
    std::unique_ptr<channel::ChannelManager> manager;
    std::optional<gateway::DurableReplyOutbox> outbox;
    std::optional<runtime::ChannelWorkPump> pump;
    workspace::WorkspaceIdentity identity;
    Params params_;
    // Q4 附件接纳 seam:案内按需装配(默认空 = 渠道未装配下载)。
    runtime::ChannelMediaDownloadFn media_download;

    // rebuild=true:同一 root 上重建(停机恢复/崩溃注入的"新进程")。
    Q2Fixture(const char* tag, Params params = {}, bool rebuild = false)
        : root(std::filesystem::temp_directory_path() /
               ("lubancode-q2-pump-" + std::string(tag))) {
        if (!rebuild) {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(root, ec);
        }
        paths = gateway::ResolveGatewayProfilePaths(root / "gateway", "default");
        workspaces_root = root / "workspaces";
        channels_root = root / "channels";
        ws_root = root / params.ws_subdir;
        counter_file = root / "calls.log";
        std::error_code ec;
        std::filesystem::create_directories(ws_root, ec);

        channel::ChannelManagerOptions manager_options;
        manager_options.state_root = channels_root;
        manager_options.now_ms = [this] { return now; };
        manager_options.alive_checker = [](unsigned long) { return true; };
        manager_options.send_timeout_ms = params.send_timeout_ms;
        manager_options.inbox_limits.max_pending_total = params.max_pending_total;
        manager = std::make_unique<channel::ChannelManager>(std::move(manager_options));

        channel::ChannelAccountUserConfig config;
        config.enabled = true;
        config.transport = "websocket";
        config.secret_env = "QQBOT_SECRET";
        config.dm_policy = params.dm_policy;
        config.tools.allow = {"repo_probe"};
        REQUIRE(manager->AddAccount("qqbot", "main", config, &transport).status ==
                channel::ChannelManager::AddAccountResult::Status::Ok);
        REQUIRE_FALSE(manager->StartAccount("qqbot", "main").has_value());
        manager->Pump("qqbot", "main");
        manager->Pump("qqbot", "main");
        REQUIRE(manager->Snapshot("qqbot", "main")->state ==
                channel::ChannelAccountState::Running);

        outbox.emplace();
        gateway::DurableReplyOutbox::Paths outbox_paths;
        outbox_paths.log_file = paths.outbox_log;
        outbox_paths.replies_dir = paths.replies_dir;
        outbox_paths.published_dir = paths.published_dir;
        REQUIRE(gateway::DurableReplyOutbox::Open(&*outbox, outbox_paths).ok);

        identity = workspace::MakeFallbackIdentity(ws_root);
        params_ = params;
    }

    tools::ToolRegistry MakeRegistry() {
        tools::ToolRegistry registry;
        registry.Register(std::make_unique<ControlledTool>(counter_file, "repo_probe"));
        return registry;
    }

    runtime::ChannelWorkPump::OpenResult OpenPump(
        tools::ToolRegistry& registry,
        const std::function<std::string(runtime::HeadlessExecutor::Options::FaultPoint)>& fault =
            {},
        const std::function<std::string()>& fault_after_enqueue = {},
        bool broken_outbox = false) {
        runtime::ChannelWorkPump::Options options;
        options.manager = manager.get();
        options.outbox = &*outbox;
        options.channels_state_root = channels_root;
        options.workspaces_root = workspaces_root;
        options.workspace_identity = identity;
        options.cwd_utf8 = tools::PathToUtf8(root);
        options.lubancode_version = "0.26.238-test";
        options.wire_name = "test-wire";
        options.model = "test-model";
        options.context_window_tokens = context_window_tokens;
        options.on_progress = [this](const std::string& line) { progress_lines.push_back(line); };
        options.tools.allow = {"repo_probe"};
        options.max_steps_per_turn = 8;
        options.send_retry_backoff_ms = params_.send_retry_backoff_ms;
        options.max_send_attempts = params_.max_send_attempts;
        options.now_ms = [this] { return now; };
        options.fault_injection = fault;
        options.fault_after_enqueue = fault_after_enqueue;
        options.media_download = media_download;
        if (broken_outbox) {
            std::error_code ec;
            std::filesystem::create_directories(paths.outbox_log, ec);  // 目录占住账文件
        }
        backend = std::make_unique<CaptureBackend>(counter_file, scripts);
        pump.emplace();
        auto open = runtime::ChannelWorkPump::Open(&*pump, *backend, registry,
                                                   std::move(options));
        if (open.ok) {
            pump->set_owner_epoch("q2-test-epoch");
        }
        return open;
    }

    // 一 tick:推进泵(泵内自带桥泵字节往返),时间走一步。
    bool Tick(std::int64_t step_ms = 100) {
        const bool ok = pump->TickOnce(now);
        now += step_ms;
        return ok;
    }

    void EmitAndIngest(const channel::ChannelInboundEvent& event) {
        sidecar.EmitInboundEvent(event);
        manager->Pump("qqbot", "main");
    }

    // 收敛到没有新进展(投递全终态、无 Running/待办)。
    void TickUntilQuiet(int max_ticks = 40) {
        for (int i = 0; i < max_ticks; ++i) {
            Tick();
        }
    }

    std::string IngressStateNameOf(std::int64_t sid) {
        const auto records = manager->IngressRecords("qqbot", "main");
        for (const auto& record : records) {
            if (record.sid == sid) return channel::IngressEventStateName(record.state);
        }
        return "missing";
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 验收 1:上下文共享/隔离/停机恢复
// ---------------------------------------------------------------------------

TEST_CASE("连续两条消息共享上下文:同会话同场,第二轮请求带第一轮对话") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("shared");
    fixture.context_window_tokens = 64000;
    fixture.scripts = {TextScript("第一答:记住了暗号甲。"), TextScript("第二答:暗号是甲。")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "暗号是甲,记住", "m-1"));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    const std::string session1 = fixture.pump->session_id_for("qqbot", "main",
                                                                   "dm-a");
    REQUIRE_FALSE(session1.empty());

    fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-a", "暗号是什么", "m-2"));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 2);
    // 同一场(不另开空场)。
    REQUIRE(fixture.pump->session_id_for("qqbot", "main", "dm-a") == session1);
    // 第二轮请求真带了第一轮的对话(上下文共享,不靠猜)。
    REQUIRE(fixture.backend->dumps().size() == 2);
    const std::string& second = fixture.backend->dumps()[1];
    CHECK(second.find("会话启动目录:") != std::string::npos);
    CHECK(second.find("本轮开始时间(宿主时钟):") == std::string::npos);
    CHECK(second.find("get_current_time") != std::string::npos);
    CHECK(second.find("delay_seconds") != std::string::npos);
    REQUIRE(fixture.backend->systems().size() == 2);
    CHECK(fixture.backend->systems()[0] == fixture.backend->systems()[1]);
    CHECK(fixture.backend->systems()[1].find("本轮开始时间(宿主时钟):") == std::string::npos);
    REQUIRE(second.find("暗号是甲") != std::string::npos);
    REQUIRE(second.find("第一答") != std::string::npos);
    // 两封信都投递成功(sidecar 收到两条回复)。
    REQUIRE(fixture.sidecar.sent_messages().size() == 2);
    const auto has_progress = [&](const std::string& text) {
        return std::any_of(fixture.progress_lines.begin(), fixture.progress_lines.end(),
            [&](const std::string& line) { return line.find(text) != std::string::npos; });
    };
    CHECK(has_progress("运行窗口=64000"));
    CHECK(has_progress("模型回合结束"));
    CHECK(has_progress("回复已入投递队列"));
    CHECK(has_progress("QQ 已接受回复"));
}

TEST_CASE("前台用量:缓存计入输入,未知不冒充零,上下文不累计") {
    std::string log;
    runtime::HeadlessProgressReporter reporter(
        [&](const std::string& line) { log += line + "\n"; }, "qqbot/main", "test");
    agent::ContextPressure pressure;
    pressure.phase = agent::ContextPressure::Phase::PreRequest;
    pressure.window_tokens = 10000;
    pressure.working_view_tokens = 800;
    pressure.projected_tokens = 2000;
    reporter.Context(pressure);
    runtime::ServerEvent event;
    event.kind = runtime::ServerEventKind::ModelStepStarted;
    reporter.Observe(event);
    event.kind = runtime::ServerEventKind::UsageUpdated;
    event.payload = {{"reported_by_provider", true}, {"input_tokens", 100},
        {"output_tokens", 20}, {"cache_read_tokens", 900},
        {"cache_read_reported_by_provider", true}};
    reporter.Observe(event);
    CHECK(log.find("输入=1000 输出=20 缓存读=900 缓存写=未报告") != std::string::npos);
    CHECK(log.find("命中率=90.0%") != std::string::npos);
    CHECK(log.find("本次输入占运行窗口=1000/10000（10.0%）") != std::string::npos);
    event.kind = runtime::ServerEventKind::ModelStepStarted;
    reporter.Observe(event);
    event.kind = runtime::ServerEventKind::UsageUpdated;
    event.payload = {{"reported_by_provider", true}, {"input_tokens", 500},
        {"cache_read_reported_by_provider", true}, {"cache_creation_reported_by_provider", true}};
    reporter.Observe(event);
    CHECK(log.find("缓存读=0 缓存写=0") != std::string::npos);
    CHECK(log.find("本次输入占运行窗口=500/10000（5.0%）") != std::string::npos);
    event.kind = runtime::ServerEventKind::ModelStepStarted;
    reporter.Observe(event);
    event.kind = runtime::ServerEventKind::UsageUpdated;
    event.payload = nlohmann::json::object();
    reporter.Observe(event);
    CHECK(log.find("输入=未报告 输出=未报告") != std::string::npos);
    event.kind = runtime::ServerEventKind::TurnCompleted;
    event.outcome = runtime::Outcome::Succeeded;
    reporter.Observe(event);
    CHECK(log.find("累计输入=1500 累计输出=20（部分请求未报告）") != std::string::npos);
    CHECK(log.find("缓存写合计=0（仅已报告）") != std::string::npos);
}

TEST_CASE("前台显示:单行预览保留完整 UTF8,显示故障不阻断执行") {
    CHECK(runtime::HeadlessProgressReporter::Preview("中文", 4) == "中…");
    CHECK(runtime::HeadlessProgressReporter::Preview("a\nb\x1b") == "a b ");
    runtime::HeadlessProgressReporter reporter(
        [](const std::string&) { throw std::runtime_error("display failed"); }, "qq", "test");
    runtime::ServerEvent event;
    event.kind = runtime::ServerEventKind::TurnStarted;
    CHECK_NOTHROW(reporter.Observe(event));
}

TEST_CASE("不同用户不串场:dm-a 与 dm-b 各开各场,互不见对方对话") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("isolate-user");
    fixture.scripts = {TextScript("a1"), TextScript("b1")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "甲的秘密", "m-1"));
    fixture.Tick();
    fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-b", "乙的问题", "m-2"));
    fixture.Tick();
    fixture.TickUntilQuiet();

    REQUIRE(fixture.backend->dumps().size() == 2);
    // dm-b 的请求里没有 dm-a 的对话。
    REQUIRE(fixture.backend->dumps()[1].find("甲的秘密") == std::string::npos);
    const std::string session_a = fixture.pump->session_id_for("qqbot", "main", "dm-a");
    const std::string session_b = fixture.pump->session_id_for("qqbot", "main", "dm-b");
    REQUIRE_FALSE(session_a.empty());
    REQUIRE(session_a != session_b);
}

TEST_CASE("不同 workspace 不串场:换 cwd 重启后开新场,不误续别的项目") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "lubancode-q2-pump-isolate-ws";
    {
        Q2Fixture fixture("isolate-ws");
        fixture.scripts = {TextScript("workspace-甲的回答")};
        auto registry = fixture.MakeRegistry();
        REQUIRE(fixture.OpenPump(registry).ok);
        fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "甲项目的问题", "m-1"));
        fixture.Tick();
        fixture.TickUntilQuiet();
        REQUIRE(fixture.pump->Close(0));
    }
    // 重启:换了 workspace(ws2),映射键对不上 → 新场;模型看不见旧对话。
    {
        Q2Fixture::Params params;
        params.ws_subdir = "ws2";
        Q2Fixture fixture("isolate-ws", params, /*rebuild=*/true);
        fixture.scripts = {TextScript("workspace-乙的回答")};
        auto registry = fixture.MakeRegistry();
        REQUIRE(fixture.OpenPump(registry).ok);
        fixture.EmitAndIngest(MakeDm("in-9", "pe-9", "dm-a", "乙项目的问题", "m-9"));
        fixture.Tick();
        fixture.TickUntilQuiet();
        REQUIRE(fixture.backend->dumps().size() == 1);
        REQUIRE(fixture.backend->dumps()[0].find("甲项目的问题") == std::string::npos);
    }
}

TEST_CASE("停机恢复仍接上上文:重建后 resume-as-new,新场带旧上下文,映射更新") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    std::string session1;
    {
        Q2Fixture fixture("resume");
        fixture.scripts = {TextScript("第一答:暗号甲。")};
        auto registry = fixture.MakeRegistry();
        REQUIRE(fixture.OpenPump(registry).ok);
        fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "暗号是甲", "m-1"));
        fixture.Tick();
        fixture.TickUntilQuiet();
        session1 = fixture.pump->session_id_for("qqbot", "main", "dm-a");
        REQUIRE_FALSE(session1.empty());
        REQUIRE(fixture.pump->Close(0));  // 干净停机(渠道活场封口)
    }
    {
        Q2Fixture fixture("resume", {}, /*rebuild=*/true);
        fixture.scripts = {TextScript("第二答。")};
        auto registry = fixture.MakeRegistry();
        REQUIRE(fixture.OpenPump(registry).ok);
        fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-a", "暗号是什么", "m-2"));
        fixture.Tick();
        fixture.TickUntilQuiet();
        // 续接源场(2026-09-19 拍板):同 id 续写,映射不变,上下文接得上。
        const std::string session2 = fixture.pump->session_id_for("qqbot", "main", "dm-a");
        REQUIRE_FALSE(session2.empty());
        CHECK(session2 == session1);
        REQUIRE(fixture.backend->dumps().size() == 1);
        REQUIRE(fixture.backend->dumps()[0].find("暗号是甲") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// 验收 2:重放/撤销/队列满/写盘失败/三窗崩溃注入
// ---------------------------------------------------------------------------

TEST_CASE("来信重放:同 delivery 重投只跑一次模型") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("replay");
    fixture.scripts = {TextScript("答")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    const auto event = MakeDm("in-1", "pe-1", "dm-a", "问题", "m-1");
    fixture.EmitAndIngest(event);
    fixture.Tick();
    fixture.TickUntilQuiet();
    // sidecar 未收到 ack 前按退避重发(同 delivery)。
    fixture.EmitAndIngest(event);
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    REQUIRE(fixture.sidecar.sent_messages().size() == 1);
}

TEST_CASE("权限撤销:排队输入执行前重验, revoked 落 rejected 零模型调用") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("revoke");
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "问题", "m-1"));
    // 取件前撤准入:两条同档(conversation 档)binding 互相冲突 →
    // binding_conflict,重验不过就地 rejected。
    std::vector<channel::ChannelBindingConfig> bindings(2);
    for (auto& binding : bindings) {
        channel::ChannelBindingConversationMatch conversation;
        conversation.kind = "direct";
        conversation.id = "dm-a";
        binding.match.conversation = conversation;
    }
    fixture.manager->SetChannelBindings("qqbot", std::move(bindings));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 0);
    REQUIRE(fixture.IngressStateNameOf(1) == "rejected");
}

TEST_CASE("队列满:背压进 rate_limited,不默丢不执行") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture::Params params;
    params.max_pending_total = 1;
    Q2Fixture fixture("queuefull", params);
    fixture.scripts = {TextScript("答1"), TextScript("答2")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "问题一", "m-1"));
    fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-b", "问题二", "m-2"));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    // 只跑了排进队的那枚;另一枚 rate_limited 旁路留账。
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    REQUIRE(fixture.IngressStateNameOf(2) == "rate_limited");
}

TEST_CASE("写盘失败:outbox 账开不了 → 入箱失败停泵,不再多跑模型") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("diskfail");
    fixture.scripts = {TextScript("答")};
    auto registry = fixture.MakeRegistry();
    // 先占住 outbox 账文件(目录),再开泵。
    std::error_code ec;
    std::filesystem::create_directories(fixture.paths.outbox_log, ec);
    REQUIRE(fixture.OpenPump(registry).ok);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "问题", "m-1"));
    REQUIRE_FALSE(fixture.Tick());  // 执行成功、入箱失败 → 泵停(写盘失败停止推进)
    REQUIRE_FALSE(fixture.Tick());  // broken:不再推进,不再多跑模型
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
}

namespace {

// 三窗共用骨架:注入 → "进程死"(装配销毁,不 Close)→ 重建 → 恢复,
// 全程模型/工具计数不多一次,回复恰好投一份。
void HardKillWindowCase(const char* tag,
                        runtime::HeadlessExecutor::Options::FaultPoint point) {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture::Params params;
    params.send_timeout_ms = 300;
    {
        Q2Fixture fixture(tag, params);
        fixture.scripts = {ToolUseScript("tu-1", "repo_probe"), TextScript("窗口案回答")};
        auto registry = fixture.MakeRegistry();
        std::function<std::string(runtime::HeadlessExecutor::Options::FaultPoint)> fault =
            [point](runtime::HeadlessExecutor::Options::FaultPoint at) {
                return at == point ? std::string("kill") : std::string();
            };
        REQUIRE(fixture.OpenPump(registry, fault).ok);
        fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "窗口案问题", "m-1"));
        REQUIRE(fixture.Tick());  // fault 注入:按"进程死"收场
    }
    {
        Q2Fixture fixture(tag, params, /*rebuild=*/true);
        fixture.scripts = {TextScript("不该跑的第二次")};
        auto registry = fixture.MakeRegistry();
        REQUIRE(fixture.OpenPump(registry).ok);
        // 恢复:补 selection/outbox 投影,投递出去;不重跑 Agent。
        REQUIRE(fixture.Tick());
        fixture.TickUntilQuiet();
        REQUIRE(CountOf(fixture.counter_file, "model") == 2);  // 工具轮+终答,各一次
        REQUIRE(CountOf(fixture.counter_file, "tool") == 1);
        REQUIRE(fixture.sidecar.sent_messages().size() == 1);
        const auto& sent = fixture.sidecar.sent_messages()[0];
        REQUIRE(sent.params["parts"][0]["text"] == "窗口案回答");
        REQUIRE(fixture.IngressStateNameOf(1) == "delivered");
    }
}

}  // namespace

TEST_CASE("崩溃窗口 1:生成后、selection 前——恢复补齐全程不调模型") {
    HardKillWindowCase("window1", runtime::HeadlessExecutor::Options::FaultPoint::AfterGeneration);
}
TEST_CASE("崩溃窗口 2:selection 后、outbox 前——恢复补投影不重跑") {
    HardKillWindowCase("window2",
                       runtime::HeadlessExecutor::Options::FaultPoint::AfterSelectionCommitted);
}
TEST_CASE("崩溃窗口 3:入 outbox 后、发送前——恢复续投不重跑") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture::Params params;
    params.send_timeout_ms = 300;
    {
        Q2Fixture fixture("window3", params);
        fixture.scripts = {TextScript("窗口三回答")};
        auto registry = fixture.MakeRegistry();
        std::function<std::string()> after_enqueue = [] { return std::string("kill"); };
        REQUIRE(fixture.OpenPump(registry, {}, after_enqueue).ok);
        fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "窗口三问题", "m-1"));
        REQUIRE(fixture.Tick());
        REQUIRE(fixture.sidecar.sent_messages().empty());  // 没发出就"死"了
    }
    {
        Q2Fixture fixture("window3", params, /*rebuild=*/true);
        auto registry = fixture.MakeRegistry();
        REQUIRE(fixture.OpenPump(registry).ok);
        REQUIRE(fixture.Tick());
        fixture.TickUntilQuiet();
        REQUIRE(CountOf(fixture.counter_file, "model") == 1);  // 不重跑
        REQUIRE(fixture.sidecar.sent_messages().size() == 1);
        REQUIRE(fixture.IngressStateNameOf(1) == "delivered");
    }
}

TEST_CASE("claim 后崩(无绑定行):needs_review 进死信,不盲重跑") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture::Params params;
    params.send_timeout_ms = 300;
    {
        Q2Fixture fixture("claimcrash", params);
        // 不开泵:取件(claim → ingress Running)后"进程死"——模拟泵在
        // claim 后、绑定行落账前瞬间崩溃。
        fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "问题", "m-1"));
        REQUIRE(fixture.manager->TakeNextWork("qqbot", "main").has_value());
    }
    {
        Q2Fixture fixture("claimcrash", params, /*rebuild=*/true);
        auto registry = fixture.MakeRegistry();
        REQUIRE(fixture.OpenPump(registry).ok);
        REQUIRE(fixture.Tick());
        fixture.TickUntilQuiet();
        REQUIRE(CountOf(fixture.counter_file, "model") == 0);  // 不盲重跑
        REQUIRE(fixture.IngressStateNameOf(1) == "dead_letter");
    }
}

// ---------------------------------------------------------------------------
// 验收 3:QQ 投递族(超时未知/明确拒绝/重复回执/限频)
// ---------------------------------------------------------------------------

TEST_CASE("超时未知:无回执 → delivery_unknown,停自动重发,不虚 exactly-once") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture::Params params;
    params.send_timeout_ms = 300;
    Q2Fixture fixture("timeout", params);
    fixture.scripts = {TextScript("超时案回答")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);
    fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::Silent);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "问题", "m-1"));
    REQUIRE(fixture.Tick());
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);  // Agent 不重跑
    // 恰好发过一次;超时后不再自动重发。
    const std::string delivery = fixture.sidecar.sent_messages()[0].client_id;
    REQUIRE(fixture.sidecar.send_count_for(delivery) == 1);
    // outbox:delivery_unknown 终态;ingress 投递失败(执行事实保留)。
    const auto items = fixture.outbox->ListItems();
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].state == "delivery_unknown");
    REQUIRE(fixture.IngressStateNameOf(1) == "delivery_failed");
    // 已结算的 delivery 不再发。
    REQUIRE(fixture.Tick());
    REQUIRE(fixture.sidecar.send_count_for(delivery) == 1);
}

TEST_CASE("明确拒绝:platform_reject 终态,不重试不重跑,ingress 投递失败") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("reject");
    fixture.scripts = {TextScript("拒绝案回答")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);
    fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::PermanentReject);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "问题", "m-1"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    const std::string delivery = fixture.sidecar.sent_messages()[0].client_id;
    REQUIRE(fixture.sidecar.send_count_for(delivery) == 1);  // 拒绝不重试
    const auto items = fixture.outbox->ListItems();
    REQUIRE(items[0].state == "failed");
    REQUIRE(items[0].delivery_error == "platform_reject");
    REQUIRE(fixture.IngressStateNameOf(1) == "delivery_failed");
}

TEST_CASE("重复回执:send 响应已结,再来的 delivery.receipt 不翻账") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("dupreceipt");
    fixture.scripts = {TextScript("重复回执案回答")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "问题", "m-1"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    const std::string delivery = fixture.sidecar.sent_messages()[0].client_id;
    const auto items = fixture.outbox->ListItems();
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].state == "sent");
    // 迟到的重复回执:账不翻(仍 sent,仍一份)。
    fixture.sidecar.EmitDeliveryReceipt(delivery, "failed", "late");
    REQUIRE(fixture.Tick());
    REQUIRE(fixture.outbox->ListItems()[0].state == "sent");
    REQUIRE(fixture.IngressStateNameOf(1) == "delivered");
}

TEST_CASE("限频:退避后同载荷重试(同 client_id),成功后不重跑 Agent") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture::Params params;
    params.send_retry_backoff_ms = 200;
    params.max_send_attempts = 3;
    Q2Fixture fixture("ratelimit", params);
    fixture.scripts = {TextScript("限频案回答")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);
    fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::RateLimitedFirst);
    fixture.sidecar.set_rate_limited_first(1);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "问题", "m-1"));
    fixture.Tick();          // 发出 → 限频
    REQUIRE(fixture.sidecar.sent_messages().size() == 1);
    fixture.TickUntilQuiet();  // 退避后重试同载荷 → 成功
    const std::string delivery = fixture.sidecar.sent_messages()[0].client_id;
    REQUIRE(fixture.sidecar.send_count_for(delivery) == 2);
    // 两次载荷逐字节同(冻结正文 + 同 client_id → 同 msg_seq 底)。
    REQUIRE(fixture.sidecar.sent_messages()[0].params ==
            fixture.sidecar.sent_messages()[1].params);
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);  // 发送重试不重跑 Agent
    REQUIRE(fixture.outbox->ListItems()[0].state == "sent");
    REQUIRE(fixture.IngressStateNameOf(1) == "delivered");
}

// ---------------------------------------------------------------------------
// A05:msg_seq 持久冻结——同来信分段各占一号;限频重试/重启重投沿用原号。
// ---------------------------------------------------------------------------

TEST_CASE("A05 msg_seq 冻结:分段各占一号;限频后进程重启重投沿用原号") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture::Params params;
    params.send_timeout_ms = 300;
    params.send_retry_backoff_ms = 100;
    params.max_send_attempts = 3;
    std::string delivery_id;
    {
        Q2Fixture fixture("seqfreeze", params);
        fixture.scripts = {TextScript("短回答")};
        auto registry = fixture.MakeRegistry();
        REQUIRE(fixture.OpenPump(registry).ok);
        fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::RateLimitedFirst);
        fixture.sidecar.set_rate_limited_first(1);

        fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "问题", "m-1"));
        REQUIRE(fixture.Tick());  // 首发被限频:身份已冻结(seq=1),尝试已记账
        REQUIRE(fixture.sidecar.sent_messages().size() == 1);
        delivery_id = fixture.sidecar.sent_messages()[0].client_id;
        // 首发载荷:被动锚 + 持久分配的 1 号(不是 ordinal 顶替,是发号器)。
        const auto& first = fixture.sidecar.sent_messages()[0];
        REQUIRE(first.params.contains("reply_to_message_id"));
        CHECK(first.params["reply_to_message_id"] == "m-1");
        REQUIRE(first.params.contains("msg_seq"));
        CHECK(first.params["msg_seq"] == 1);
        // 这里"进程死"(不退避重试):限频窗口 + 重启 = 三窗口之三
        //(重启后重投必须沿用原号,平台去重才认得是同一条)。
    }
    {
        Q2Fixture fixture("seqfreeze", params, /*rebuild=*/true);
        auto registry = fixture.MakeRegistry();
        REQUIRE(fixture.OpenPump(registry).ok);  // 假件恢复 AutoAccept
        REQUIRE(fixture.Tick());
        fixture.TickUntilQuiet();
        // 不重跑 Agent;恰再发一次;重投沿用冻结的 1 号(账上身份,不是
        // 内存新号)。
        REQUIRE(CountOf(fixture.counter_file, "model") == 1);
        REQUIRE(fixture.sidecar.send_count_for(delivery_id) == 1);
        const auto& resent = fixture.sidecar.sent_messages()[0];
        CHECK(resent.client_id == delivery_id);
        REQUIRE(resent.params.contains("msg_seq"));
        CHECK(resent.params["msg_seq"] == 1);
        REQUIRE(fixture.outbox->ListItems().size() == 1);
        REQUIRE(fixture.outbox->ListItems()[0].state == "sent");
    }
}

TEST_CASE("A05 msg_seq 发号:同来信多段与第二条回复各占一号,不撞号") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("seqcounter");
    // 长文两段(拆段 >1 会自动附产物附件段:共 3 段)。
    std::string long_text;
    for (int i = 0; i < 800; ++i) {
        long_text += "字";
    }
    fixture.scripts = {TextScript(long_text), TextScript("第二条回答")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "长问题", "m-1"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    // 同一封来信(m-1)下 3 段:msg_seq 1/2/3(ordinal 顶替的旧病是全 1)。
    REQUIRE(fixture.sidecar.sent_messages().size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& send = fixture.sidecar.sent_messages()[i];
        REQUIRE(send.params.contains("msg_seq"));
        CHECK(send.params["msg_seq"] == static_cast<std::uint64_t>(i + 1));
        CHECK(send.params["reply_to_message_id"] == "m-1");
    }

    // 第二封来信(m-2)的新回复:发号器按 (账号,锚) 各自计数,从 1 起。
    fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-a", "再来", "m-2"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(fixture.sidecar.sent_messages().size() == 4);
    const auto& second_reply = fixture.sidecar.sent_messages()[3];
    CHECK(second_reply.params["reply_to_message_id"] == "m-2");
    REQUIRE(second_reply.params.contains("msg_seq"));
    CHECK(second_reply.params["msg_seq"] == 1);
}

TEST_CASE("长文拆段:文本段按序投递,Q4 产物附件殿后,全 sent 才结算 delivered") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("segment");
    // 800 个汉字(2400 字节)→ 段帽 2000:两文本段;Q4 起长回复(拆段>1)
    // 末尾自动附带任务结果文件(冻结正文原件)——共 3 段,末段纯附件。
    std::string long_text;
    for (int i = 0; i < 800; ++i) {
        long_text += "字";
    }
    fixture.scripts = {TextScript(long_text)};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "长文问题", "m-1"));
    fixture.Tick();
    REQUIRE(fixture.sidecar.sent_messages().size() == 1);  // 段 1 先发
    fixture.TickUntilQuiet();
    REQUIRE(fixture.sidecar.sent_messages().size() == 3);  // 文本段 2 + 附件段接上
    const auto& first = fixture.sidecar.sent_messages()[0];
    const auto& second = fixture.sidecar.sent_messages()[1];
    REQUIRE(first.params["client_id"] != second.params["client_id"]);
    REQUIRE(first.params["reply_to_message_id"] == "m-1");
    REQUIRE(first.params["parts"][0]["text"] != second.params["parts"][0]["text"]);
    // 末段是任务结果文件(Q4:拆段>1 自动附带)。
    const auto& last = fixture.sidecar.sent_messages()[2];
    REQUIRE(last.params["parts"][0]["type"] == "file");
    REQUIRE(fixture.IngressStateNameOf(1) == "delivered");
    REQUIRE(fixture.outbox->ListItems().size() == 3);
}

TEST_CASE("工具轮走完整链:五层交集放行的工具真执行并计数一次") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("toolround");
    fixture.scripts = {ToolUseScript("tu-1", "repo_probe"), TextScript("工具案回答")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "查一下仓库", "m-1"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 2);
    REQUIRE(CountOf(fixture.counter_file, "tool") == 1);
    for (const auto* expected : {"请求工具 repo_probe", "开始执行工具 repo_probe", "工具结果 repo_probe"}) {
        CHECK(std::any_of(fixture.progress_lines.begin(), fixture.progress_lines.end(),
            [&](const std::string& line) { return line.find(expected) != std::string::npos; }));
    }
    REQUIRE(fixture.IngressStateNameOf(1) == "delivered");
    // V3 流:绑定事件带渠道审计载荷。
    const std::string session = fixture.pump->session_id_for("qqbot", "main", "dm-a");
    const auto room = workspace::index::ResolveDirByWorkspaceKey(
        fixture.workspaces_root, workspace::MakeFallbackIdentity(fixture.ws_root).workspace_key);
    REQUIRE(room.has_value());
    const auto stream = trajectory::v3::FindV3SessionStream(*room / "sessions" / session);
    REQUIRE(stream.has_value());
    const auto ledger = trajectory::v3::ReadV3Ledger(*stream);
    REQUIRE(ledger.has_value());
    bool saw_channel_binding = false;
    for (const auto& event : ledger->events) {
        if (event.kind == trajectory::v3::EventKindV3::GatewayWorkBound &&
            event.payload.contains("channel") && event.payload["channel"].is_object() &&
            event.payload["channel"].contains("ingressSid")) {
            saw_channel_binding = true;
        }
    }
    REQUIRE(saw_channel_binding);
}

TEST_CASE("关机次序:StopAccepting 后不取新活;Close 幂等") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("shutdown");
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);
    fixture.pump->StopAccepting();
    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "问题", "m-1"));
    for (int i = 0; i < 5; ++i) {
        REQUIRE(fixture.Tick());
    }
    REQUIRE(CountOf(fixture.counter_file, "model") == 0);  // 不取新活
    REQUIRE(fixture.pump->Close(0));
    REQUIRE(fixture.pump->Close(0));  // 幂等
}

// ---------------------------------------------------------------------------
// QQ 接入单 Q1b:配对提示与批准闭环(假渠道 fixture 纵向链)
// ---------------------------------------------------------------------------

// 从 sidecar 收到的 channel.send 正文里抠配对码("配对码: XXXXXXXX(")。
std::string PairingCodeFromSend(const test_support::FakeChannelSidecar::RecordedSend& send) {
    const std::string text = send.params["parts"][0]["text"].get<std::string>();
    const std::string mark = "配对码: ";
    const std::size_t at = text.find(mark);
    if (at == std::string::npos) return std::string();
    return text.substr(at + mark.size(), channel::kPairingCodeLength);
}

TEST_CASE("Q1b 配对闭环:未配对来信→提示带码→批准→重发→进模型") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Params params;
    params.dm_policy = channel::DmPolicy::Pairing;
    Q2Fixture fixture("pairing_loop", params);
    fixture.scripts = {TextScript("批准后的第一答")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    // 1) 未配对来信:零模型调用,回的是宿主提示(带一次性配对码)。
    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-stranger", "帮我干活", "m-1"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 0);
    REQUIRE(fixture.sidecar.sent_messages().size() == 1);
    const std::string code = PairingCodeFromSend(fixture.sidecar.sent_messages()[0]);
    REQUIRE(code.size() == 8);
    // 提示锚定来者会话与来信消息(被动回复)。
    REQUIRE(fixture.sidecar.sent_messages()[0].params["conversation"]["id"] == "dm-stranger");
    REQUIRE(fixture.sidecar.sent_messages()[0].params["reply_to_message_id"] == "m-1");
    // 原信不进执行:ingress rejected pairing_pending。
    REQUIRE(fixture.IngressStateNameOf(1) == "rejected");

    // 2) 本地批准(另一终端的 CLI 命令最终走到的口)。
    std::string error;
    const auto approved = fixture.manager->ApprovePairing("qqbot", "main", code, &error);
    REQUIRE(approved.has_value());
    CHECK(*approved == "sender-dm-stranger");
    // 配对码一次性:再用同码批,明报。
    CHECK_FALSE(fixture.manager->ApprovePairing("qqbot", "main", code, &error).has_value());
    CHECK(error == "already_finalized");

    // 3) 批准后原消息不自动执行(计数仍 0);用户重发才进模型。
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 0);
    fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-stranger", "重发:帮我干活", "m-2"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    // 第二条出站 = 模型回复(不是提示:没有"配对码"字样)。
    REQUIRE(fixture.sidecar.sent_messages().size() == 2);
    const std::string reply =
        fixture.sidecar.sent_messages()[1].params["parts"][0]["text"].get<std::string>();
    CHECK(reply.find("批准后的第一答") != std::string::npos);
    CHECK(reply.find("配对码") == std::string::npos);
    REQUIRE(fixture.IngressStateNameOf(2) == "delivered");
}

TEST_CASE("Q1b 提示限频:冷却窗内第二封不重发;重启不重发;窗过出新码") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Params params;
    params.dm_policy = channel::DmPolicy::Pairing;
    {
        Q2Fixture fixture("pairing_rate", params);
        auto registry = fixture.MakeRegistry();
        REQUIRE(fixture.OpenPump(registry).ok);

        fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-stranger", "你好", "m-1"));
        fixture.Tick();
        fixture.TickUntilQuiet();
        REQUIRE(fixture.sidecar.sent_messages().size() == 1);

        // 窗内第二封(过 30s code 冷却,不过 5min 提示冷却):不重发提示。
        fixture.now += channel::kPairingRequestCooldownMs + 2000;
        fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-stranger", "还在吗", "m-2"));
        fixture.Tick();
        fixture.TickUntilQuiet();
        REQUIRE(fixture.sidecar.sent_messages().size() == 1);  // 只有一条提示
        REQUIRE(CountOf(fixture.counter_file, "model") == 0);
        REQUIRE(fixture.pump->Close(0));
    }
    // 重启(同账重建):持久已提示账在,同窗内第三封也不重发。
    {
        Q2Fixture fixture("pairing_rate", params, /*rebuild=*/true);
        fixture.now += channel::kPairingRequestCooldownMs + 2000;
        auto registry = fixture.MakeRegistry();
        REQUIRE(fixture.OpenPump(registry).ok);
        fixture.EmitAndIngest(MakeDm("in-3", "pe-3", "dm-stranger", "第三次", "m-3"));
        fixture.Tick();
        fixture.TickUntilQuiet();
        // 新进程的 sidecar 一张白纸:重启后窗内来信零提示(不重发刷屏)。
        REQUIRE(fixture.sidecar.sent_messages().empty());
        REQUIRE(CountOf(fixture.counter_file, "model") == 0);
        // 冷却窗过了:新来信出新提示(新码)。
        fixture.now += channel::kPairingNoticeCooldownMs;
        fixture.EmitAndIngest(MakeDm("in-4", "pe-4", "dm-stranger", "第四次", "m-4"));
        fixture.Tick();
        fixture.TickUntilQuiet();
        REQUIRE(fixture.sidecar.sent_messages().size() == 1);
        const std::string code = PairingCodeFromSend(fixture.sidecar.sent_messages()[0]);
        REQUIRE(code.size() == 8);
        // outbox 幂等账:重启前后两枚提示是两枚不同 delivery(不同码不同
        // selection;盘上账保留,旧的已 sent 不重投)。
        REQUIRE(fixture.outbox->ListItems().size() == 2);
    }
}

TEST_CASE("Q1b 被拒 sender:来信零提示零模型,回执链上安静") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Params params;
    params.dm_policy = channel::DmPolicy::Pairing;
    Q2Fixture fixture("pairing_reject", params);
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-spam", "广告", "m-1"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(fixture.sidecar.sent_messages().size() == 1);
    const std::string code = PairingCodeFromSend(fixture.sidecar.sent_messages()[0]);
    REQUIRE(code.size() == 8);
    std::string error;
    REQUIRE(fixture.manager->RejectPairing("qqbot", "main", code, &error).has_value());

    // 拒过之后:同 sender 再来信,零新提示零模型。
    fixture.now += channel::kPairingNoticeCooldownMs + channel::kPairingRequestCooldownMs;
    fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-spam", "再来一条广告", "m-2"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(fixture.sidecar.sent_messages().size() == 1);
    REQUIRE(CountOf(fixture.counter_file, "model") == 0);
    // 但别的 sender 照常领提示。
    fixture.EmitAndIngest(MakeDm("in-3", "pe-3", "dm-other", "你好", "m-3"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(fixture.sidecar.sent_messages().size() == 2);
}

// ---------------------------------------------------------------------------
// Q4:手机发文件,处理后把产物发回去
// ---------------------------------------------------------------------------

TEST_CASE("Q4 收件闭环:附件下载落仓,模型请求带文件名与有界预览") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("q4-inbound");
    fixture.scripts = {TextScript("已收到并读过该文件。")};

    // 假下载器:回一段可读文本(带换行,预览可核)。
    fixture.media_download = [](const std::string& url)
        -> std::expected<runtime::ChannelMediaBytes, std::string> {
        if (url.find("multimedia.nt.qq.com") == std::string::npos) {
            return std::unexpected("not_found");
        }
        return runtime::ChannelMediaBytes{std::string("报表第一行\n报表第二行\n合计 42")};
    };
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    // 来信:正文 + 一枚文本附件(QQ 事件映射后的真类型 part)。
    auto event = MakeDm("in-1", "pe-1", "dm-q4", "统计一下这个文件", "m-1");
    channel::ChannelPart attachment;
    attachment.type = channel::ChannelPartType::File;
    attachment.file_name = std::string("../../../report.txt");
    attachment.remote_ref = std::string("https://multimedia.nt.qq.com/d?token=SECTOK");
    attachment.mime_type = std::string("text/plain");
    attachment.size_bytes = 25;
    event.parts.push_back(attachment);
    fixture.EmitAndIngest(event);
    fixture.Tick();
    fixture.TickUntilQuiet();

    // 模型恰跑一次,请求里带净化名与预览(不整塞正文——有界)。
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    REQUIRE(fixture.backend->dumps().size() == 1);
    const std::string& prompt = fixture.backend->dumps()[0];
    REQUIRE(prompt.find("report.txt") != std::string::npos);
    REQUIRE(prompt.find("报表第一行") != std::string::npos);
    REQUIRE(prompt.find("已存档") != std::string::npos);
    // url 的 token 不进模型。
    REQUIRE(prompt.find("SECTOK") == std::string::npos);

    // 原件落在 workspace 身份根的受控仓(read_file 可达,不在渠道状态根)。
    const std::filesystem::path media_root = fixture.identity.identity_root / "channel-media";
    REQUIRE(std::filesystem::exists(media_root / "media.jsonl"));
    bool found_bin = false;
    for (const auto& entry :
         std::filesystem::directory_iterator(media_root / "inbound")) {
        if (entry.path().extension() == ".bin") {
            found_bin = true;
        }
    }
    REQUIRE(found_bin);
    // 回复照常投递。
    REQUIRE(fixture.sidecar.sent_messages().size() == 1);
}

TEST_CASE("Q4 发件闭环:长回复拆段,末段带任务结果文件发回") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("q4-outbound");
    // 超长回复:6000 字节 → 3 文本段 + 1 纯附件末段(msg_type=7 不带
    // content,正文全在前面的段,谁也不吃掉谁)。
    std::string long_reply;
    for (int i = 0; i < 2000; ++i) {
        long_reply += "结";
    }
    fixture.scripts = {TextScript(long_reply)};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-q4b", "整理一下", "m-1"));
    fixture.Tick();
    fixture.TickUntilQuiet();

    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    const auto& sends = fixture.sidecar.sent_messages();
    REQUIRE(sends.size() == 5);  // 四段文本 + 一段附件
    // 前四段纯文本;末段纯附件(冻结正文原件)。
    for (std::size_t i = 0; i + 1 < sends.size(); ++i) {
        const auto& parts = sends[i].params.at("parts");
        REQUIRE(parts.size() == 1);
        REQUIRE(parts[0].at("type") == "text");
    }
    const auto& last_parts = sends.back().params.at("parts");
    REQUIRE(last_parts.size() == 1);
    REQUIRE(last_parts[0].at("type") == "file");
    REQUIRE(last_parts[0].at("mime_type") == "text/plain");
    const std::string attached = last_parts[0].at("local_path").get<std::string>();
    REQUIRE_FALSE(attached.empty());
    REQUIRE(last_parts[0].at("file_name").get<std::string>().find(".txt") !=
            std::string::npos);
    // 附件即冻结正文原件(任务结果文件),盘上可读且内容与全文一致。
    std::error_code ec;
    REQUIRE(std::filesystem::exists(std::filesystem::path(attached), ec));
    std::ifstream stream(std::filesystem::path(attached), std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(stream)),
                              std::istreambuf_iterator<char>());
    REQUIRE(content == long_reply);
    // 全段终态:ingress delivered。
    REQUIRE(fixture.IngressStateNameOf(1) == "delivered");
}

// ---------------------------------------------------------------------------
// QQBot 配对后第二轮静默失败单:P0 刀一(预算误伤)+ P0 刀二(失败用户
// 零感知)。现场病:anthropic wire 真机,第二轮纯文本死信
// turn_failed: context.unestimated_media_or_reasoning,QQ 端"在线却不回"。
// ---------------------------------------------------------------------------

TEST_CASE("P0 刀一:anthropic wire 思考签名历史回传不误伤,三轮纯文本连聊全过") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("think-replay");
    fixture.scripts = {ThinkingTextScript("sig-r1", "第一答:记住了。"),
                       TextScript("第二答。"), TextScript("第三答。")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);
    // 真适配器序列化(anthropic wire):adapter 预算闸真开。
    fixture.backend->wire_json = [](const api::Request& request) {
        return api::anthropic::BuildRequestJson(request);
    };

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "第一问", "m-1"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);

    // 旧病现场:第二轮纯文本带 thinking+signature 历史回传,曾被预算闸拦死。
    fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-a", "第二问", "m-2"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 2);
    REQUIRE(fixture.IngressStateNameOf(2) == "delivered");

    fixture.EmitAndIngest(MakeDm("in-3", "pe-3", "dm-a", "第三问", "m-3"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 3);
    REQUIRE(fixture.IngressStateNameOf(3) == "delivered");
    REQUIRE(fixture.sidecar.sent_messages().size() == 3);
}

TEST_CASE("P0 刀一·保护保留:redacted thinking 历史照旧拒收,不填 0 放行") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("redacted-keep");
    fixture.scripts = {RedactedThinkingTextScript("第一答。"), TextScript("第二答。")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);
    fixture.backend->wire_json = [](const api::Request& request) {
        return api::anthropic::BuildRequestJson(request);
    };

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "第一问", "m-1"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    REQUIRE(fixture.IngressStateNameOf(1) == "delivered");

    fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-a", "第二问", "m-2"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    // 第二轮没发往模型(本地拦下),死信如账。
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    REQUIRE(fixture.IngressStateNameOf(2) == "dead_letter");
}

TEST_CASE("P0 刀二·窗一:预算拒绝 → 死信 + 用户提示(E-CTX1),脱敏且不刷屏") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("turnfail-ctx");
    fixture.scripts = {RedactedThinkingTextScript("第一答。"), TextScript("第二答。")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);
    fixture.backend->wire_json = [](const api::Request& request) {
        return api::anthropic::BuildRequestJson(request);
    };

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "第一问", "m-1"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(fixture.sidecar.sent_messages().size() == 1);

    fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-a", "第二问", "m-2"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(fixture.IngressStateNameOf(2) == "dead_letter");  // 执行失败事实不改写
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);     // 不重跑
    // 用户提示投出:第二条 send 是失败提示,带短编号与下一步,不带内部
    // 报错/字段值(脱敏)。
    REQUIRE(fixture.sidecar.sent_messages().size() == 2);
    const std::string notice =
        fixture.sidecar.sent_messages()[1].params.at("parts")[0].at("text").get<std::string>();
    CHECK(notice.find("[未回复说明]") != std::string::npos);
    CHECK(notice.find("E-CTX1") != std::string::npos);
    CHECK(notice.find("unestimated_media_or_reasoning") == std::string::npos);
    CHECK(notice.find("opaque-redacted-payload") == std::string::npos);
    // 幂等:再 tick 不再入箱不再发(同 sid 同 deliveryId)。
    fixture.TickUntilQuiet();
    fixture.TickUntilQuiet();
    REQUIRE(fixture.sidecar.sent_messages().size() == 2);
    // outbox:提示段有独立来源审计前缀,与正文段分账。
    bool saw_turnfail_ref = false;
    for (const auto& item : fixture.outbox->ListItems()) {
        if (item.source_ref == "turnfail:qqbot:main:2") {
            saw_turnfail_ref = true;
            CHECK(item.state == "sent");
        }
    }
    CHECK(saw_turnfail_ref);
}

TEST_CASE("P0 刀二·窗二:模型调用失败 → 死信 + 用户提示(E-NET1)") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture fixture("turnfail-net");
    // 脚本只备一轮:第二条消息时后端报 Api 错(模拟超时/服务端错)。
    fixture.scripts = {TextScript("第一答。")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "第一问", "m-1"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(fixture.IngressStateNameOf(1) == "delivered");

    fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-a", "第二问", "m-2"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(fixture.IngressStateNameOf(2) == "dead_letter");
    REQUIRE(fixture.sidecar.sent_messages().size() == 2);
    const std::string notice =
        fixture.sidecar.sent_messages()[1].params.at("parts")[0].at("text").get<std::string>();
    CHECK(notice.find("E-NET1") != std::string::npos);
    CHECK(notice.find("script exhausted") == std::string::npos);  // 内部报错不出 QQ
}

TEST_CASE("P0 刀二·窗三:崩溃窗口恢复的死信也提示(E-RVW1),重复恢复不刷屏") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture::Params params;
    params.send_timeout_ms = 300;
    {
        Q2Fixture fixture("turnfail-rvw", params);
        // 不开泵:取件(claim → Running)后"进程死"——claim 后、绑定行前。
        fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "问题", "m-1"));
        REQUIRE(fixture.manager->TakeNextWork("qqbot", "main").has_value());
    }
    {
        Q2Fixture fixture("turnfail-rvw", params, /*rebuild=*/true);
        auto registry = fixture.MakeRegistry();
        REQUIRE(fixture.OpenPump(registry).ok);
        fixture.Tick();
        fixture.TickUntilQuiet();
        REQUIRE(fixture.IngressStateNameOf(1) == "dead_letter");
        REQUIRE(CountOf(fixture.counter_file, "model") == 0);  // 不盲重跑
        // 用户提示:挂起待核对(E-RVW1)。
        REQUIRE(fixture.sidecar.sent_messages().size() == 1);
        const std::string notice =
            fixture.sidecar.sent_messages()[0].params.at("parts")[0].at("text").get<std::string>();
        CHECK(notice.find("E-RVW1") != std::string::npos);
        CHECK(notice.find("needs_review") == std::string::npos);  // 稳定码之外不出内部词
        // 重复恢复不刷屏:死信是终态,幂等入箱。
        fixture.TickUntilQuiet();
        fixture.TickUntilQuiet();
        REQUIRE(fixture.sidecar.sent_messages().size() == 1);
    }
}

TEST_CASE("P0 刀二·窗四:提示回执丢失 → delivery_unknown,两层失败本地可查,不重跑") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture::Params params;
    params.send_timeout_ms = 300;
    Q2Fixture fixture("turnfail-receipt", params);
    fixture.scripts = {RedactedThinkingTextScript("第一答。")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);
    fixture.backend->wire_json = [](const api::Request& request) {
        return api::anthropic::BuildRequestJson(request);
    };

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "第一问", "m-1"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(fixture.IngressStateNameOf(1) == "delivered");
    // 回执从这起丢(Silent:不应答 → 超时 delivery_unknown)。
    fixture.sidecar.set_send_script(FakeChannelSidecar::SendScript::Silent);

    fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-a", "第二问", "m-2"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    // 层一:执行失败(死信);层二:提示投递结果未知(停自动重发)。
    REQUIRE(fixture.IngressStateNameOf(2) == "dead_letter");
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    REQUIRE(fixture.sidecar.sent_messages().size() == 2);
    const std::string notice_delivery = fixture.sidecar.sent_messages()[1].client_id;
    REQUIRE(fixture.sidecar.send_count_for(notice_delivery) == 1);
    bool saw_notice_unknown = false;
    for (const auto& item : fixture.outbox->ListItems()) {
        if (item.source_ref == "turnfail:qqbot:main:2") {
            saw_notice_unknown = true;
            CHECK(item.state == "delivery_unknown");
        }
    }
    CHECK(saw_notice_unknown);
    // 后续 tick:不重跑模型、不重发提示。
    fixture.TickUntilQuiet();
    REQUIRE(CountOf(fixture.counter_file, "model") == 1);
    REQUIRE(fixture.sidecar.send_count_for(notice_delivery) == 1);
}

TEST_CASE("P0 刀二:失败提示的发送身份走 A05 持久分配器,与正文段同账同规则") {
    EnvGuard v3pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    Q2Fixture::Params params;
    params.send_timeout_ms = 300;
    Q2Fixture fixture("turnfail-seq", params);
    // 第一轮长回复(拆两文本段 + 一枚附件末段,同锚 m-1:seq 1、2、3);
    // 第二轮失败 → 提示(锚 m-2:seq 1)。四枚身份同一只 (账号,锚) 分配器,
    // 各自账上可查。
    fixture.scripts = {RedactedThinkingTextScript(std::string(2600, 'a')), TextScript("x")};
    auto registry = fixture.MakeRegistry();
    REQUIRE(fixture.OpenPump(registry).ok);
    fixture.backend->wire_json = [](const api::Request& request) {
        return api::anthropic::BuildRequestJson(request);
    };

    fixture.EmitAndIngest(MakeDm("in-1", "pe-1", "dm-a", "第一问", "m-1"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    // 拆段帽 2000 字节:2600 字节 → 2 文本段;附件(Q4)独占一枚空文本
    // 末段 → 共 3 段 3 发。
    REQUIRE(fixture.sidecar.sent_messages().size() == 3);
    fixture.EmitAndIngest(MakeDm("in-2", "pe-2", "dm-a", "第二问", "m-2"));
    fixture.Tick();
    fixture.TickUntilQuiet();
    REQUIRE(fixture.sidecar.sent_messages().size() == 4);

    // A05 身份账:同锚下按段序递增;提示段按自己的锚从 1 起——同一只
    // 分配器,不旁路(ListItems 是 map 序,按 ordinal 排回段序)。
    std::vector<gateway::ReplyOutboxItem> reply_items;
    std::string notice_delivery_id;
    for (const auto& item : fixture.outbox->ListItems()) {
        if (item.source_ref == "ingress:qqbot:main:1") {
            reply_items.push_back(item);
        } else if (item.source_ref == "turnfail:qqbot:main:2") {
            notice_delivery_id = item.delivery_id;
        }
    }
    REQUIRE(reply_items.size() == 3);
    REQUIRE_FALSE(notice_delivery_id.empty());
    std::sort(reply_items.begin(), reply_items.end(),
              [](const gateway::ReplyOutboxItem& a, const gateway::ReplyOutboxItem& b) {
                  return a.ordinal < b.ordinal;
              });
    for (std::size_t i = 0; i < reply_items.size(); ++i) {
        const auto identity = fixture.outbox->FindLiveChannelSendIdentity(reply_items[i].delivery_id);
        REQUIRE(identity.has_value());
        CHECK(identity->anchor_msg_id == "m-1");
        CHECK(identity->msg_seq == i + 1);
    }
    const auto notice_identity = fixture.outbox->FindLiveChannelSendIdentity(notice_delivery_id);
    REQUIRE(notice_identity.has_value());
    CHECK(notice_identity->anchor_msg_id == "m-2");
    CHECK(notice_identity->msg_seq == 1);
}

TEST_CASE("P0 刀二:MakeTurnFailureNotice 脱敏映射钉字") {
    using runtime::MakeTurnFailureNotice;
    // 预算拒绝 → E-CTX1。
    CHECK(MakeTurnFailureNotice("gateway.turn_failed",
                                "context.unestimated_media_or_reasoning: ...").short_code ==
          "E-CTX1");
    // 窗口超限 → E-CTX2。
    CHECK(MakeTurnFailureNotice("gateway.turn_failed",
                                "context.adapter_input_exceeds_capacity: ...").short_code ==
          "E-CTX2");
    CHECK(MakeTurnFailureNotice("gateway.turn_failed",
                                "上下文预检未通过:输入约 ...").short_code == "E-CTX2");
    // 其余模型失败 → E-NET1;会话/受理/回执/挂起/取消/未知各归各位。
    CHECK(MakeTurnFailureNotice("gateway.turn_failed", "boom").short_code == "E-NET1");
    CHECK(MakeTurnFailureNotice("gateway.launch_failed", "x").short_code == "E-SES1");
    CHECK(MakeTurnFailureNotice("gateway.requires_v3", "x").short_code == "E-SES1");
    CHECK(MakeTurnFailureNotice("gateway.input_rejected", "x").short_code == "E-INT1");
    CHECK(MakeTurnFailureNotice("selection.artifact_failed", "x").short_code == "E-RPL1");
    CHECK(MakeTurnFailureNotice("gateway.reply_unavailable", "x").short_code == "E-RPL1");
    CHECK(MakeTurnFailureNotice("needs_review", "needs_review:v3_stream_not_found").short_code ==
          "E-RVW1");
    CHECK(MakeTurnFailureNotice("gateway.turn_failed", "user cancelled").short_code == "E-CNL1");
    CHECK(MakeTurnFailureNotice("whatever", "x").short_code == "E-OTH1");
    // 脱敏:内部报错(路径/密钥样)一个字不进文案。
    const auto notice = MakeTurnFailureNotice(
        "gateway.turn_failed", "connect to https://secret.example/keys?token=ABC failed");
    CHECK(notice.text.find("secret.example") == std::string::npos);
    CHECK(notice.text.find("ABC") == std::string::npos);
}
