// 轨迹 v3 收尾棒:/export 与 /copy 的 v3 投影。
//   - FoldMainReplay 对 v3 场出真投影(ReadV3Ledger + ProjectModelContext 链
//     投影,EffectiveConversationFromV3 与 resume 共用),不再报
//     replay.v3_main_unavailable;ProjectHistoryFromReplay 照吃(/copy 路);
//   - ProjectExportMessages(纯函数):hidden 默认不导(§4.28)、压缩标记折
//     成分界位、started_at 取时间线首格;ExportSessionMarkdown 的既有 compact
//     文案在分界位渲染;
//   - 真账端到端:工具轮折进 <details>,system 根不进导出正文。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_history_view.hpp"
#include "runtime/trajectory_session.hpp"
#include "tools/session_utils.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
using namespace lubancode;
using lubancode::runtime::TrajectorySessionLedger;
using lubancode::runtime::TrajectoryTurnBridge;

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

std::filesystem::path FreshRoot(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-v3-export-copy-" + std::string(tag));
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
    options.lubancode_version = "0.26.238-test";
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

// 一轮带工具的回合(与 test_v3_write_wiring 的 DriveToolTurn 同形)。
void DriveToolTurn(TrajectoryTurnBridge& bridge, const std::string& system,
                   const std::string& user_text, const std::string& call_id) {
    bridge.BeginTurn("turn-1", "external_user");
    bridge.RecordInput(UserMessage(user_text));
    const std::string request_id =
        bridge.OnRequestPrepared(MakeRequest(system, {UserMessage(user_text)}), PreparedContext());
    REQUIRE_FALSE(request_id.empty());
    bridge.OnRequestSent(request_id);
    bridge.OnUsageRecorded(request_id, SampleUsage(), /*reported_by_provider=*/true, "resp-123", 0,
                           true, false);
    REQUIRE(bridge.OnOutputCompleted(request_id, AssistantWithToolCall(call_id), "tool_calls",
                                     "resp-123"));
    bridge.OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, call_id));
    agent::ToolTraceEvent started =
        TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, call_id);
    started.effective_arguments = nlohmann::json{{"path", "src/app/main.cpp"}};
    bridge.OnToolTrace(started);
    agent::ToolTraceEvent finished =
        TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, call_id);
    finished.outcome = agent::ToolOutcome::Succeeded;
    bridge.OnToolTrace(finished);
    bridge.OnToolResultsCommitted("batch-1", ToolResults(call_id, "int main(int argc, char** argv)"));
    bridge.EndTurn(/*ok=*/true, /*cancelled=*/false, "");
}

std::filesystem::path V3StreamOf(const TrajectorySessionLedger& ledger) {
    return ledger.session_dir() /
           platform::Utf8ToPath(platform::PathToUtf8(ledger.session_dir().filename()) + ".jsonl");
}

runtime::RestoredHistoryItem MessageItem(std::uint64_t seq, const std::string& ts,
                                         const std::string& text, bool hidden = false) {
    runtime::RestoredHistoryItem item;
    item.kind = runtime::RestoredHistoryItem::Kind::Message;
    item.seq = seq;
    item.timestamp = ts;
    item.message.message = UserMessage(text);
    item.message.hidden = hidden;
    return item;
}

runtime::RestoredHistoryItem CompactItem(std::uint64_t seq) {
    runtime::RestoredHistoryItem item;
    item.kind = runtime::RestoredHistoryItem::Kind::Compact;
    item.seq = seq;
    item.compact.compact_id = "compact-1";
    return item;
}

}  // namespace

// ---------------------------------------------------------------------------
// FoldMainReplay:v3 场出真投影(/copy 取数口)
// ---------------------------------------------------------------------------

TEST_CASE("v3 FoldMainReplay: 链投影出有效对话,/copy 路 ProjectHistoryFromReplay 照吃") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("fold");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string call_id = "call_prov_export";
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        DriveToolTurn(*bridge, "你是 LubanCode,读写跑都走工具。", "看看入口", call_id);
    }
    // clear 前的活场也能折(不必先封口)。
    const auto fold = ledger->FoldMainReplay();
    REQUIRE(fold.ok());
    CHECK(fold.error_code.empty());
    CHECK(fold.state.session_id == ledger->session_id());
    CHECK(fold.state.integrity.events_folded > 0);
    CHECK(fold.state.folded_seq > 0);
    // 有效对话:user → assistant(带 tool_call)→ tool。
    const auto& conversation = fold.state.effective_conversation;
    REQUIRE(conversation.size() == 3);
    CHECK(conversation[0].role == trajectory::ReplayMessage::Role::User);
    CHECK(conversation[1].role == trajectory::ReplayMessage::Role::Assistant);
    CHECK(conversation[2].role == trajectory::ReplayMessage::Role::Tool);
    // 工具配对键统一 actionId:assistant 调用块 call_id == tool 消息 call_id。
    std::string assistant_call_id;
    for (const auto& block : conversation[1].blocks) {
        if (block.value("type", std::string()) == "tool_call") {
            assistant_call_id = block.value("call_id", std::string());
        }
    }
    CHECK_FALSE(assistant_call_id.empty());
    CHECK(conversation[2].call_id.has_value());
    CHECK(*conversation[2].call_id == assistant_call_id);

    // /copy 路:ProjectHistoryFromReplay 把链投影翻成 api::Message。
    const auto history = runtime::ProjectHistoryFromReplay(fold.state);
    REQUIRE(history.size() == 3);
    bool has_tool_use = false;
    bool has_tool_result = false;
    for (const auto& block : history[1].content) {
        if (std::holds_alternative<api::ToolUseBlock>(block)) {
            has_tool_use = true;
        }
    }
    for (const auto& block : history[2].content) {
        if (const auto* result = std::get_if<api::ToolResultBlock>(&block);
            result != nullptr && result->tool_use_id == assistant_call_id) {
            has_tool_result = true;
        }
    }
    CHECK(has_tool_use);
    CHECK(has_tool_result);

    CHECK(ledger->CloseSession("exit").error_code.empty());
    // 封口后照样可折(resume/export 都用这口径)。
    const auto fold_after = ledger->FoldMainReplay();
    CHECK(fold_after.ok());
}

TEST_CASE("v3 FoldMainReplay: 验卷不过如实报错,不折半本") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("fold-bad");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    CHECK(ledger->CloseSession("exit").error_code.empty());
    const auto stream = V3StreamOf(*ledger);
    // 撕裂尾行(去掉末行换行再截半行):ReadV3Ledger 拒收。
    std::ifstream in(stream, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    content.resize(content.size() / 2);
    std::ofstream out(stream, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    const auto fold = ledger->FoldMainReplay();
    CHECK_FALSE(fold.ok());
    CHECK(fold.error_code == "replay.v3_ledger_failed");
    CHECK_FALSE(fold.message.empty());
}

// ---------------------------------------------------------------------------
// ProjectExportMessages:分界位/hidden/started_at(纯函数)
// ---------------------------------------------------------------------------

TEST_CASE("ProjectExportMessages: hidden 不导、压缩分界位、started_at 取首格") {
    runtime::RestoredHistoryView view;
    view.items.push_back(MessageItem(2, "2026-09-11T01:00:00.000Z", "第一问"));
    view.items.push_back(CompactItem(3));
    view.items.push_back(MessageItem(4, "2026-09-11T01:01:00.000Z", "内部回合", /*hidden=*/true));
    view.items.push_back(MessageItem(5, "2026-09-11T01:02:00.000Z", "第二问"));
    view.items.push_back(CompactItem(6));

    const auto projection = runtime::ProjectExportMessages(view);
    REQUIRE(projection.messages.size() == 2);  // hidden 不导
    CHECK(projection.compact_positions.size() == 2);
    CHECK(projection.compact_positions[0] == 1);  // 第一问之后、(隐藏消息之前)
    CHECK(projection.compact_positions[1] == 2);  // 第二问之后
    CHECK(projection.started_at == "2026-09-11T01:00:00.000Z");

    // 既有 compact 文案在分界位渲染(ExportSessionMarkdown 的既有行为)。
    tools::ExportSessionHeader header;
    const std::string markdown = tools::ExportSessionMarkdown(
        header, projection.messages, "sess-export", /*max_result_lines=*/30, std::string(),
        projection.compact_positions);
    const std::size_t first_q = markdown.find("第一问");
    const std::size_t divider_1 = markdown.find("⚡");
    const std::size_t second_q = markdown.find("第二问");
    const std::size_t divider_2 = markdown.find("⚡", divider_1 + 1);
    REQUIRE(first_q != std::string::npos);
    REQUIRE(second_q != std::string::npos);
    REQUIRE(divider_1 != std::string::npos);
    REQUIRE(divider_2 != std::string::npos);
    CHECK(first_q < divider_1);
    CHECK(divider_1 < second_q);
    CHECK(second_q < divider_2);
    CHECK(markdown.find("内部回合") == std::string::npos);  // hidden 不进导出正文
}

// ---------------------------------------------------------------------------
// 真账端到端:/export 的 v3 投影(ReadV3Ledger + ProjectHistoryTimeline)
// ---------------------------------------------------------------------------

TEST_CASE("v3 真账 export 投影: 工具轮折进 details,system 根不进正文") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("e2e");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::string call_id = "call_prov_e2e";
    {
        auto bridge = ledger->NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
        REQUIRE(bridge != nullptr);
        DriveToolTurn(*bridge, "你是 LubanCode,读写跑都走工具。", "看看入口", call_id);
    }
    CHECK(ledger->CloseSession("exit").error_code.empty());
    const auto stream = V3StreamOf(*ledger);

    const auto view = runtime::ProjectRestoredHistory(stream);
    CHECK_FALSE(view.items.empty());
    const auto projection = runtime::ProjectExportMessages(view);
    REQUIRE(projection.messages.size() == 3);  // user + assistant + tool 结果(user 携带)
    CHECK(projection.compact_positions.empty());  // 没压缩过,不造分界线
    CHECK_FALSE(projection.started_at.empty());

    tools::ExportSessionHeader header;
    header.started_at = projection.started_at;
    const std::string markdown =
        tools::ExportSessionMarkdown(header, projection.messages, ledger->session_id(),
                                     /*max_result_lines=*/30, std::string("导出标题"),
                                     projection.compact_positions);
    CHECK(markdown.find("# 导出标题") != std::string::npos);
    CHECK(markdown.find("看看入口") != std::string::npos);
    CHECK(markdown.find("## 助手") != std::string::npos);
    CHECK(markdown.find("工具调用: read_file") != std::string::npos);
    CHECK(markdown.find("int main(int argc, char** argv)") != std::string::npos);
    // system 根是上下文,不是会话正文,不进导出。
    CHECK(markdown.find("你是 LubanCode,读写跑都走工具。") == std::string::npos);
}
