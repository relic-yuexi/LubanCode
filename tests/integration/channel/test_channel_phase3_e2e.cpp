// 多渠道消息接入单阶段 3 验收(假 sidecar 回路,不连真 IM):
//   - 两只假 conversation 同时来信,各进自己的 session(router 的
//     session_key 分场 + host 的引擎缓存各建一只);
//   - 同一 conversation 保序(per-session FIFO + 单飞);
//   - 工具权限不越过 binding:needs_confirm 工具不在 allowlist ->
//     fail closed 拒绝,tool_result 带渠道拒绝文案,工具零执行;
//   - provenance 落 session JSONL,档案里查得回 sender/conversation;
//   - pairing 链路:未知 sender 挂 pending,/channel pairing approve 后放行。
//
// 装配:FakeChannelSidecar 喂 manager;TakeNextWork 拿活;ChannelRouter
// 已在 manager 准入时判过;ChannelSessionHost 装真 AgentChannelEngine
//(真 SessionRuntime + 真 Agent + AgentLoop::Run),模型用脚本假后端。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "channel/manager.hpp"
#include "fake_channel_sidecar.hpp"
#include "runtime/agent_channel_engine.hpp"
#include "runtime/channel_session_host.hpp"
#include "runtime/turn_ingress.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;
using lubancode::test_support::FakeChannelSidecar;

namespace {

// ---- 假后端:按脚本吐流式事件(记下收到的请求,供工具权限断言) -----------
class ScriptBackend final : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured;

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured.push_back(request);
        if (captured.size() > scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "脚本用完了", 0});
        }
        for (const auto& event : scripts[captured.size() - 1]) {
            on_event(event);
        }
        return {};
    }
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolThenTextScript(const std::string& tool_id,
                                                 const std::string& tool_name,
                                                 const std::string& text) {
    return {
        api::MessageStart{"msg", "test-model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
        api::MessageStart{"msg", "test-model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

// ---- 假工具:needs_confirm 可控,执行记数 ---------------------------------
class ProbeTool final : public tools::Tool {
public:
    ProbeTool(std::string name, bool confirm, std::atomic<int>* calls)
        : name_(std::move(name)), confirm_(confirm), calls_(calls) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "probe tool"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return confirm_; }
    tools::Tool::Result execute(const nlohmann::json& /*input*/) override {
        if (calls_ != nullptr) ++*calls_;
        return tools::Tool::Result{"probe ok", false};
    }

private:
    std::string name_;
    bool confirm_;
    std::atomic<int>* calls_;
};

// ---- 假 transport(与 unit/channel 同款) ---------------------------------
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

std::filesystem::path MakeTempDir(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode_ch3_" + std::string(tag) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

channel::ChannelInboundEvent MakeDm(const std::string& delivery_id,
                                    const std::string& provider_event_id,
                                    const std::string& conversation_id, const std::string& text) {
    channel::ChannelInboundEvent event;
    event.delivery_id = delivery_id;
    event.provider_event_id = provider_event_id;
    event.channel_id = "qqbot";
    event.account_id = "main";
    event.conversation.kind = channel::ConversationKind::Direct;
    event.conversation.id = conversation_id;
    event.sender.id = "owner-openid";
    event.sender.display_name = "老板";
    event.message_id = "m-" + delivery_id;
    channel::ChannelPart part;
    part.type = channel::ChannelPartType::Text;
    part.text = text;
    event.parts.push_back(part);
    return event;
}

// 装配一台最小 Gateway:manager(假 sidecar)+ host(真引擎工厂)。
struct MiniGateway {
    std::filesystem::path root;
    FakeChannelSidecar sidecar;
    FakeTransport transport{sidecar};
    ScriptBackend backend;
    tools::ToolRegistry registry;
    std::unique_ptr<channel::ChannelManager> manager;
    runtime::ChannelSessionHost host;

    explicit MiniGateway(const char* tag)
        : root(MakeTempDir(tag)), host(runtime::ChannelSessionHost::Options{}) {
        channel::ChannelManagerOptions options;
        options.state_root = root / "state";
        options.now_ms = [] { return 1724700000000; };
        options.alive_checker = [](unsigned long) { return true; };
        manager = std::make_unique<channel::ChannelManager>(options);

        channel::ChannelAccountUserConfig config;
        config.enabled = true;
        config.transport = "websocket";
        config.secret_env = "QQBOT_SECRET";
        config.dm_policy = channel::DmPolicy::Allowlist;
        config.allow_from = {"owner-openid"};
        manager->AddAccount("qqbot", "main", config, &transport);
        REQUIRE_FALSE(manager->StartAccount("qqbot", "main").has_value());
        manager->Pump("qqbot", "main");
        manager->Pump("qqbot", "main");
    }

    void InstallHost(channel::ToolRoutePolicy tools) {
        // 引擎工厂:每场 session 一只真引擎(真 SessionRuntime 落盘 + 真 Agent)。
        host.SetEngineFactory([this, tools](const std::string& /*session_key*/) {
            runtime::AgentChannelEngine::Options engine_options;
            engine_options.sessions_dir = (root / "sessions").string();
            engine_options.workspaces_dir = (root / "workspaces").string();  // P0-2:会话账根
            engine_options.wire_name = "anthropic";
            engine_options.model = "test-model";
            engine_options.cwd = root.string();
            engine_options.tools = tools;
            agent::AgentProfile profile;
            profile.provider = "test";
            profile.request.model = "test-model";
            return std::make_unique<runtime::AgentChannelEngine>(backend, registry,
                                                                 std::move(profile),
                                                                 std::move(engine_options));
        });
    }

    // 给假后端排一份纯文本回复脚本(一轮一份,按请求次序消费)。
    void QueueTextReply(const std::string& text) { backend.scripts.push_back(TextScript(text)); }

    // 会话存档目录里落了几份 .jsonl(引擎建档数)。
    // P0-2:会话账住 workspaces/<key>/sessions/<id>/main.jsonl(数有几场)。
    std::vector<std::string> SessionFiles() const {
        std::vector<std::string> files;
        std::error_code ec;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root / "workspaces", ec)) {
            if (entry.is_regular_file() && entry.path().filename() == "main.jsonl") {
                files.push_back(entry.path().string());
            }
        }
        return files;
    }

    // sidecar 来信 -> manager 入账 -> 取活 -> 折 ingress -> 泵一轮。
    std::optional<runtime::ChannelSessionHost::TurnOutcome> DeliverOne(
        const channel::ChannelInboundEvent& event) {
        sidecar.EmitInboundEvent(event);
        manager->Pump("qqbot", "main");  // 发出站帧 + 收 sidecar 字节(入账/ack)
        auto work = manager->TakeNextWork("qqbot", "main");
        if (!work.has_value()) return std::nullopt;
        REQUIRE(work->route.status == channel::RouteDecision::Status::Admitted);
        auto ingress = runtime::MakeChannelTurnIngress(
            work->event, work->route.provenance, work->route.session_key,
            work->route.memory.user_memory || work->route.memory.project_memory);
        host.Submit(std::move(ingress));
        return host.PumpOne();
    }
};

}  // namespace

TEST_CASE("验收一:两只假 conversation 同时来信,各进自己的 session") {
    MiniGateway gw("two_sessions");
    gw.InstallHost(channel::ToolRoutePolicy{});
    gw.QueueTextReply("收到,第一件事办好了");
    gw.QueueTextReply("收到,第二件事办好了");

    // 两位 owner 各自发一条 DM(同一账号,两只 conversation)。
    auto first = gw.DeliverOne(MakeDm("d1", "pe1", "dm-alice", "帮我看看第一件事"));
    REQUIRE(first.has_value());
    CHECK(first->ok);
    CHECK(first->session_key == "channel:qqbot:main:direct:dm-alice");
    CHECK(first->reply_text == "收到,第一件事办好了");

    auto second = gw.DeliverOne(MakeDm("d2", "pe2", "dm-bob", "第二件事"));
    REQUIRE(second.has_value());
    CHECK(second->ok);
    CHECK(second->session_key == "channel:qqbot:main:direct:dm-bob");
    CHECK(second->reply_text == "收到,第二件事办好了");

    // 两场会话两只引擎,各自存档;history 互不掺。
    CHECK(gw.host.engine_count() == 2);
    const auto files = gw.SessionFiles();
    REQUIRE(files.size() == 2);
}

TEST_CASE("验收二:同一 conversation 保序,消息一条条排队跑") {
    MiniGateway gw("fifo");
    gw.InstallHost(channel::ToolRoutePolicy{});
    for (int i = 1; i <= 3; ++i) {
        gw.QueueTextReply("回 " + std::to_string(i));
    }

    // 同一 conversation 三条来信:先全部入账,再统一泵。
    std::vector<std::string> replies;
    for (int i = 1; i <= 3; ++i) {
        auto outcome = gw.DeliverOne(MakeDm("d" + std::to_string(i), "pe" + std::to_string(i),
                                           "dm-owner", "第 " + std::to_string(i) + " 条"));
        REQUIRE(outcome.has_value());
        REQUIRE(outcome->ok);
        replies.push_back(outcome->reply_text);
        CHECK(outcome->session_key == "channel:qqbot:main:direct:dm-owner");
    }
    CHECK(replies[0] == "回 1");
    CHECK(replies[1] == "回 2");
    CHECK(replies[2] == "回 3");
    // 一场 session 跑三轮(引擎缓存一只)。
    CHECK(gw.host.engine_count() == 1);
    REQUIRE(gw.SessionFiles().size() == 1);

    // 假后端收到的三轮请求,上下文逐轮增长(同一 conversation 连续)。
    REQUIRE(gw.backend.captured.size() == 3);
    std::size_t last_user_texts = 0;
    for (const auto& request : gw.backend.captured) {
        std::size_t user_texts = 0;
        for (const auto& message : request.messages) {
            if (message.role == api::Role::User) ++user_texts;
        }
        CHECK(user_texts > last_user_texts);
        last_user_texts = user_texts;
    }
}

TEST_CASE("验收三:工具权限不越过 binding,confirm fail closed") {
    std::atomic<int> dangerous_calls{0};
    std::atomic<int> safe_calls{0};

    SUBCASE("binding 名单外:拦在暴露面,模型看不见、调了给稳定拒绝") {
        MiniGateway gw("fail_closed_binding");
        gw.registry.Register(std::make_unique<ProbeTool>("run_shell", true, &dangerous_calls));
        gw.registry.Register(std::make_unique<ProbeTool>("read_file", true, &safe_calls));

        // binding:只许 read_file,deny run_shell。
        channel::ToolRoutePolicy tools;
        tools.allow = {"read_file"};
        tools.deny = {"run_shell"};
        gw.InstallHost(tools);

        gw.backend.scripts.push_back(ToolThenTextScript("t1", "run_shell", "算了,不用了"));
        gw.backend.scripts.push_back(TextScript("好吧"));

        auto outcome = gw.DeliverOne(MakeDm("d1", "pe1", "dm-owner", "删掉那个文件"));
        REQUIRE(outcome.has_value());
        CHECK(outcome->ok);
        CHECK(dangerous_calls.load() == 0);  // 危险工具零执行

        // 模型收到的 tool_result 带渠道层的稳定拒绝(不是"用户拒绝")。
        REQUIRE(gw.backend.captured.size() == 2);
        const auto& retry_request = gw.backend.captured[1];
        bool saw_denial = false;
        for (const auto& message : retry_request.messages) {
            for (const auto& block : message.content) {
                if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                    if (result->tool_use_id == "t1") {
                        CHECK(result->is_error);
                        CHECK(result->content.find("渠道 binding") != std::string::npos);
                        saw_denial = true;
                    }
                }
            }
        }
        CHECK(saw_denial);

        // allowlist 内的 read_file:needs_confirm 也放行执行。
        gw.backend.scripts.push_back(ToolThenTextScript("t2", "read_file", "读完了"));
        gw.backend.scripts.push_back(TextScript("内容在这"));
        auto second = gw.DeliverOne(MakeDm("d2", "pe2", "dm-owner", "看看配置"));
        REQUIRE(second.has_value());
        CHECK(second->ok);
        CHECK(safe_calls.load() == 1);
    }

    SUBCASE("无 binding:needs_confirm 工具在确认口 fail closed") {
        MiniGateway gw("fail_closed_confirm");
        gw.registry.Register(std::make_unique<ProbeTool>("run_shell", true, &dangerous_calls));

        // 没有 binding 策略:工具暴露给模型;needs_confirm 的调用在
        // on_tool_confirm 裁定——渠道会话没有审批渠道,一律拒绝。
        gw.InstallHost(channel::ToolRoutePolicy{});
        gw.backend.scripts.push_back(ToolThenTextScript("t1", "run_shell", "好吧"));
        gw.backend.scripts.push_back(TextScript("干不了"));

        auto outcome = gw.DeliverOne(MakeDm("d1", "pe1", "dm-owner", "删掉那个文件"));
        REQUIRE(outcome.has_value());
        CHECK(outcome->ok);
        CHECK(dangerous_calls.load() == 0);

        REQUIRE(gw.backend.captured.size() == 2);
        bool saw_denial = false;
        for (const auto& message : gw.backend.captured[1].messages) {
            for (const auto& block : message.content) {
                if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                    if (result->tool_use_id == "t1") {
                        CHECK(result->is_error);
                        // 拒绝文案点名"没有审批渠道",不冒充"用户拒绝"。
                        CHECK(result->content.find("渠道会话") != std::string::npos);
                        CHECK(result->content.find("用户拒绝") == std::string::npos);
                        saw_denial = true;
                    }
                }
            }
        }
        CHECK(saw_denial);
    }
}

TEST_CASE("渠道轮的真账落 Journal:P0-2 起进 workspaces,provenance 投影另批接") {
    MiniGateway gw("provenance");
    gw.InstallHost(channel::ToolRoutePolicy{});
    gw.QueueTextReply("从渠道来的");

    auto outcome = gw.DeliverOne(MakeDm("d1", "pe1", "dm-owner", "从哪儿来的"));
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->ok);
    const auto files = gw.SessionFiles();
    REQUIRE(files.size() == 1);

    // 真账是 main.jsonl:来信与回话都在。provenance 字段进 Journal 的
    // typed 投影是 channel 线后续批次的活(轮末补抄路已停,不伪造账)。
    std::ifstream in(files[0], std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(content.find("从哪儿来的") != std::string::npos);
    CHECK(content.find("从渠道来的") != std::string::npos);
}

TEST_CASE("准入与 pairing:allowlist 外的 sender 不进 Agent,命令口子的账可查") {
    MiniGateway gw("pairing_flow");
    // 账号是 allowlist(owner-openid 在名单):陌生 sender 的 DM 直接拒,
    // 不建 session、不进 inbox、不跑模型。
    gw.QueueTextReply("不该跑到这条");
    channel::ChannelInboundEvent stranger = MakeDm("d1", "pe1", "dm-stranger", "你好");
    stranger.sender.id = "stranger-openid";
    gw.sidecar.EmitInboundEvent(stranger);
    gw.manager->Pump("qqbot", "main");
    CHECK_FALSE(gw.manager->TakeNextWork("qqbot", "main").has_value());
    CHECK(gw.backend.captured.empty());  // 模型一次都没被调

    // pairing 口子(阶段 3 命令面背后的账):allowlist 不产 pairing 挂账;
    // approve 不认的 code 明报错。
    CHECK(gw.manager->PendingPairings("qqbot", "main").empty());
    std::string error;
    CHECK_FALSE(gw.manager->ApprovePairing("qqbot", "main", "DEADBEEF", &error).has_value());
    CHECK_FALSE(error.empty());
}
