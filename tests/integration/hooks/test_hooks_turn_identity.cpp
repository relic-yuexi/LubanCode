// Hook 回合身份合同(审计单 P0:Hook 的 turn_id 另造一本账):
//   1. 一轮之内 UserPromptSubmit / PreToolUse / PermissionRequest / PostToolUse /
//      Stop 共用同一枚 canonical turn id,且与事件流(TurnStarted)那枚等值
//      ——Hook 记录与 runtime event 可直接按 turn_id 联表。
//   2. 每次发射各有不同的 hook_run_id;hook_run_id 不得与 turn_id 相同,
//      turn_id 字段不得是 hookrun_* 代班号。
//   3. 连跑两轮,第二轮首个 prompt Hook 不得继承第一轮的 id。
//   4. prompt Hook 阻断景:turn id 已预留(payload 里看得见),但未发
//      TurnStarted、模型零请求,以 blocked 收口——不许为抢 ID 先发一个
//      永不收尾的 TurnStarted。
//   5. one-shot 无宿主 turn id:RunTurn 自己从发号局 mint,五类 Hook 仍共用
//      一枚 turn-<n>,不吃 hookrun_* 代班号。
//
// 手法:FakeBackend 按脚本吐事件(不碰网络),五类 Hook 全配真进程 handler,
// stdin JSON 逐条落日志文件,跑完解析对账。旧实现(2026-09-03 前)在这册
// 上全红:prompt Hook 吃 "unassigned"/上一轮遗留,工具类 Hook 吃 hookrun_*
// 代班号,联表必裂。
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/turn_runner.hpp"
#include "cli/console_input.hpp"
#include "cli/context_tracker.hpp"
#include "cli/theme.hpp"
#include "cli/transcript.hpp"
#include "hooks/dispatcher.hpp"
#include "hooks/hash.hpp"
#include "hooks/loader.hpp"
#include "runtime/event.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "tools/registry.hpp"
#include "tools/todo_tool.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// ---- 假后端:按脚本吐事件(同 test_loop 的手法) ----------------------------

class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::size_t request_count = 0;

    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        const std::size_t idx = request_count++;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

// 需要过确认门的假工具:Confirm 档下裁定为 Ask,PermissionRequest 钩子才有
// 机会表态。
class GatedTool : public tools::Tool {
public:
    std::string name() const override { return "gated_tool"; }
    std::string description() const override { return "gated tool for turn identity test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return true; }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"gated ok", false}; }
};

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, "gated_tool"},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

// ---- Hook 日志:stdin JSON 逐条追加进文件 -----------------------------------

const std::filesystem::path& HookLogDir() {
    static const std::filesystem::path dir = [] {
        const auto base = std::filesystem::temp_directory_path() / "lubancode-hook-turnid";
        std::filesystem::create_directories(base);
        return base;
    }();
    return dir;
}

#ifdef _WIN32
// cmd 脚本:先把 stdin 全量追加进日志,再按需吐一行 JSON / 退出码。
std::string WriteHookScript(const std::string& name, const std::filesystem::path& log,
                            const std::string& tail) {
    const std::filesystem::path file = HookLogDir() / (name + ".cmd");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "@echo off\r\n"
        << "@more >> \"" << log.string() << "\"\r\n"
        << tail << "\r\n";
    return "cmd /d /s /c \"\"" + std::string(reinterpret_cast<const char*>(file.u8string().data()),
                                              file.u8string().size()) +
           "\"\"";
}
#else
std::string WriteHookScript(const std::string& name, const std::filesystem::path& log,
                            const std::string& tail) {
    const std::filesystem::path file = HookLogDir() / (name + ".sh");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    // stdin 的 payload JSON 不带尾换行,cat 又是字节直通(Windows 的 more
    // 会自补 CRLF,POSIX 没这待遇)——逐条记账粘成一行,ParseHookLog 就
    // 解不动了。cat 后补一枚换行,每条 payload 独占一行。
    out << "#!/bin/sh\ncat >> \"" << log.string() << "\"\necho >> \"" << log.string() << "\"\n"
        << tail << "\n";
    return "sh \"" + file.string() + "\"";
}
#endif

// echo 一行 JSON:sh 的 quote removal 会把参数里的双引号当引号语法吃掉,
// 吐出来的就不是 JSON 了(姊妹册 test_hooks_dispatcher 的同款坑,那里有
// 同名帮手);POSIX 用单引号裹整串保真(这些 JSON 里没有单引号),cmd
// 不吃双引号、原样透传。
std::string EchoJson(const std::string& json) {
#ifdef _WIN32
    return "echo " + json;
#else
    return "echo '" + json + "'";
#endif
}

struct HookLine {
    std::string event;
    std::string turn_id;
    std::string hook_run_id;
};

std::vector<HookLine> ParseHookLog(const std::filesystem::path& log) {
    std::vector<HookLine> lines;
    std::ifstream in(log);
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }
        if (raw.empty()) {
            continue;
        }
        const nlohmann::json parsed = nlohmann::json::parse(raw);
        lines.push_back({parsed.value("hook_event_name", std::string()),
                         parsed.value("turn_id", std::string()),
                         parsed.value("hook_run_id", std::string())});
    }
    return lines;
}

hooks::HookDefinition MakeDefinition(hooks::HookEvent event, const std::string& command) {
    hooks::HookDefinition def;
    def.event = event;
    def.source_kind = hooks::HookSourceKind::User;
    def.source_path = "test://user-config";
    def.source_label = "user test://user-config";
    def.matcher = "*";
    def.handler.command = command;
    def.handler.timeout_ms = 15000;
    def.handler.failure_policy = "warn";
    def.definition_hash = hooks::ComputeDefinitionHash(def.handler);
    def.definition_hash_short = hooks::DefinitionHashShort(def.definition_hash);
    def.trusted = true;
    return def;
}

hooks::HookDispatcher MakeDispatcher(std::vector<hooks::HookDefinition> defs) {
    hooks::LoadedHooks loaded;
    loaded.definitions = std::move(defs);
    hooks::HookTrustStore trust = hooks::HookTrustStore::Load(std::nullopt).first;
    hooks::HookContext ctx;
    ctx.session_id = "turnid-test-session";
    ctx.turn_id = "unassigned";  // 与 app::SetupHookRuntime 的启动态同款
    ctx.cwd = "/test";
    ctx.permission_mode = "default";
    hooks::HookDispatcher dispatcher;
    dispatcher.Configure(std::move(loaded), std::move(trust), std::move(ctx));
    return dispatcher;
}

// 五类事件的 handler 脚本:纯记账(log)、放行 PermissionRequest(log+allow)、
// Stop 拉闸续跑(log+continue:false)。PreToolUse 只记账不表态,确认裁定才会
// 落到 Ask,PermissionRequest 钩子才真有机会说话。
std::vector<hooks::HookDefinition> MakeFiveEventDefinitions(const std::filesystem::path& log,
                                                            const std::string& tag) {
    const std::string log_cmd = WriteHookScript("log_" + tag, log, "");
    const std::string allow_cmd = WriteHookScript(
        "allow_" + tag, log,
        EchoJson(
            R"({"continue":true,"hookSpecificOutput":{"hookEventName":"PermissionRequest","permissionDecision":"allow"}})"));
    const std::string stop_cmd =
        WriteHookScript("stop_" + tag, log, EchoJson(R"({"continue":false})"));
    return {
        MakeDefinition(hooks::HookEvent::UserPromptSubmit, log_cmd),
        MakeDefinition(hooks::HookEvent::PreToolUse, log_cmd),
        MakeDefinition(hooks::HookEvent::PermissionRequest, allow_cmd),
        MakeDefinition(hooks::HookEvent::PostToolUse, log_cmd),
        MakeDefinition(hooks::HookEvent::Stop, stop_cmd),
    };
}

// ---- 回合装配:真 RunTurn + 假后端 + 五类 Hook ------------------------------

struct TurnRig {
    FakeBackend backend;
    tools::ToolRegistry registry;
    std::unique_ptr<agent::Agent> loop;
    hooks::HookDispatcher dispatcher;
    cli::ContextTracker tracker{100000};
    std::vector<cli::TranscriptItem> transcript;
    std::shared_ptr<tools::TodoListState> todo = std::make_shared<tools::TodoListState>();
    cli::Theme theme{};
    std::atomic<bool> expanded{false};

    // 事件流录音:数 TurnStarted、记 turn_id。
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter;
    std::vector<runtime::ServerEvent> events;

    explicit TurnRig(std::vector<hooks::HookDefinition> defs)
        : dispatcher(MakeDispatcher(std::move(defs))), adapter("turnid-test", ids) {
        registry.Register(std::make_unique<GatedTool>());
        loop = std::make_unique<agent::Agent>(
            backend, registry,
            agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
        adapter.Attach([this](const runtime::ServerEvent& event) { events.push_back(event); });
    }

    app::TurnContext MakeContext(const std::string& input) {
        app::TurnContext ctx;
        ctx.loop = loop.get();
        ctx.user_input = input;
        ctx.always_allowed_tools = &no_allow;
        ctx.theme = theme;
        ctx.context_tracker = &tracker;
        ctx.registry = &registry;
        ctx.hook_dispatcher = &dispatcher;
        ctx.is_console = false;
        ctx.transcript = &transcript;
        ctx.todo_state = todo;
        ctx.transcript_expanded = &expanded;
        ctx.silent = true;
        ctx.turn_events = &adapter;
        return ctx;
    }

    std::size_t TurnStartedCount() const {
        return static_cast<std::size_t>(std::count_if(
            events.begin(), events.end(),
            [](const runtime::ServerEvent& e) { return e.kind == runtime::ServerEventKind::TurnStarted; }));
    }

    std::string TurnStartedId() const {
        for (const auto& e : events) {
            if (e.kind == runtime::ServerEventKind::TurnStarted) {
                return e.turn_id;
            }
        }
        return std::string();
    }

private:
    std::set<std::string> no_allow;
};

std::filesystem::path FreshLog(const std::string& name) {
    const std::filesystem::path log = HookLogDir() / (name + ".log");
    std::ofstream(log, std::ios::trunc).close();
    return log;
}

}  // namespace

// ---------------------------------------------------------------------------
// 合同 1+2:一轮之内五类 Hook 共用 canonical turn id;hook_run_id 各异。
// ---------------------------------------------------------------------------

TEST_CASE("一轮内五类 Hook 共用同一 turn_id,且与事件流等值联表") {
    cli::SetConfirmMode(cli::ConfirmMode::Confirm);
    const std::filesystem::path log = FreshLog("five_events");
    TurnRig rig(MakeFiveEventDefinitions(log, "five"));

    const std::string canonical = runtime::ProcessIdAuthority().NextTurnId();

    rig.backend.scripts = {
        ToolUseScript("toolu_1"),           // 第一请求:带一枚要过闸的工具
        TextScript("first round done"),     // 工具回来后的收尾请求
        TextScript("stop continuation"),    // Stop 钩子拉闸后的续跑轮
    };
    app::TurnContext ctx = rig.MakeContext("turn identity probe");
    ctx.turn_id_for_trace = canonical;
    const app::RunTurnResult result = app::RunTurn(std::move(ctx));
    CHECK(result.status == 0);

    const std::vector<HookLine> lines = ParseHookLog(log);
    REQUIRE(lines.size() >= 6);  // 五类事件,Stop 因续跑发射两次

    // 五类事件都在场(Stop 至少两次)。
    const auto count_event = [&lines](const char* name) {
        return static_cast<std::size_t>(
            std::count_if(lines.begin(), lines.end(),
                          [name](const HookLine& l) { return l.event == name; }));
    };
    CHECK(count_event("UserPromptSubmit") == 1);
    CHECK(count_event("PreToolUse") == 1);
    CHECK(count_event("PermissionRequest") == 1);
    CHECK(count_event("PostToolUse") == 1);
    CHECK(count_event("Stop") >= 2);  // 第一次拉闸续跑,第二次 stop_hook_active 收口

    // 合同 1:每一条的 turn_id 都是同一枚 canonical(与事件流那枚等值)。
    for (const HookLine& line : lines) {
        INFO("event=", line.event, " turn_id=", line.turn_id);
        CHECK(line.turn_id == canonical);
    }
    CHECK(rig.TurnStartedCount() == 1);
    CHECK(rig.TurnStartedId() == canonical);  // 联表钩子:payload.turn_id == event.turn_id

    // 合同 2:hook_run_id 每次发射各不同,且不得与 turn_id 相同;turn_id
    // 不得长成 hookrun_* 代班号。
    std::vector<std::string> run_ids;
    for (const HookLine& line : lines) {
        CHECK(line.hook_run_id != line.turn_id);
        CHECK(line.turn_id.rfind("hookrun_", 0) != 0);
        CHECK(line.hook_run_id.rfind("hookrun_", 0) == 0);
        run_ids.push_back(line.hook_run_id);
    }
    std::sort(run_ids.begin(), run_ids.end());
    CHECK(std::adjacent_find(run_ids.begin(), run_ids.end()) == run_ids.end());  // 无重复
}

// ---------------------------------------------------------------------------
// 合同 3:连跑两轮,第二轮首个 prompt Hook 不继承第一轮 id。
// ---------------------------------------------------------------------------

TEST_CASE("连跑两轮:第二轮 UserPromptSubmit 换新 turn_id,不继承上一轮") {
    cli::SetConfirmMode(cli::ConfirmMode::Confirm);
    const std::filesystem::path log = FreshLog("two_turns");
    TurnRig rig(MakeFiveEventDefinitions(log, "two"));

    const std::string turn1 = runtime::ProcessIdAuthority().NextTurnId();
    rig.backend.scripts = {
        ToolUseScript("toolu_a"),
        TextScript("round one"),
        TextScript("stop continuation one"),
    };
    {
        app::TurnContext ctx = rig.MakeContext("first probe");
        ctx.turn_id_for_trace = turn1;
        CHECK(app::RunTurn(std::move(ctx)).status == 0);
    }

    const std::string turn2 = runtime::ProcessIdAuthority().NextTurnId();
    CHECK(turn2 != turn1);
    rig.backend.request_count = 0;  // 脚本按下标取,第二轮从头数
    rig.backend.scripts = {
        ToolUseScript("toolu_b"),
        TextScript("round two"),
        TextScript("stop continuation two"),
    };
    {
        app::TurnContext ctx = rig.MakeContext("second probe");
        ctx.turn_id_for_trace = turn2;
        CHECK(app::RunTurn(std::move(ctx)).status == 0);
    }

    const std::vector<HookLine> lines = ParseHookLog(log);
    REQUIRE(lines.size() >= 2);
    // 两轮各自的 UserPromptSubmit 都在场,各挂各的号。
    std::vector<std::string> prompt_ids;
    for (const HookLine& line : lines) {
        if (line.event == "UserPromptSubmit") {
            prompt_ids.push_back(line.turn_id);
        }
    }
    REQUIRE(prompt_ids.size() == 2);
    CHECK(prompt_ids[0] == turn1);
    CHECK(prompt_ids[1] == turn2);  // 旧实现:第二轮 prompt Hook 吃第一轮的 hookrun_* 遗留
    CHECK(prompt_ids[0] != prompt_ids[1]);
}

// ---------------------------------------------------------------------------
// 合同 4:prompt Hook 阻断——预留 turn id、未发 TurnStarted、零模型请求。
// ---------------------------------------------------------------------------

TEST_CASE("prompt Hook 阻断:预留 turn id,不发 TurnStarted,模型零请求") {
    cli::SetConfirmMode(cli::ConfirmMode::Confirm);
    const std::filesystem::path log = FreshLog("blocked");
    // 只配一只 UserPromptSubmit 拦截钩子:记账后 exit 2。
    const std::string block_cmd = WriteHookScript("block_prompt", log, "exit 2");
    TurnRig rig({MakeDefinition(hooks::HookEvent::UserPromptSubmit, block_cmd)});

    const std::string canonical = runtime::ProcessIdAuthority().NextTurnId();
    rig.backend.scripts = {};  // 一个脚本都不给:有请求就当场露馅

    app::TurnContext blocked_ctx = rig.MakeContext("blocked probe");
    blocked_ctx.turn_id_for_trace = canonical;
    const app::RunTurnResult result = app::RunTurn(std::move(blocked_ctx));

    CHECK(result.status == 0);           // 阻断不算错误轮
    CHECK(rig.backend.request_count == 0);  // 模型零请求
    CHECK(rig.TurnStartedCount() == 0);  // 未发 TurnStarted(生命周期:不以抢 ID 换开轮)
    CHECK(rig.events.empty());           // 整条事件流一封未发

    const std::vector<HookLine> lines = ParseHookLog(log);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].event == "UserPromptSubmit");
    CHECK(lines[0].turn_id == canonical);  // 预留的 turn id 在阻断钩子的 payload 里看得见
}

// ---------------------------------------------------------------------------
// 合同 5:one-shot 无宿主 turn id——RunTurn 自 mint,五类 Hook 仍共用一枚。
// ---------------------------------------------------------------------------

TEST_CASE("one-shot 无宿主 turn id:自 mint turn-<n>,五类 Hook 共用") {
    cli::SetConfirmMode(cli::ConfirmMode::Confirm);
    const std::filesystem::path log = FreshLog("oneshot");
    TurnRig rig(MakeFiveEventDefinitions(log, "oneshot"));

    rig.backend.scripts = {
        ToolUseScript("toolu_c"),
        TextScript("oneshot done"),
        TextScript("oneshot continuation"),
    };
    app::TurnContext ctx = rig.MakeContext("oneshot probe");
    // turn_id_for_trace 留空 = 无宿主(one-shot/单测)路径;turn_events 也留
    // 空,RunTurn 就地起本地适配器,同一套接线。
    ctx.turn_events = nullptr;
    const app::RunTurnResult result = app::RunTurn(std::move(ctx));
    CHECK(result.status == 0);

    const std::vector<HookLine> lines = ParseHookLog(log);
    REQUIRE(lines.size() >= 6);
    const std::string minted = lines[0].turn_id;
    CHECK(minted.rfind("turn-", 0) == 0);  // 发号局的 turn 域,不是 hookrun_* 代班
    for (const HookLine& line : lines) {
        INFO("event=", line.event, " turn_id=", line.turn_id);
        CHECK(line.turn_id == minted);  // 五类共用同一枚
        CHECK(line.hook_run_id != line.turn_id);
    }
}
