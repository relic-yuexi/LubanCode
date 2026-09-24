// T16(V3-ADD-02,B1 批,SessionV3 旧设计清理单):默认-v3 冒烟。
// 翻默认(v0.26.250)后产品语义 = 未设 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS
// 时新会话写 v3;ctest 分账清单(tests/CMakeLists.txt 的
// LUBANCODE_TESTS_V3_DEFAULT_BOOKS)不给本册注入格式变量,环境与生产一致。
// 本册钉"完全不设变量"时的真实入口链:
//   - TrajectorySessionLedger::Open(CLI/AppServer 建场共用路)开的是 v3 场;
//   - 一轮 turn 的账面形状(user/assistant 入链、prepared 落账);
//   - compact 管理入口的分派判据(v3_main_writer 非空)在默认态成立;
//   - CloseSession 封口(session.ended);
//   - SessionManager::LaunchSession(建场的更底层)同默认开 v3;
//   - 子代理入口(SpawnSubagent,CLI/AppServer 共用)默认场下子账同 v3
//     (T16 勾二补齐,原仅注释声明"跟随父场");
//   - CLI /compact 管理入口(RunCompactCommand)默认场下分派 v3 分支干跑
//     全链——零模型零写入(T16 勾二补齐;真压缩链由 v3_compact_runtime/
//     v3_compact_gates 册守,冒烟不重复)。
#include <doctest/doctest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <expected>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/commands/session_commands.hpp"
#include "cli/theme.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
#include "tools/registry.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
using namespace lubancode;
using namespace lubancode::trajectory;
using lubancode::runtime::TrajectorySessionLedger;
using lubancode::runtime::TrajectoryTurnBridge;

namespace {

// 摘掉格式变量(ctest 的 ENVIRONMENT 注入、CI 父环境的意外设置都清掉),
// 析构还原为未设——钉"完全不设"的产品默认态。
struct EnvUnset {
    explicit EnvUnset(const char* name) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    ~EnvUnset() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

std::filesystem::path FreshRoot(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-v3-default-smoke-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

TrajectorySessionLedger::Options LedgerOptions(const std::filesystem::path& root) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "ws");
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.251-test";
    options.v3_system_content = "你是 LubanCode,读写跑都走工具。";
    return options;
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantText(const std::string& text) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Request MakeRequest(const std::string& system, const std::vector<api::Message>& messages) {
    api::Request request;
    request.model = "kimi-k2.6";
    request.system = system;
    request.messages = messages;
    return request;
}

agent::RequestPreparedContext PreparedContext() { return agent::RequestPreparedContext{}; }

api::Message AssistantWithToolCall(const std::string& call_id) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{"帮我派个代理。"});
    api::ToolUseBlock call;
    call.id = call_id;
    call.name = "agent";
    call.input = nlohmann::json{{"task", "查文档"}};
    message.content.push_back(std::move(call));
    return message;
}

api::Message ToolResults(const std::string& call_id, const std::string& content) {
    api::Message message;
    message.role = api::Role::User;
    api::ToolResultBlock result;
    result.tool_use_id = call_id;
    result.content = content;
    message.content.push_back(std::move(result));
    return message;
}

agent::ToolTraceEvent TraceEvent(agent::ToolTraceEventKind kind, const std::string& call_id) {
    agent::ToolTraceEvent event;
    event.kind = kind;
    event.tool_use_id = call_id;
    event.tool_name = "agent";
    event.execution_id = "exec-" + call_id;
    event.effective_input_sha256 =
        "aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111";
    event.effective_arguments = nlohmann::json{{"task", "查文档"}};
    return event;
}

// 一轮对话 turn:user 入账入链、prepared 落账、assistant 定稿、收口。
void DriveConversationTurn(TrajectorySessionLedger& ledger, const std::string& turn_id,
                           const std::string& text, const std::string& response_id) {
    auto bridge = ledger.NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    bridge->BeginTurn(turn_id, "external_user");
    bridge->RecordInput(UserMessage(text));
    const std::string request_id = bridge->OnRequestPrepared(
        MakeRequest("你是 LubanCode,读写跑都走工具。", {UserMessage(text)}), PreparedContext());
    REQUIRE_FALSE(request_id.empty());
    REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText("答:" + text.substr(0, 6)),
                                      "end_turn", response_id));
    bridge->OnUsageRecorded(request_id, api::Usage{}, /*reported_by_provider=*/false, response_id);
    bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
}

// 只计数的假后端:compact 干跑"零模型"断言的底(被调即测试失败)。
class CountingBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>&,
        const std::atomic<bool>*) override {
        ++calls;
        return {};
    }
    int calls = 0;
};

std::vector<nlohmann::json> ReadLines(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    std::vector<nlohmann::json> rows;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        rows.push_back(nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false));
        REQUIRE_FALSE(rows.back().is_discarded());
    }
    return rows;
}

}  // namespace

// ---------------------------------------------------------------------------
// 默认态:未设变量,开账走 v3
// ---------------------------------------------------------------------------

TEST_CASE("默认-v3 冒烟: 未设变量时 ledger 开场即 v3,一轮 turn 账面齐全") {
    EnvUnset unset("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    const auto root = FreshRoot("open-turn-close");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());

    // 建场形状:v3 主账在,旧两件(v2 main.jsonl / session.json)不在。
    const std::string session_id = ledger->session_id();
    const std::filesystem::path session_dir = ledger->session_dir();
    const std::filesystem::path stream =
        session_dir / platform::Utf8ToPath(session_id + ".jsonl");
    CHECK(std::filesystem::exists(stream));
    CHECK_FALSE(std::filesystem::exists(session_dir / "main.jsonl"));
    CHECK_FALSE(std::filesystem::exists(session_dir / "session.json"));
    // compact 管理入口的分派判据(RunCompactCommand/TryRunCompact 的
    // v3_main_writer() 判定)在默认态成立。
    CHECK(ledger->v3_main_writer() != nullptr);

    // 首两行:system(seq=1)+ session.started。
    const auto head = ReadLines(stream);
    REQUIRE(head.size() >= 2);
    CHECK(head[0].value("type", std::string()) == "message");
    CHECK(head[0]["message"].value("role", std::string()) == "system");
    CHECK(head[1].value("type", std::string()) == "event");
    CHECK(head[1].value("kind", std::string()) == "session.started");
    // 会话级事实(R2):launchCwd/runKind 随 session.started 落账,列表
    // 投影的权威来源。
    CHECK(head[1]["payload"].value("launchCwd", std::string()) == "D:/tmp/ws");
    CHECK(head[1]["payload"].value("runKind", std::string()) == "main_session");

    // 一轮 turn:user 入账入链、prepared 落账、assistant 定稿。
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("默认态的一问"));
        const std::string request_id =
            bridge->OnRequestPrepared(MakeRequest("你是 LubanCode,读写跑都走工具。",
                                                  {UserMessage("默认态的一问")}),
                                      PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText("默认态的一答"), "end_turn",
                                          "resp-1"));
        bridge->OnUsageRecorded(request_id, api::Usage{}, /*reported_by_provider=*/false,
                                "resp-1");
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    }
    const auto ledger_read = v3::ReadV3Ledger(stream);
    REQUIRE(ledger_read.has_value());
    bool saw_prepared = false;
    bool saw_user = false;
    bool saw_assistant = false;
    for (const auto& event : ledger_read->events) {
        if (event.kind == v3::EventKindV3::ModelRequestPrepared) {
            saw_prepared = true;
        }
    }
    for (const auto& message : ledger_read->messages) {
        const std::string role = message.message.value("role", std::string());
        saw_user = saw_user || role == "user";
        saw_assistant = saw_assistant || role == "assistant";
    }
    CHECK(saw_prepared);
    CHECK(saw_user);
    CHECK(saw_assistant);
    CHECK(ledger_read->context.chain.size() >= 3);  // system -> user -> assistant

    // 封口:CloseSession 落 session.ended。
    REQUIRE(ledger->CloseSession("exit").error_code.empty());
    const auto sealed = v3::ReadV3Ledger(stream);
    REQUIRE(sealed.has_value());
    bool ended = false;
    for (const auto& event : sealed->events) {
        ended = ended || event.kind == v3::EventKindV3::SessionEnded;
    }
    CHECK(ended);
}

TEST_CASE("默认-v3 冒烟: SessionManager::LaunchSession 同默认开 v3 场") {
    EnvUnset unset("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    const auto root = FreshRoot("manager-launch");
    lubancode::trajectory::SessionManagerOptions options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
    options.identity = lubancode::workspace::MakeFallbackIdentity(root / "ws");
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.251-test";
    options.v3_system_content = "你是 LubanCode,读写跑都走工具。";
    lubancode::trajectory::SessionManager manager(options);
    auto active = manager.LaunchSession();
    REQUIRE(active.has_value());
    const std::string session_id = (*active)->session_id();
    const std::filesystem::path session_dir = (*active)->session_dir();
    // v3 布局:只有 <id>.jsonl,无 v2 的 session.json。
    CHECK(std::filesystem::exists(
        session_dir / platform::Utf8ToPath(session_id + ".jsonl")));
    CHECK_FALSE(std::filesystem::exists(session_dir / "session.json"));
    CHECK_FALSE(std::filesystem::exists(session_dir / "main.jsonl"));
}

// Resume 接入 v3 单 R1 回归钉:v3 场的 workspace_key() 曾只认 v2 main
//(恒空),/resume 的 Cwd 范围拿空 key 直接空手——盘上档案全在也 0/0。
// 本案钉三层:key 非空且等于身份钥匙;封口后重开(退场再进的截图场景)
// Cwd 查询列得上一场;All 范围同账。
TEST_CASE("默认-v3 冒烟: v3 场 workspace_key 非空,Cwd 列表列得上一场") {
    EnvUnset unset("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    const auto root = FreshRoot("workspace-key-cwd");
    const auto identity = lubancode::workspace::MakeFallbackIdentity(root / "ws");
    const std::string first_id = [&] {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        // v3 场读面身份:不再只认 v2 main 的 scope。
        CHECK_FALSE(ledger->workspace_key().empty());
        CHECK(ledger->workspace_key() == identity.workspace_key);
        const std::string id = ledger->session_id();
        REQUIRE(ledger->CloseSession("exit").error_code.empty());
        return id;
    }();

    // 截图场景:退出重进,同 workspace 裸开新场,/resume 默认 Cwd 范围。
    auto second = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(second.has_value());
    trajectory::SessionIndexQuery cwd_query;  // all_workspaces=false:填 workspace_key()
    const auto cwd_page = second->ListWorkspaceSessions(cwd_query);
    CHECK(cwd_page.total >= 1);
    bool saw_first = false;
    for (const auto& entry : cwd_page.entries) {
        saw_first = saw_first || entry.session_id == first_id;
    }
    CHECK(saw_first);

    // All 范围同一份账(不走 workspace_key,历来自成;对齐用)。
    trajectory::SessionIndexQuery all_query;
    all_query.all_workspaces = true;
    const auto all_page = second->ListWorkspaceSessions(all_query);
    CHECK(all_page.total >= cwd_page.total);
}

// ---------------------------------------------------------------------------
// T16 勾二补齐:子代理入口默认态(接线点 1 只在建场读开关,子账跟随父场——
// 本案把 B1 时点的注释声明落成真断言)。
// ---------------------------------------------------------------------------

TEST_CASE("默认-v3 冒烟: 子代理入口默认场下子账同 v3,父账 spawn/linked 在") {
    EnvUnset unset("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    const auto root = FreshRoot("subagent-default");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string call_id = "call_default_agent";

    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("帮我派个代理查文档"));
    const std::string request_id = bridge->OnRequestPrepared(
        MakeRequest("你是 LubanCode,读写跑都走工具。", {UserMessage("帮我派个代理查文档")}),
        PreparedContext());
    REQUIRE_FALSE(request_id.empty());
    REQUIRE(bridge->OnOutputCompleted(request_id, AssistantWithToolCall(call_id), "tool_calls",
                                      "resp-a"));
    bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, call_id));
    bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, call_id));

    // 子代理入口(CLI/AppServer 共用):默认场分派 v3 五步,不读环境变量。
    auto child = ledger->SpawnSubagent(call_id, "查文档里的入口说明", "");
    REQUIRE(child.has_value());
    auto& child_bridge = (*child)->turn_bridge();
    child_bridge.BeginTurn("turn-child-1", "peer_agent");
    const std::string child_request = child_bridge.OnRequestPrepared(
        MakeRequest("你是 LubanCode,读写跑都走工具。", {UserMessage("查文档里的入口说明")}),
        PreparedContext());
    REQUIRE_FALSE(child_request.empty());
    REQUIRE(child_bridge.OnOutputCompleted(child_request, AssistantText("文档说入口在 main.cpp"),
                                           "end_turn", "resp-child"));
    child_bridge.EndTurn(true, false, "");
    REQUIRE_FALSE((*child)->Finish(/*ok=*/true, "done").empty());

    {
        agent::ToolTraceEvent finished =
            TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, call_id);
        finished.outcome = agent::ToolOutcome::Succeeded;
        bridge->OnToolTrace(finished);
        bridge->OnToolResultsCommitted("batch-2", ToolResults(call_id, "文档说入口在 main.cpp"));
        bridge->EndTurn(true, false, "");
    }
    REQUIRE(ledger->CloseSession("exit").error_code.empty());

    // 父账:默认场的 spawn/linked 事实在(不是 v2 的 subagents/<run>.jsonl 老账)。
    const auto parent_rows = ReadLines(ledger->session_dir() /
                                       platform::Utf8ToPath(ledger->session_id() + ".jsonl"));
    bool saw_spawn = false;
    bool saw_linked = false;
    for (const auto& row : parent_rows) {
        if (row.value("type", std::string()) != "event") {
            continue;
        }
        saw_spawn = saw_spawn || row.value("kind", std::string()) == "subagent.spawn.requested";
        saw_linked = saw_linked || row.value("kind", std::string()) == "subagent.linked";
    }
    CHECK(saw_spawn);
    CHECK(saw_linked);

    // 子账:v3 布局 subagents/<child>/<child>.jsonl,首行 system 带派生来源。
    bool found_child = false;
    for (const auto& entry :
         std::filesystem::directory_iterator(ledger->session_dir() / "subagents")) {
        if (!entry.is_directory()) {
            continue;
        }
        found_child = true;
        const std::string child_name = platform::PathToUtf8(entry.path().filename());
        const auto child_stream = entry.path() / platform::Utf8ToPath(child_name + ".jsonl");
        REQUIRE(std::filesystem::exists(child_stream));
        const auto child_rows = ReadLines(child_stream);
        REQUIRE_FALSE(child_rows.empty());
        CHECK(child_rows[0].value("type", std::string()) == "message");
        CHECK(child_rows[0].at("message").value("role", std::string()) == "system");
        CHECK(child_rows[0].at("systemMeta").value("cause", std::string()) == "subagent_spawn");
        CHECK(lubancode::trajectory::v3::VerifyV3File(child_stream).ok);
    }
    CHECK(found_child);
}

// ---------------------------------------------------------------------------
// T16 勾二补齐:CLI /compact 管理入口(RunCompactCommand)默认场分派 v3 分支
// ——干跑全链过真入口(T12-B 的容量规划器/门禁/回退梯真跑),零模型零写入。
// 真压缩链(applied/阻断/降档)由 v3_compact_runtime/v3_compact_gates 册
// 守;本案只钉"完全不设变量时管理入口走的是 v3 卷"。
// ---------------------------------------------------------------------------

TEST_CASE("默认-v3 冒烟: CLI compact 管理入口默认场分派 v3 干跑,零模型零写入") {
    EnvUnset unset("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    const auto root = FreshRoot("compact-cli-dry-run");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    CHECK(ledger->v3_main_writer() != nullptr);  // 分派判据(默认态)

    // 两轮对话进账(v3_compact_runtime 干跑案同款体量,过 no_eligible 门)。
    DriveConversationTurn(*ledger, "turn-1", std::string(600, 'a'), "r-1");
    DriveConversationTurn(*ledger, "turn-2", std::string(600, 'b'), "r-2");
    const std::filesystem::path stream =
        ledger->session_dir() / platform::Utf8ToPath(ledger->session_id() + ".jsonl");
    const std::size_t lines_before = ReadLines(stream).size();

    CountingBackend backend;  // 干跑绝不调;被调即测试失败
    lubancode::tools::ToolRegistry registry;
    lubancode::agent::Agent loop(backend, registry,
                                 lubancode::agent::AgentProfile{
                                     .request{.model = "kimi-k2.6"},
                                     .runtime{.context_window_tokens = 200000},
                                     .system_prompt = "sys"});
    app::CompactSessionInputs in;
    in.agent = &loop;
    const lubancode::cli::Theme theme;
    in.theme = &theme;
    int compact_epoch = 0;
    in.session_compact_epoch = &compact_epoch;
    std::string last_compact_line;
    in.last_compact_line = &last_compact_line;
    in.build_compact_options = [] { return lubancode::agent::CompactOptions{}; };
    lubancode::agent::ModelRoute route;
    route.model = "kimi-k2.6";
    route.provider = "moonshot";
    in.route_compact = [&backend, &route]() {
        lubancode::app::ModelRouterService::Routed routed;
        routed.route = route;
        routed.backend = &backend;
        return routed;
    };
    in.route_repair = in.route_compact;
    in.record_usage = [](const lubancode::agent::ModelRole, const lubancode::agent::ModelRoute&,
                         const lubancode::agent::BackgroundCallAccounting&) {};
    in.record_fallback = [](lubancode::agent::TaskKind, lubancode::agent::ModelRole,
                            lubancode::agent::ModelRole, const std::string&) {};
    in.trajectory = &*ledger;  // Open 回 expected<TrajectorySessionLedger,string>(按值),解引用取址
    in.trajectory_wire = "openai-chat-completions";

    app::RunCompactCommand("--dry-run", in);

    // 零模型:干跑只算不压,backend 不发包。
    CHECK(backend.calls == 0);
    // 零写入:账一字不长,无 compact 事件;干跑不是 applied,台账不挂。
    const auto rows = ReadLines(stream);
    CHECK(rows.size() == lines_before);
    for (const auto& row : rows) {
        if (row.value("type", std::string()) == "event") {
            const std::string kind = row.value("kind", std::string());
            CHECK_MESSAGE(kind.rfind("compact.", 0) != 0, "干跑不得落 compact 事件: " << kind);
        }
    }
    CHECK(compact_epoch == 0);  // 干跑不算一次压缩收口
    REQUIRE(ledger->CloseSession("exit").error_code.empty());
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}
