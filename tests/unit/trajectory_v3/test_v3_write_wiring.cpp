// 轨迹 v3 接线点 1(session_switch.hpp):新会话写侧 v3 开关真接线 +
// 读写闭环。开关(LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=1)只在建场时读
// 一次:开 = sessions/<id>/<id>.jsonl(首行 system seq=1 turnId=null +
// session.started),关 = 现行 v2 main.jsonl 一字不动。两路各有回归钉:
//   - 开关关:LaunchSession 落 v2 布局(main.jsonl + session.json),回合
//     流落 v2 事件账,<id>.jsonl 不存在;
//   - 开关开:建场即首行 system;一轮完整会话流(输入 → 请求/回复 →
//     工具调用/结果(tool_action + result_store 路径)→ 结束)全落 v3,
//     验卷过、工具配对完整;resume/--continue 读回(P3 链投影)。
// 验收对表 todos §5.1:启动后不输入、加载 soul、普通 resume、工具组配对
// 完整、只读 replay(零模型调用零工具重跑)。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/tool_trace.hpp"
#include "api/anthropic/client.hpp"  // BuildRequestJson:P1-B wire 字段对照
#include "api/types.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
#include "runtime/v3_compact_runtime.hpp"  // D3:RunV3Compact 驱动 applied
#include "trajectory/journal.hpp"
#include "trajectory/session_index.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
using namespace lubancode;
using lubancode::runtime::TrajectorySessionLedger;
using lubancode::runtime::TrajectoryTurnBridge;

namespace {

// 环境变量门卫:构造置值,析构还原(不漏开关状态给别的册)。
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

std::filesystem::path FreshRoot(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-v3-write-wiring-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

TrajectorySessionLedger::Options LedgerOptions(const std::filesystem::path& root) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
    // 身份显式递(免走 cwd 现场裁决——ws 目录不存在时裁决起点会退到父
    // 目录,与裸 manager 的 fallback 对不上房门)。
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "ws");
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.238-test";
    options.v3_system_content = "你是 LubanCode,读写跑都走工具。";
    return options;
}

// ---- 假后端材料(与生产桥口同形) ----

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

api::Message AssistantWithToolCall(const std::string& call_id) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{"我读一下入口文件。"});
    api::ToolUseBlock call;
    call.id = call_id;
    call.name = "read_file";
    call.input = nlohmann::json{{"path", "src/app/main.cpp"}};
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

api::Request MakeRequest(const std::string& system, const std::vector<api::Message>& messages) {
    api::Request request;
    request.model = "kimi-k2.6";
    request.system = system;
    request.messages = messages;
    return request;
}

agent::RequestPreparedContext PreparedContext() { return agent::RequestPreparedContext{}; }

api::Usage SampleUsage() {
    api::Usage usage;
    usage.input_tokens = 1180;
    usage.output_tokens = 24;
    return usage;
}

agent::ToolTraceEvent TraceEvent(agent::ToolTraceEventKind kind, const std::string& call_id) {
    agent::ToolTraceEvent event;
    event.kind = kind;
    event.tool_use_id = call_id;
    event.tool_name = "read_file";
    event.execution_id = "exec-" + call_id;
    event.effective_input_sha256 = "aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111aaaa1111";
    event.effective_arguments = nlohmann::json{{"path", "src/app/main.cpp"}};
    return event;
}

// 一轮"输入 → 请求/回复(带工具声明)→ 执行 → 结果回喂"的最小完整流。
// system 非空即触发 system 对表(首请求会从基础 system 切到真 system)。
struct TurnFlow {
    std::string request_id;
};

TurnFlow DriveToolTurn(TrajectoryTurnBridge& bridge, const std::string& system,
                       const std::string& user_text, const std::string& call_id) {
    bridge.BeginTurn("turn-1", "external_user");
    bridge.RecordInput(UserMessage(user_text));
    const std::string request_id =
        bridge.OnRequestPrepared(MakeRequest(system, {UserMessage(user_text)}), PreparedContext());
    REQUIRE_FALSE(request_id.empty());
    bridge.OnRequestSent(request_id);
    bridge.OnUsageRecorded(request_id, SampleUsage(), /*reported_by_provider=*/true,
                           "resp-123", 0, true, false);
    REQUIRE(bridge.OnOutputCompleted(request_id, AssistantWithToolCall(call_id), "tool_calls",
                                     "resp-123"));
    bridge.OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, call_id));
    agent::ToolTraceEvent started =
        TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, call_id);
    started.outcome = agent::ToolOutcome::Succeeded;
    bridge.OnToolTrace(started);
    agent::ToolTraceEvent finished =
        TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, call_id);
    finished.outcome = agent::ToolOutcome::Succeeded;
    finished.duration_ms = 42;
    finished.details = nlohmann::json{{"exit_code", 0}};
    bridge.OnToolTrace(finished);
    bridge.OnToolResultsCommitted("batch-1", ToolResults(call_id, "int main(int argc, char** argv)"));
    bridge.EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    return TurnFlow{request_id};
}

std::filesystem::path V3StreamOf(const TrajectorySessionLedger& ledger) {
    return ledger.session_dir() /
           platform::Utf8ToPath(platform::PathToUtf8(ledger.session_dir().filename()) + ".jsonl");
}

std::vector<nlohmann::json> ReadLines(const std::filesystem::path& stream) {
    std::vector<nlohmann::json> rows;
    std::ifstream file(stream, std::ios::binary);
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

std::vector<std::string> KindsOf(const std::vector<nlohmann::json>& rows) {
    std::vector<std::string> kinds;
    for (const auto& row : rows) {
        const auto it = row.find("kind");
        kinds.push_back(it != row.end() && it->is_string() ? it->get<std::string>() : std::string());
    }
    return kinds;
}

}  // namespace

// ---------------------------------------------------------------------------
// 开关关:现行 v2 路一字不动(回归钉)
// ---------------------------------------------------------------------------

TEST_CASE("开关关: 开场与回合流照走 v2,不长 v3 文件") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const auto root = FreshRoot("off");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path session_dir = ledger->session_dir();
    // v2 布局:main.jsonl + session.json;v3 主账不存在。
    CHECK(std::filesystem::exists(session_dir / "main.jsonl"));
    CHECK(std::filesystem::exists(session_dir / "session.json"));
    CHECK_FALSE(std::filesystem::exists(
        session_dir / platform::Utf8ToPath(platform::PathToUtf8(session_dir.filename()) + ".jsonl")));

    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    DriveToolTurn(*bridge, "SYSTEM-A", "看看入口", "call_prov_off");
    const auto closed = ledger->CloseSession("exit");
    CHECK(closed.error_code.empty());

    const auto rows = ReadLines(session_dir / "main.jsonl");
    const auto kinds = KindsOf(rows);
    CHECK(std::find(kinds.begin(), kinds.end(), "run.started") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "input.received") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "model.request.prepared") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "model.output.completed") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "tool.execution.finished") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "tool.result.committed") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "session.ended") != kinds.end());
}

// ---------------------------------------------------------------------------
// 开关开:建场即首行 system;启动后不输入,退出不造用户回合(§5.1 行 1)
// ---------------------------------------------------------------------------

TEST_CASE("开关开: 首行 system seq=1 turnId=null,无 main.jsonl/session.json") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("open-idle");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream = V3StreamOf(*ledger);

    // 目录合同:只有 <id>.jsonl(artifacts/subagents 占位),v2 文件一枚不长。
    CHECK(std::filesystem::exists(stream));
    CHECK_FALSE(std::filesystem::exists(ledger->session_dir() / "main.jsonl"));
    CHECK_FALSE(std::filesystem::exists(ledger->session_dir() / "session.json"));
    // 清单/resume 识别(接线点 2)立刻认得这场。
    CHECK(lubancode::trajectory::v3::FindV3SessionStream(ledger->session_dir()).has_value());

    auto verified = lubancode::trajectory::v3::VerifyV3File(stream);
    CHECK(verified.ok);
    const auto rows = ReadLines(stream);
    REQUIRE(rows.size() >= 2);
    // 首行 system(§1.2):seq=1、turnId=null、自带身份、systemMeta.cause=initial。
    CHECK(rows[0].value("type", std::string()) == "message");
    CHECK(rows[0].value("seq", 0) == 1);
    CHECK(rows[0].contains("turnId"));
    CHECK(rows[0].at("turnId").is_null());
    CHECK(rows[0].value("schemaVersion", 0) == 3);
    CHECK(rows[0].value("sessionId", std::string()) == ledger->session_id());
    CHECK(rows[0].at("message").value("role", std::string()) == "system");
    CHECK(rows[0].at("systemMeta").value("cause", std::string()) == "initial");
    CHECK(rows[0].at("message").value("content", std::string()) ==
          LedgerOptions(root).v3_system_content);
    // 次行 session.started(修订 1 单节点链)。
    CHECK(rows[1].value("type", std::string()) == "event");
    CHECK(rows[1].value("kind", std::string()) == "session.started");

    // 启动后不输入直接退出:只补 session.ended,不凭空造用户回合。
    const auto closed = ledger->CloseSession("exit");
    CHECK(closed.error_code.empty());
    const auto after = ReadLines(stream);
    CHECK(after.size() == rows.size() + 1);
    CHECK(after.back().value("kind", std::string()) == "session.ended");
    for (const auto& row : after) {
        if (row.value("type", std::string()) == "message") {
            CHECK(row.at("message").value("role", std::string()) == "system");
        }
    }
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

// ---------------------------------------------------------------------------
// 开关开:一轮完整会话流的写读闭环(§5.1 工具组配对完整 + 只读 replay)
// ---------------------------------------------------------------------------

TEST_CASE("开关开: 输入→回复→工具链→结束,验卷过、配对完整、结果仓落档") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("open-loop");
    TrajectorySessionLedger::Options options = LedgerOptions(root);
    options.v3_system_content = "SYSTEM-BASE";
    auto ledger = TrajectorySessionLedger::Open(options);
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream = V3StreamOf(*ledger);
    const std::string source_id = ledger->session_id();

    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        DriveToolTurn(*bridge, "SYSTEM-REAL", "帮我看看 src/app/main.cpp 的入口。", "call_prov_9xK");
    }
    const auto closed = ledger->CloseSession("exit");
    CHECK(closed.error_code.empty());

    // 验卷:seq 连续、哈希衔接、语义校验全过。
    const auto verify = lubancode::trajectory::v3::VerifyV3File(stream);
    REQUIRE(verify.ok);

    // 读账:核心会话轴的行齐(user/assistant/tool 消息 + 请求/工具事件)。
    auto read = lubancode::trajectory::v3::ReadV3Ledger(stream);
    REQUIRE(read.has_value());
    const auto kinds = KindsOf(ReadLines(stream));
    CHECK(std::find(kinds.begin(), kinds.end(), "input.received") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "model.request.prepared") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "model.request.sent") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "tool.execution.pending") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "tool.execution.started") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "tool.execution.finished") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "tool.result.persisted") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "tool.result.selected") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "session.ended") != kinds.end());

    // 工具组配对完整(§4.19/§5.1):声明→接纳→执行终态→选用→tool 消息
    // 全链在账上,折叠状态 done;provider 号经 actionId 与 tool 消息配对。
    const auto snapshots = lubancode::trajectory::v3::FoldToolActions(*read);
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].provider_tool_call_id.value_or("") == "call_prov_9xK");
    CHECK(snapshots[0].assistant_message_ref.has_value());
    CHECK(snapshots[0].folded_status == "done");
    REQUIRE(snapshots[0].message_versions.size() == 1);
    CHECK(snapshots[0].message_versions[0].on_current_chain);
    CHECK(snapshots[0].selected_event_ref.has_value());
    CHECK(snapshots[0].persisted_event_refs.size() == 1);

    // 链投影(§4.1 行 2):user → assistant(带调用块)→ tool 有序在链,
    // assistant 调用块 id 与 tool 消息 tool_call_id 同为 actionId。
    const auto context = lubancode::trajectory::v3::ProjectModelContext(*read);
    REQUIRE(context.inputs.size() == 3);
    CHECK(context.inputs[0].message.value("role", std::string()) == "user");
    CHECK(context.inputs[0].message.value("content", std::string()) ==
          "帮我看看 src/app/main.cpp 的入口。");
    CHECK(context.inputs[1].message.value("role", std::string()) == "assistant");
    REQUIRE(context.inputs[1].message.contains("tool_calls"));
    // §4.15 配对口径:assistant 调用块带 provider 原始号(原样留档,不改
    // 写);tool 消息带 v3 全局 actionId;两者经 FoldToolActions 的
    // providerToolCallId 映射对上(§4.19 折叠同一条链)。
    const std::string provider_id =
        context.inputs[1].message.at("tool_calls").at(0).value("id", std::string());
    CHECK(provider_id == "call_prov_9xK");
    CHECK(snapshots[0].provider_tool_call_id.value_or("") == provider_id);
    CHECK(context.inputs[2].message.value("role", std::string()) == "tool");
    CHECK(context.inputs[2].message.value("tool_call_id", std::string()) ==
          snapshots[0].tool_call_id);
    CHECK(context.inputs[2].message.value("content", std::string()) ==
          "int main(int argc, char** argv)");
    // system 已从基础版切到真版(下一条用例细验三步)。
    CHECK(context.system_content == "SYSTEM-REAL");
    // assistant 来源与 usage(§4.44/§五):来源三件套 + 实报 usage 随行。
    for (const auto& row : ReadLines(stream)) {
        if (row.value("type", std::string()) != "message" ||
            row.at("message").value("role", std::string()) != "assistant") {
            continue;
        }
        CHECK(row.value("provider", std::string()) == "moonshot");
        CHECK(row.value("wire", std::string()) == "openai-chat-completions");
        CHECK(row.value("model", std::string()) == "kimi-k2.6");
        CHECK(row.value("responseModel", std::string()) == "resp-123");
        REQUIRE(row.contains("usage"));
        CHECK(row.at("usage").value("inputTokens", 0) == 1180);
    }

    // 结果仓(§4.16/§4.18):artifact 真落盘,选用链能展开、hash 对得上。
    const std::string tool_message_id = context.inputs[2].message_id;
    const auto preview = lubancode::trajectory::v3::ExpandResultPreview(
        *read, ledger->session_dir(), tool_message_id);
    CHECK(preview.complete);
    // §4.16:result_ref = [result_metadata, combined](描述文件 + 通道正文)。
    REQUIRE(preview.result_refs.size() == 2);
    for (const auto& ref : preview.result_refs) {
        CHECK(std::filesystem::exists(
            ledger->session_dir() / platform::Utf8ToPath(ref.value("path", std::string()))));
    }

    // /sessions 清单(接线点 2)并列认 v3 场:摘要与提问历史可查。
    {
        lubancode::trajectory::SessionIndexQuery query;
        query.all_workspaces = true;
        query.include_archived = true;
        const auto page = ledger->ListWorkspaceSessions(query);
        const auto* summary = [&]() -> const lubancode::trajectory::WorkspaceSessionSummary* {
            for (const auto& entry : page.entries) {
                if (entry.session_id == source_id) {
                    return &entry;
                }
            }
            return nullptr;
        }();
        REQUIRE(summary != nullptr);
        CHECK(summary->message_count == 2);  // human user + assistant(§口径)
        CHECK(summary->first_user_text == "帮我看看 src/app/main.cpp 的入口。");
        CHECK(summary->status == "closed");
    }

    // --continue(--continue 读回):新账本开 resume_at_launch,链投影灌回。
    TrajectorySessionLedger::Options resume_options = LedgerOptions(root);
    resume_options.resume_at_launch = true;
    auto resumed = TrajectorySessionLedger::Open(resume_options);
    REQUIRE(resumed.has_value());
    CHECK(resumed->resumed_at_launch());
    const auto history = resumed->LaunchResumeHistory();
    REQUIRE(history.size() == 3);
    CHECK(history[0].role == lubancode::api::Role::User);
    CHECK(history[1].role == lubancode::api::Role::Assistant);
    CHECK(history[2].role == lubancode::api::Role::User);  // tool 结果以 user 携带(投影口径)
    // v3 源的旧史显示投影(P3)在场。
    CHECK(resumed->LaunchRestoredHistoryView().has_value());
    // 新场也是 v3(开关仍开):目录合同 + resume.source.attached 五键指源末行。
    const std::filesystem::path new_stream = V3StreamOf(*resumed);
    CHECK(std::filesystem::exists(new_stream));
    CHECK_FALSE(std::filesystem::exists(resumed->session_dir() / "main.jsonl"));
    const auto source_rows = ReadLines(stream);
    const auto new_rows = ReadLines(new_stream);
    const auto attached = std::find_if(new_rows.begin(), new_rows.end(), [](const nlohmann::json& row) {
        return row.value("kind", std::string()) == "resume.source.attached";
    });
    REQUIRE(attached != new_rows.end());
    REQUIRE(attached->contains("payload"));
    const auto& source_ref = attached->at("payload").at("sourceRef");
    CHECK(source_ref.value("sessionId", std::string()) == source_id);
    CHECK(source_ref.value("id", std::string()) ==
          source_rows.back().value("eventId", source_rows.back().value("messageId", std::string())));
    CHECK(source_ref.value("hash", std::string()) ==
          source_rows.back().value("lineHash", std::string()));
    CHECK(lubancode::trajectory::v3::VerifyV3File(new_stream).ok);
}

// ---------------------------------------------------------------------------
// 加载 soul(§5.1 行 2):旧 system → 切换事件 → 新 system
// ---------------------------------------------------------------------------

TEST_CASE("开关开: 换 system 走三步,prepared 引用新根,旧请求不动") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("open-soul");
    TrajectorySessionLedger::Options options = LedgerOptions(root);
    options.v3_system_content = "SYSTEM-OLD";
    auto ledger = TrajectorySessionLedger::Open(options);
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream = V3StreamOf(*ledger);

    // 第一轮:真 system 到场,基础版 → 真版(一次切换)。
    std::string first_request;
    std::string first_assistant;
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("第一问"));
        first_request = bridge->OnRequestPrepared(MakeRequest("SYSTEM-NEW", {UserMessage("第一问")}),
                                                  PreparedContext());
        REQUIRE_FALSE(first_request.empty());
        REQUIRE(bridge->OnOutputCompleted(first_request, AssistantText("第一答"), "end_turn", "resp-1"));
        first_assistant = "第一答";
        bridge->EndTurn(true, false, "");
    }
    // 第二轮:system 没变,不再制造假版本。
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        bridge->BeginTurn("turn-2", "external_user");
        bridge->RecordInput(UserMessage("第二问"));
        const std::string second_request = bridge->OnRequestPrepared(
            MakeRequest("SYSTEM-NEW", {UserMessage("第二问")}), PreparedContext());
        REQUIRE_FALSE(second_request.empty());
        REQUIRE(bridge->OnOutputCompleted(second_request, AssistantText("第二答"), "end_turn", "resp-2"));
        bridge->EndTurn(true, false, "");
    }
    (void)first_assistant;

    const auto rows = ReadLines(stream);
    // system 消息恰两枚:基础版 + 新版;顺序旧在前。
    std::vector<std::size_t> system_rows;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].value("type", std::string()) == "message" &&
            rows[i].at("message").value("role", std::string()) == "system") {
            system_rows.push_back(i);
        }
    }
    REQUIRE(system_rows.size() == 2);
    CHECK(rows[system_rows[0]].at("message").value("content", std::string()) == "SYSTEM-OLD");
    CHECK(rows[system_rows[1]].at("message").value("content", std::string()) == "SYSTEM-NEW");
    CHECK(rows[system_rows[1]].at("systemMeta").value("changeEventRef", std::string()) != "");
    // 切换事件族:system.change 在新 system 之前,context.system.applied 紧随其后。
    const auto kinds = KindsOf(rows);
    const auto change_at = std::find(kinds.begin(), kinds.end(), "system.change");
    const auto applied_at = std::find(kinds.begin(), kinds.end(), "context.system.applied");
    REQUIRE(change_at != kinds.end());
    REQUIRE(applied_at != kinds.end());
    const std::size_t change_index = static_cast<std::size_t>(change_at - kinds.begin());
    const std::size_t applied_index = static_cast<std::size_t>(applied_at - kinds.begin());
    CHECK(change_index < system_rows[1]);
    CHECK(applied_index > system_rows[1]);
    CHECK(rows[change_index].at("payload").value("systemChanged", false) == true);
    CHECK(rows[change_index].at("payload").value("oldSystemMessageRef", std::string()) ==
          rows[system_rows[0]].value("messageId", std::string()));
    // prepared 引用:第一轮请求指旧根?不——第一轮就切到了真版,两轮都指
    // 新根;旧根只留在链外档里(§4.3"旧请求仍指旧版"对已发出的请求成立,
    // 这里验的是内存当前根与 prepared 同拍)。
    std::vector<std::string> prepared_systems;
    for (const auto& row : rows) {
        if (row.value("kind", std::string()) == "model.request.prepared") {
            prepared_systems.push_back(
                row.at("payload").value("systemMessageRef", std::string()));
        }
    }
    REQUIRE(prepared_systems.size() == 2);
    CHECK(prepared_systems[0] == rows[system_rows[1]].value("messageId", std::string()));
    CHECK(prepared_systems[1] == rows[system_rows[1]].value("messageId", std::string()));
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

// ---------------------------------------------------------------------------
// 普通 resume(§5.1 行 4):/resume 交互路,ID 顺序不变
// ---------------------------------------------------------------------------

TEST_CASE("开关开: /resume 封旧场开新 v3 场,旧消息 ID 顺序不变") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("open-resume");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string source_id = ledger->session_id();
    const std::filesystem::path source_stream = V3StreamOf(*ledger);

    std::string assistant_id;
    std::string user_id;
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("旧场一问"));
        const std::string request_id =
            bridge->OnRequestPrepared(MakeRequest("SYSTEM-X", {UserMessage("旧场一问")}),
                                      PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText("旧场一答"), "end_turn", "r-1"));
        bridge->EndTurn(true, false, "");
    }
    // 源场消息 id(投影里对得上才算"ID 不变")。
    for (const auto& row : ReadLines(source_stream)) {
        if (row.value("type", std::string()) != "message") {
            continue;
        }
        const std::string role = row.at("message").value("role", std::string());
        if (role == "user") {
            user_id = row.value("messageId", std::string());
        } else if (role == "assistant") {
            assistant_id = row.value("messageId", std::string());
        }
    }
    REQUIRE_FALSE(user_id.empty());
    REQUIRE_FALSE(assistant_id.empty());

    // 交互 /resume:封旧场(switch_to_resume)→ 开新 v3 场 → 折叠投影。
    const auto summary = ledger->ResumeInteractive(source_id);
    REQUIRE(summary.outcome.error_code.empty());
    CHECK(summary.outcome.source_is_v3);
    CHECK(summary.outcome.source_session_id == source_id);
    CHECK(summary.restored_view.has_value());
    // 折叠出的有效对话:角色顺序不变,source id 逐一指回源场消息。
    REQUIRE(summary.history.size() == 2);
    CHECK(summary.history[0].role == lubancode::api::Role::User);
    CHECK(summary.history[1].role == lubancode::api::Role::Assistant);
    REQUIRE(summary.outcome.effective_conversation.size() == 2);
    CHECK(summary.outcome.effective_conversation[0].source_event_id == user_id);
    CHECK(summary.outcome.effective_conversation[1].source_event_id == assistant_id);
    // 源场封口(session.ended 落稳),新场是 v3 带 resume.source.attached。
    const auto source_kinds = KindsOf(ReadLines(source_stream));
    CHECK(std::find(source_kinds.begin(), source_kinds.end(), "session.ended") != source_kinds.end());
    const auto new_kinds = KindsOf(ReadLines(V3StreamOf(*ledger)));
    CHECK(std::find(new_kinds.begin(), new_kinds.end(), "resume.source.attached") != new_kinds.end());
    CHECK(std::filesystem::exists(V3StreamOf(*ledger)));
    // 新场继续写:再来一轮,验卷仍过(新旧链同卷)。
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        bridge->BeginTurn("turn-9", "external_user");
        bridge->RecordInput(UserMessage("新场一问"));
        const std::string request_id =
            bridge->OnRequestPrepared(MakeRequest("SYSTEM-X", {UserMessage("新场一问")}),
                                      PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText("新场一答"), "end_turn", "r-2"));
        bridge->EndTurn(true, false, "");
    }
    CHECK(lubancode::trajectory::v3::VerifyV3File(V3StreamOf(*ledger)).ok);
}

// ---------------------------------------------------------------------------
// 子账五步(§4.31/§4.32):开 = subagents/<child>/<child>.jsonl 走 v3
// ---------------------------------------------------------------------------

TEST_CASE("开关开: 子代理走五步,父账 spawn/linked、子账派生来源可验") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("open-subagent");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream = V3StreamOf(*ledger);
    const std::string call_id = "call_prov_agent";

    auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    // 父回合推进到调用声明 + 排程,然后派工(与 turn_runner 的钩子同拍)。
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("帮我派个代理查文档"));
    const std::string request_id = bridge->OnRequestPrepared(
        MakeRequest("SYSTEM-Y", {UserMessage("帮我派个代理查文档")}), PreparedContext());
    REQUIRE_FALSE(request_id.empty());
    REQUIRE(bridge->OnOutputCompleted(request_id, AssistantWithToolCall(call_id), "tool_calls",
                                      "resp-a"));
    bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, call_id));
    bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, call_id));

    auto child = ledger->SpawnSubagent(call_id, "查文档里的入口说明", "");
    REQUIRE(child.has_value());
    // 子账驱动:委派输入已在 Bootstrap 落稳,这里走子轮模型边界。
    auto& child_bridge = (*child)->turn_bridge();
    child_bridge.BeginTurn("turn-child-1", "peer_agent");
    const std::string child_request = child_bridge.OnRequestPrepared(
        MakeRequest("SYSTEM-Y", {UserMessage("查文档里的入口说明")}), PreparedContext());
    REQUIRE_FALSE(child_request.empty());
    REQUIRE(child_bridge.OnOutputCompleted(child_request, AssistantText("文档说入口在 main.cpp"),
                                           "end_turn", "resp-child"));
    child_bridge.EndTurn(true, false, "");
    const std::string child_hash = (*child)->Finish(/*ok=*/true, "done");
    REQUIRE_FALSE(child_hash.empty());

    // 父侧收尾:执行终态 + 结果回喂(agent 调用的 tool 消息)。
    {
        agent::ToolTraceEvent finished =
            TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, call_id);
        finished.outcome = agent::ToolOutcome::Succeeded;
        bridge->OnToolTrace(finished);
        bridge->OnToolResultsCommitted("batch-2", ToolResults(call_id, "文档说入口在 main.cpp"));
        bridge->EndTurn(true, false, "");
    }
    const auto closed = ledger->CloseSession("exit");
    CHECK(closed.error_code.empty());

    // 父账:五步的事件族在场。
    const auto kinds = KindsOf(ReadLines(stream));
    CHECK(std::find(kinds.begin(), kinds.end(), "subagent.spawn.requested") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "subagent.linked") != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), "tool.result.selected") != kinds.end());
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);

    // 子账目录与首行派生来源(§4.31)。
    bool found_child = false;
    for (const auto& entry :
         std::filesystem::directory_iterator(ledger->session_dir() / "subagents")) {
        if (!entry.is_directory()) {
            continue;
        }
        found_child = true;
        const std::string child_name = platform::PathToUtf8(entry.path().filename());
        const auto child_stream = entry.path() / platform::Utf8ToPath(child_name + ".jsonl");
        CHECK(std::filesystem::exists(child_stream));
        const auto child_rows = ReadLines(child_stream);
        REQUIRE_FALSE(child_rows.empty());
        CHECK(child_rows[0].value("type", std::string()) == "message");
        CHECK(child_rows[0].at("message").value("role", std::string()) == "system");
        const auto& meta = child_rows[0].at("systemMeta");
        CHECK(meta.value("cause", std::string()) == "subagent_spawn");
        REQUIRE(meta.contains("spawnEventRef"));
        CHECK(meta.at("spawnEventRef").value("sessionId", std::string()) == ledger->session_id());
        CHECK(meta.at("spawnEventRef").contains("hash"));
        const auto child_kinds = KindsOf(child_rows);
        CHECK(std::find(child_kinds.begin(), child_kinds.end(), "task.started") != child_kinds.end());
        CHECK(std::find(child_kinds.begin(), child_kinds.end(), "session.ended") != child_kinds.end());
        CHECK(lubancode::trajectory::v3::VerifyV3File(child_stream).ok);
    }
    CHECK(found_child);
}

// ---------------------------------------------------------------------------
// 恢复器不碰 v3 场(接线点 1 的护栏:无 manifest 的场不得被当孤儿清账)
// ---------------------------------------------------------------------------

TEST_CASE("开关开: RecoverWorkspace 认得 v3 场,原样保留不误删") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("open-recover");
    std::filesystem::path stream;
    std::string session_id;
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        session_id = ledger->session_id();
        stream = V3StreamOf(*ledger);
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        DriveToolTurn(*bridge, "SYSTEM-Z", "留个档", "call_prov_rec");
        CHECK(ledger->CloseSession("exit").error_code.empty());
    }
    const std::string before = [&] {
        std::ifstream file(stream, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }();
    // 新 manager 起恢复器(v2 状态机的账):v3 场应被认出并跳过。身份按
    // 账本同款兜底(裸 manager 不递身份,EnsureWorkspace 直接失败)。
    lubancode::trajectory::SessionManagerOptions options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
    options.identity = lubancode::workspace::MakeFallbackIdentity(root / "ws");
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.238-test";
    lubancode::trajectory::SessionManager manager(options);
    const auto report = manager.RecoverWorkspace();
    bool seen = false;
    for (const auto& entry : report.sessions) {
        if (entry.session_id == session_id) {
            seen = true;
            CHECK_FALSE(entry.aborted_before_start);
        }
    }
    CHECK(seen);
    CHECK(std::filesystem::exists(stream));
    std::string after;
    {
        std::ifstream file(stream, std::ios::binary);
        after = std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }
    CHECK(after == before);
}

// ---------------------------------------------------------------------------
// 开关只在建场读一次:半程翻转环境变量不改本场格式
// ---------------------------------------------------------------------------

TEST_CASE("开关已翻默认: 环境变量未设时新会话写 v3(守门)") {
    // ctest 全局注入 0 保 v2 老册;此处显式摘掉变量,钉产品默认语义——
    // 翻默认(2026-09-11)的本义:未设=开,只有显式 0 才回 v2。
#ifdef _WIN32
    _putenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=");
#else
    unsetenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
#endif
    CHECK(lubancode::trajectory::v3::NewSessionV3WriteEnabled());
#ifdef _WIN32
    _putenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0");
#else
    setenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0", 1);
#endif
    CHECK_FALSE(lubancode::trajectory::v3::NewSessionV3WriteEnabled());
}

TEST_CASE("开关读一次: 场开成 v3 后,环境变量翻回 0 不改本场写侧") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("open-once");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream = V3StreamOf(*ledger);
    CHECK(std::filesystem::exists(stream));
#ifdef _WIN32
    _putenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0");
#else
    setenv("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0", 1);
#endif
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        DriveToolTurn(*bridge, "SYSTEM-Q", "半程翻转", "call_prov_flip");
    }
    CHECK(ledger->CloseSession("exit").error_code.empty());
    // 本场仍是纯 v3:v2 文件一枚不长,验卷过。
    CHECK_FALSE(std::filesystem::exists(ledger->session_dir() / "main.jsonl"));
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

// ---------------------------------------------------------------------------
// D3(§5.1.2):compact applied 后的内存换账投影——ProjectV3ContextHistory
// 重读主卷按链投影,返回"生效摘要 + 保留消息"(内部 compact 问答天然
// 排除);压缩后新回合的 prepared 引用与投影同链(账实一致)。v2 场
// 报错不换。
// ---------------------------------------------------------------------------

// 压缩模型桩:回一份带 manifest 围栏的合格摘要。
class StubCompactClient : public lubancode::runtime::V3CompactModelClient {
public:
    lubancode::runtime::V3CompactModelReply Send(
        const std::string& system, const std::vector<nlohmann::json>& messages) override {
        (void)system;
        (void)messages;
        lubancode::runtime::V3CompactModelReply reply;
        reply.ok = true;
        reply.text = "# 交接摘要\n- 用户看了入口,工具读出了 main 函数。\n```json\n"
                     "{\"goal\": \"看入口\", \"constraints\": [], \"open_items\": [], "
                     "\"next_action\": \"继续\"}\n```\n";
        reply.usage = nlohmann::json::object({{"inputTokens", 90}, {"outputTokens", 12}});
        return reply;
    }
};

TEST_CASE("D3: compact applied 后 ProjectV3ContextHistory 投影新链,prepared 同链") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("d3-swap");
    TrajectorySessionLedger::Options options = LedgerOptions(root);
    options.v3_system_content = "SYSTEM-BASE";
    auto ledger = TrajectorySessionLedger::Open(options);
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream = V3StreamOf(*ledger);
    auto* writer = ledger->v3_main_writer();
    REQUIRE(writer != nullptr);

    // 一轮大块对话(材料给足体量,压缩才有收益)。
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage(std::string(6000, 'u')));
        const std::string request_id =
            bridge->OnRequestPrepared(MakeRequest("SYSTEM-REAL", {UserMessage(std::string(6000, 'u'))}),
                                      PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        bridge->OnRequestSent(request_id);
        bridge->OnUsageRecorded(request_id, SampleUsage(), /*reported_by_provider=*/true,
                                "resp-d3", 0, true, false);
        REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText(std::string(6000, 'a')),
                                          "end_turn", "resp-d3"));
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    }

    // 全链压缩:applied 落稳(writer 内存链已换,账侧同卷)。
    StubCompactClient client;
    lubancode::runtime::V3CompactProfile profile;
    profile.provider = "moonshot";
    profile.wire = "openai-chat-completions";
    profile.model = "kimi-k2.6";
    profile.compact_window_tokens = 0;  // 门禁关:不掺容量变量
    lubancode::runtime::V3CompactRunInput input;
    input.trigger = "manual";
    input.reason = "user_command";
    const auto result = lubancode::runtime::RunV3Compact(*writer, client, profile,
                                                          std::move(input));
    REQUIRE(result.applied);

    // 投影:摘要接 system,原对话退出模型上下文;内部 compact 问答不在。
    auto projected = ledger->ProjectV3ContextHistory();
    REQUIRE(projected.has_value());
    REQUIRE(projected->size() == 1);  // 全量压缩:链 = system + 摘要
    CHECK(projected->front().role == api::Role::User);
    bool summary_text = false;
    for (const auto& block : projected->front().content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            summary_text = text->text.find("交接摘要") != std::string::npos;
        }
    }
    CHECK(summary_text);

    // 压缩后新回合:prepared 的 inputMessageRefs 与投影同链(账实一致,
    // C13 的账侧断言)。
    std::string summary_ref = writer->context().chain[1].message_ref;
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-2", "external_user");
        bridge->RecordInput(UserMessage("新输入"));
        const std::string request_id =
            bridge->OnRequestPrepared(MakeRequest("SYSTEM-REAL", {UserMessage("新输入")}),
                                      PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        const auto rows = ReadLines(stream);
        std::vector<std::string> refs;
        for (const auto& row : rows) {
            if (row.value("kind", "") == "model.request.prepared" &&
                row.value("requestId", "") == request_id) {
                for (const auto& ref : row["payload"]["inputMessageRefs"]) {
                    refs.push_back(ref.get<std::string>());
                }
            }
        }
        REQUIRE(refs.size() == 2);
        CHECK(refs[0] == summary_ref);  // 摘要在前,旧史不携
        // 第二枚 = 新输入消息(账上刚落的 user 行)。
        std::string new_user_ref;
        for (const auto& row : rows) {
            if (row.value("type", "") == "message" &&
                row.value("purpose", "") == "conversation" &&
                row.at("message").value("role", "") == "user" &&
                row.at("message").at("content").dump().find("新输入") != std::string::npos) {
                new_user_ref = row.value("messageId", "");
            }
        }
        CHECK(refs[1] == new_user_ref);
        // 不收口这一轮:assistant 未落,prepared 断言已足。
    }
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

TEST_CASE("D3: v2 场 ProjectV3ContextHistory 报错不换") {
    EnvGuard v2pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "0");
    const auto root = FreshRoot("d3-v2");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    CHECK(ledger->v3_main_writer() == nullptr);
    const auto projected = ledger->ProjectV3ContextHistory();
    CHECK_FALSE(projected.has_value());
    CHECK(projected.error().rfind("compact.swap.not_v3", 0) == 0);
}

// ---------------------------------------------------------------------------
// P1-B(失败与恢复单 FA-02):错误结果往返不变——回喂语义以 Hook 处理后
// 真正交给模型的结果为准,写进最终 tool 消息本体,投影原样还原;不从
// 原始执行终态猜。
// ---------------------------------------------------------------------------

namespace {

// 结果消息带 is_error 开关(默认 ToolResults 只有成功形)。
api::Message ToolResultsWithError(const std::string& call_id, const std::string& content, bool is_error) {
    api::Message message;
    message.role = api::Role::User;
    api::ToolResultBlock result;
    result.tool_use_id = call_id;
    result.content = content;
    result.is_error = is_error;
    message.content.push_back(std::move(result));
    return message;
}

// 执行终态与回喂结果可配置的一轮工具流(与 DriveToolTurn 同形)。
TurnFlow DriveToolTurnWithOutcome(TrajectoryTurnBridge& bridge, const std::string& system,
                                  const std::string& call_id, agent::ToolOutcome execution_outcome,
                                  const api::Message& results_message,
                                  runtime::ToolResultsCommitReceipt* receipt_out = nullptr) {
    bridge.BeginTurn("turn-1", "external_user");
    bridge.RecordInput(UserMessage("按这份结果继续。"));
    const std::string request_id =
        bridge.OnRequestPrepared(MakeRequest(system, {UserMessage("按这份结果继续。")}), PreparedContext());
    REQUIRE_FALSE(request_id.empty());
    bridge.OnRequestSent(request_id);
    bridge.OnUsageRecorded(request_id, SampleUsage(), /*reported_by_provider=*/true, "resp-t", 0, true,
                           false);
    REQUIRE(bridge.OnOutputCompleted(request_id, AssistantWithToolCall(call_id), "tool_calls", "resp-t"));
    bridge.OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, call_id));
    agent::ToolTraceEvent started = TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, call_id);
    started.outcome = agent::ToolOutcome::Succeeded;
    bridge.OnToolTrace(started);
    agent::ToolTraceEvent finished = TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, call_id);
    finished.outcome = execution_outcome;
    finished.duration_ms = 42;
    if (execution_outcome != agent::ToolOutcome::Succeeded) {
        finished.error_code = "tool_blew_up";
    } else {
        finished.details = nlohmann::json{{"exit_code", 0}};
    }
    bridge.OnToolTrace(finished);
    const auto receipt = bridge.OnToolResultsCommitted("batch-1", results_message);
    if (receipt_out != nullptr) {
        *receipt_out = receipt;
    }
    bridge.EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    return TurnFlow{request_id};
}

// 折出投影里的 tool 结果块(按 call_id 找;找不到给 nullopt)。
std::optional<api::ToolResultBlock> FindProjectedResult(const std::vector<api::Message>& history,
                                                        const std::string& call_id) {
    for (const auto& message : history) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block);
                result != nullptr && result->tool_use_id == call_id) {
                return *result;
            }
        }
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("P1-B 错误往返: 执行成功但回喂 is_error=true,选用 failed、消息本体带标记、投影还原") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("p1b-error-roundtrip");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream = V3StreamOf(*ledger);
    const std::string call_id = "call_err_flip";

    runtime::ToolResultsCommitReceipt receipt;
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        // 执行终态 Succeeded,回喂结果却是错误(Hook 改写/清洗后的最终语义)。
        DriveToolTurnWithOutcome(*bridge, "SYSTEM-P1B", call_id, agent::ToolOutcome::Succeeded,
                                 ToolResultsWithError(call_id, "文件读不出来:磁盘报错", /*is_error=*/true),
                                 &receipt);
    }
    CHECK(receipt.status == runtime::ToolResultsCommitReceipt::Status::Committed);
    // 投影还原(先投影后关账,投影自己重读盘):is_error 不丢、正文不变。
    const auto replay = ledger->ProjectV3ContextHistory();
    REQUIRE(replay.has_value());
    CHECK(ledger->CloseSession("exit").error_code.empty());

    const auto rows = ReadLines(stream);
    // 选用口径以回喂为准:effectiveOutcome=failed(不从执行终态猜 done)。
    bool saw_selected_failed = false;
    const nlohmann::json* tool_message = nullptr;
    for (const auto& row : rows) {
        if (row.value("kind", std::string()) == "tool.result.selected") {
            saw_selected_failed = row.at("payload").value("effectiveOutcome", std::string()) == "failed";
        }
        if (row.value("type", std::string()) == "message" && row.contains("message") &&
            row.at("message").value("role", std::string()) == "tool") {
            tool_message = &row;
        }
    }
    CHECK(saw_selected_failed);
    REQUIRE(tool_message != nullptr);
    // 消息本体带 is_error=true(只写真值;读取侧 contains() 认键)。
    REQUIRE(tool_message->at("message").contains("is_error"));
    CHECK(tool_message->at("message").at("is_error").get<bool>());

    const auto projected = FindProjectedResult(*replay, /*call_id=*/"action-000001");
    REQUIRE(projected.has_value());
    CHECK(projected->is_error);
    CHECK(projected->content == "文件读不出来:磁盘报错");
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

TEST_CASE("P1-B 错误往返: 执行失败但回喂成功,当场与恢复输入均取最终结果") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("p1b-success-roundtrip");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream = V3StreamOf(*ledger);
    const std::string call_id = "call_ok_flip";

    runtime::ToolResultsCommitReceipt receipt;
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        // 执行终态 ToolError,Hook 改写后的最终回喂是成功结果。
        DriveToolTurnWithOutcome(*bridge, "SYSTEM-P1B2", call_id, agent::ToolOutcome::ToolError,
                                 ToolResults(call_id, "降级读到了:缓存副本"), &receipt);
    }
    CHECK(receipt.status == runtime::ToolResultsCommitReceipt::Status::Committed);
    const auto replay = ledger->ProjectV3ContextHistory();
    REQUIRE(replay.has_value());
    CHECK(ledger->CloseSession("exit").error_code.empty());

    const auto rows = ReadLines(stream);
    bool saw_selected_done = false;
    const nlohmann::json* tool_message = nullptr;
    for (const auto& row : rows) {
        if (row.value("kind", std::string()) == "tool.result.selected") {
            saw_selected_done = row.at("payload").value("effectiveOutcome", std::string()) == "done";
        }
        if (row.value("type", std::string()) == "message" && row.contains("message") &&
            row.at("message").value("role", std::string()) == "tool") {
            tool_message = &row;
        }
    }
    CHECK(saw_selected_done);  // 不从执行终态猜 failed
    REQUIRE(tool_message != nullptr);
    // is_error=false 不落键(写侧只写真值)——缺键即成功,旧账两读法兼容。
    CHECK_FALSE(tool_message->at("message").contains("is_error"));
    const auto projected = FindProjectedResult(*replay, "action-000001");
    REQUIRE(projected.has_value());
    CHECK_FALSE(projected->is_error);
    CHECK(projected->content == "降级读到了:缓存副本");
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

TEST_CASE("P1-B 错误往返: 同一 assistant 多枚调用成功失败混排,回放不串号不换序不丢标记") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("p1b-mixed");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path stream = V3StreamOf(*ledger);
    const std::string first_call = "call_mix_a";
    const std::string second_call = "call_mix_b";

    runtime::ToolResultsCommitReceipt receipt;
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("两份都查一下。"));
        api::Message assistant;
        assistant.role = api::Role::Assistant;
        assistant.content.push_back(api::TextBlock{"分头查。"});
        for (const std::string& call_id : {first_call, second_call}) {
            api::ToolUseBlock call;
            call.id = call_id;
            call.name = "read_file";
            call.input = nlohmann::json{{"path", "a.cpp"}};
            assistant.content.push_back(std::move(call));
        }
        const std::string request_id =
            bridge->OnRequestPrepared(MakeRequest("SYSTEM-MIX", {UserMessage("两份都查一下。")}),
                                      PreparedContext());
        REQUIRE_FALSE(request_id.empty());
        bridge->OnRequestSent(request_id);
        REQUIRE(bridge->OnOutputCompleted(request_id, assistant, "tool_calls", "resp-mix"));
        for (const std::string& call_id : {first_call, second_call}) {
            bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, call_id));
            agent::ToolTraceEvent started =
                TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, call_id);
            started.outcome = agent::ToolOutcome::Succeeded;
            bridge->OnToolTrace(started);
            agent::ToolTraceEvent finished =
                TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, call_id);
            finished.outcome = agent::ToolOutcome::Succeeded;
            finished.duration_ms = 10;
            finished.details = nlohmann::json{{"exit_code", 0}};
            bridge->OnToolTrace(finished);
        }
        api::Message results;
        results.role = api::Role::User;
        api::ToolResultBlock ok_result;
        ok_result.tool_use_id = first_call;
        ok_result.content = "第一份:正常";
        api::ToolResultBlock err_result;
        err_result.tool_use_id = second_call;
        err_result.content = "第二份:炸了";
        err_result.is_error = true;
        results.content.push_back(std::move(ok_result));
        results.content.push_back(std::move(err_result));
        receipt = bridge->OnToolResultsCommitted("batch-1", results);
        bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
    }
    REQUIRE(receipt.status == runtime::ToolResultsCommitReceipt::Status::Committed);
    const auto replay = ledger->ProjectV3ContextHistory();
    REQUIRE(replay.has_value());
    CHECK(ledger->CloseSession("exit").error_code.empty());

    // 投影:两枚结果都在、次序与声明序一致、配对键不串、错误标记不丢。
    std::vector<std::pair<std::string, bool>> seen;
    for (const auto& message : *replay) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                seen.emplace_back(result->tool_use_id, result->is_error);
            }
        }
    }
    REQUIRE(seen.size() == 2);
    // 投影配对键是 actionId(writer 发号,声明序即 action 序),两枚互不相等。
    CHECK(seen[0].first != seen[1].first);
    CHECK_FALSE(seen[0].second);  // 第一枚成功
    CHECK(seen[1].second);        // 第二枚错误,标记不丢
    CHECK(lubancode::trajectory::v3::VerifyV3File(stream).ok);
}

TEST_CASE("P1-B wire 对照: 会编码错误标记的 adapter 恢复前后 wire 字段一致") {
    // FA-02 第 4 条:JSONL 校验成功不能替代 wire 字段断言——anthropic 的
    // tool_result 会把 is_error 编码进 wire,同一条结果消息(当场回喂的
    // 那份 vs 恢复投影回来的那份)过同一只拼装器,对应 wire 字段必须一致。
    const std::string content = "命令没跑起来:exit 127";
    for (const bool is_error : {false, true}) {
        CAPTURE(is_error);
        api::Message live;
        live.role = api::Role::User;
        api::ToolResultBlock live_result;
        live_result.tool_use_id = "call_wire";
        live_result.content = content;
        live_result.is_error = is_error;
        live.content.push_back(live_result);

        api::Message restored = live;  // 投影回来的形状:同 content、同标记
        // (配对键由投影换成 actionId 属预期;此处对照的是错误标记的 wire
        // 编码,不对照键名。)
        restored.content.front() = api::ToolResultBlock{"action-000001", content, is_error};

        const auto wire_of = [](const api::Message& message) {
            api::Request request;
            request.model = "test-model";
            request.messages.push_back(message);
            const auto body = api::anthropic::BuildRequestJson(request);
            // doctest 禁 && 进 REQUIRE(分解器禁逻辑与),形状校验折成布尔再断。
            const bool has_messages = body.contains("messages") && body["messages"].is_array() &&
                                      !body["messages"].empty();
            REQUIRE(has_messages);
            const bool has_content = body["messages"][0].contains("content") &&
                                     body["messages"][0]["content"].is_array() &&
                                     !body["messages"][0]["content"].empty();
            REQUIRE(has_content);
            return body["messages"][0]["content"][0];
        };
        const auto live_wire = wire_of(live);
        const auto restored_wire = wire_of(restored);
        CHECK(live_wire.value("type", std::string()) == "tool_result");
        CHECK(restored_wire.value("type", std::string()) == "tool_result");
        CHECK(live_wire.value("content", std::string()) == content);
        CHECK(restored_wire.value("content", std::string()) == content);
        // is_error 的 wire 编码:anthropic 只写真值;两份必须同形。
        CHECK(live_wire.contains("is_error") == is_error);
        CHECK(restored_wire.contains("is_error") == is_error);
        if (is_error) {
            CHECK(live_wire["is_error"].get<bool>());
            CHECK(restored_wire["is_error"].get<bool>());
        }
    }
}
