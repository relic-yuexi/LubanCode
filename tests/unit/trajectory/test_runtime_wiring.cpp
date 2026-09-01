// P0-2 运行时单一写口的接线测试:轮次边界桥(TrajectoryTurnBridge)把
// loop 边界 + hub 工具栅栏翻成 trajectory 事件、闸前拒绝的调用落
// cancelled、TrajectorySessionLedger 开账与子代理独立 JSONL、flag 合成。
// 全部走真 recorder + 临时目录,末尾 VerifyJournalFile 验账。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/tool_trace.hpp"
#include "runtime/session_runtime.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"
#include "workspace/identity.hpp"

using namespace lubancode;
using namespace lubancode::runtime;
using trajectory::Actor;
using trajectory::Durability;
using trajectory::EventKind;
using trajectory::RecordReceipt;

namespace {

struct EnvGuard {
    explicit EnvGuard(const char* name) : name_(name) {}
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value.c_str(), 1);
#endif
    }
    const char* name_;
};

std::filesystem::path FreshDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

trajectory::EventScope MainScope(const std::string& run_id = "main-0001") {
    trajectory::EventScope scope;
    scope.workspace_key = "demo-000000000000";
    scope.session_id = "20260831-000001-AAAAAA";
    scope.run_id = run_id;
    scope.run_kind = trajectory::RunKind::MainSession;
    scope.visibility = {trajectory::Visibility::HostOnly};
    return scope;
}

TrajectoryTurnBridge OpenBridge(trajectory::TrajectoryRecorder& recorder) {
    TrajectoryTurnBridge::Identity identity{"demo", "responses", "terminal"};
    return TrajectoryTurnBridge(recorder, MainScope(), identity);
}

std::vector<std::string> KindsOf(const std::filesystem::path& stream) {
    std::vector<std::string> kinds;
    const auto lines = trajectory::ReadJournalLines(stream);
    if (!lines.has_value()) {
        return kinds;
    }
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded()) {
            kinds.push_back("<bad>");
            continue;
        }
        kinds.push_back(parsed.value("kind", std::string()));
    }
    return kinds;
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantWithToolCall(const std::string& call_id) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{"我先读文件。"});
    api::ToolUseBlock call;
    call.id = call_id;
    call.name = "read_file";
    call.input = nlohmann::json{{"path", "README.md"}};
    message.content.push_back(std::move(call));
    return message;
}

agent::ToolTraceEvent TraceEvent(agent::ToolTraceEventKind kind, const std::string& call_id) {
    agent::ToolTraceEvent event;
    event.kind = kind;
    event.execution_id = "item-1";
    event.tool_use_id = call_id;
    event.tool_name = "read_file";
    event.batch_id = "batch-1";
    event.sequence_in_batch = 0;
    event.timestamp_ms = 1759000000000LL;
    if (kind == agent::ToolTraceEventKind::ExecutionStarted) {
        event.effective_input_sha256 = std::string(64, '0');
        event.effect_class = agent::EffectClass::ReadOnlyLocal;
        event.effective_arguments = nlohmann::json{{"path", "README.md"}};
    } else if (kind == agent::ToolTraceEventKind::ExecutionFinished) {
        event.outcome = agent::ToolOutcome::Succeeded;
        event.duration_ms = 18;
        event.result_ref.kind = agent::ToolResultRef::Kind::Inline;
        event.result_ref.sha256 = std::string(64, '1');
        event.result_ref.bytes = 9;
    }
    return event;
}

}  // namespace

TEST_CASE("状态机补丁:cancelled 不须 started,failed 仍须") {
    const auto dir = FreshDir("lubancode-traj-p2-cancel");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(), [] {
        trajectory::RecorderOptions options;
        options.event_schema_version = 2;  // v2:usage 走 model.usage.recorded owner
        return options;
    }());
    REQUIRE(recorder.has_value());
    REQUIRE(recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}},
                                      Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    auto bridge = OpenBridge(*recorder);
    bridge.BeginTurn("turn-1", "external_user");
    bridge.RecordInput(UserMessage("读一下"));
    const std::string request_id = bridge.OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    REQUIRE_FALSE(request_id.empty());
    bridge.OnRequestSent(request_id);
    REQUIRE(bridge.OnOutputCompleted(request_id, AssistantWithToolCall("call-1"), "tool_use", "resp-1"));
    // 闸前拒绝:planned 后直接 cancelled,没有 effective/started。
    agent::ToolTraceEvent planned = TraceEvent(agent::ToolTraceEventKind::Scheduled, "call-1");
    bridge.OnToolTrace(planned);
    agent::ToolTraceEvent denied = TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, "call-1");
    denied.outcome = agent::ToolOutcome::PermissionDeclined;
    denied.error_code = "permission.declined";
    bridge.OnToolTrace(denied);
    // 拒绝也有回喂模型的 tool_result(文案),committed 须能落。
    api::Message results;
    results.role = api::Role::User;
    results.content.push_back(api::ToolResultBlock{"call-1", "用户拒绝执行该工具", true});
    bridge.OnToolResultsCommitted("batch-1", results);
    bridge.EndTurn(true, false, "done");

    const auto report = trajectory::VerifyJournalFile(dir / "main.jsonl");
    REQUIRE(report.ok);
    const auto kinds = KindsOf(dir / "main.jsonl");
    // run.started, turn.started, input, prepared, sent, output,
    // planned, cancelled, result.committed, turn.completed
    REQUIRE(kinds.size() == 10);
    CHECK(kinds[7] == "tool.execution.cancelled");
    CHECK(kinds[9] == "turn.completed");
}

TEST_CASE("bridge 一轮全流:请求/输出/工具三道栅栏齐,verify 过") {
    const auto dir = FreshDir("lubancode-traj-p2-full");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(), [] {
        trajectory::RecorderOptions options;
        options.event_schema_version = 2;  // v2:usage 走 model.usage.recorded owner
        return options;
    }());
    REQUIRE(recorder.has_value());
    REQUIRE(recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}},
                                      Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    auto bridge = OpenBridge(*recorder);
    bridge.BeginTurn("turn-1", "external_user");
    bridge.RecordInput(UserMessage("读一下 README 并数行数"));

    // 第一请求:输出带工具。
    const std::string req1 = bridge.OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    REQUIRE_FALSE(req1.empty());
    bridge.OnRequestSent(req1);
    api::Usage usage;
    usage.input_tokens = 128;
    usage.output_tokens = 64;
    bridge.OnUsageRecorded(req1, usage, true, "resp-1");
    REQUIRE(bridge.OnOutputCompleted(req1, AssistantWithToolCall("call-1"), "tool_use", "resp-1"));

    bridge.OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, "call-1"));
    bridge.OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, "call-1"));
    bridge.OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, "call-1"));
    api::Message results;
    results.role = api::Role::User;
    results.content.push_back(api::ToolResultBlock{"call-1", "共 42 行。", false});
    bridge.OnToolResultsCommitted("batch-1", results);

    // 第二请求:纯文本收口。
    const std::string req2 = bridge.OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    REQUIRE_FALSE(req2.empty());
    bridge.OnRequestSent(req2);
    REQUIRE(bridge.OnOutputCompleted(req2, UserMessage("不是 user,这里只是复用文本形状"), "end_turn", "resp-2"));
    bridge.EndTurn(true, false, "done");

    const auto report = trajectory::VerifyJournalFile(dir / "main.jsonl");
    REQUIRE(report.ok);
    const auto kinds = KindsOf(dir / "main.jsonl");
    REQUIRE(kinds.size() == 16);
    CHECK(kinds[0] == "run.started");
    CHECK(kinds[1] == "turn.started");
    CHECK(kinds[2] == "input.received");
    CHECK(kinds[3] == "model.request.prepared");
    CHECK(kinds[4] == "model.request.sent");
    CHECK(kinds[5] == "model.usage.recorded");
    CHECK(kinds[6] == "model.output.completed");
    CHECK(kinds[7] == "tool.execution.planned");
    CHECK(kinds[8] == "tool.input.effective");
    CHECK(kinds[9] == "tool.execution.started");
    CHECK(kinds[10] == "tool.execution.finished");
    CHECK(kinds[11] == "tool.result.committed");
    CHECK(kinds[12] == "model.request.prepared");
    CHECK(kinds[13] == "model.request.sent");
    CHECK(kinds[14] == "model.output.completed");
    CHECK(kinds[15] == "turn.completed");

    // tool.result.committed 的正文来自消息(derived_from 指向终态事件)。
    const auto lines = trajectory::ReadJournalLines(dir / "main.jsonl");
    REQUIRE(lines.has_value());
    const auto committed = nlohmann::json::parse(lines->at(11));
    CHECK(committed["payload"]["content"][0]["text"] == "共 42 行。");
    CHECK(committed["payload"]["is_error"] == false);
    CHECK(committed["payload"].contains("derived_from_event"));
}

TEST_CASE("ledger:开账出 main.jsonl,子代理拿独立 JSONL 与父边界") {
    const auto root = FreshDir("lubancode-traj-p2-ledger");
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.lubancode_version = "test";
    std::error_code ec;
    std::filesystem::create_directories(root / "repo", ec);
    auto ledger = TrajectorySessionLedger::Open(options);
    REQUIRE(ledger.has_value());
    CHECK(std::filesystem::exists(ledger->session_dir() / "main.jsonl"));
    CHECK_FALSE(ledger->session_id().empty());

    // 主轮桥挂上,派一只子代理。
    auto main_bridge = ledger->NewTurnBridge({"demo", "responses", "terminal"});
    REQUIRE(main_bridge != nullptr);
    auto child = ledger->SpawnSubagent("toolu-1", "读文件并数行数");
    REQUIRE(child.has_value());
    CHECK_FALSE((*child)->run_id().empty());

    // 子账:自己的 run.started 首事件,一轮 + 终态封口。
    auto& child_bridge = (*child)->turn_bridge();
    child_bridge.BeginTurn("turn-1", "external_user");
    child_bridge.RecordInput(UserMessage("读文件并数行数"));
    const std::string req = child_bridge.OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    REQUIRE_FALSE(req.empty());
    child_bridge.OnRequestSent(req);
    REQUIRE(child_bridge.OnOutputCompleted(req, UserMessage("报告:42 行"), "end_turn", "resp-c"));
    child_bridge.EndTurn(true, false, "done");
    const std::string terminal_hash = (*child)->Finish(true, "done");
    CHECK_FALSE(terminal_hash.empty());

    // 子文件独立存在,hash chain 完整。
    const auto sub_path = ledger->session_dir() / "subagents" / ((*child)->run_id() + ".jsonl");
    REQUIRE(std::filesystem::exists(sub_path));
    const auto sub_report = trajectory::VerifyJournalFile(sub_path);
    REQUIRE(sub_report.ok);
    const auto sub_kinds = KindsOf(sub_path);
    CHECK(sub_kinds.front() == "run.started");
    CHECK(sub_kinds.back() == "run.completed");

    // 父账:边界引用落在 agent 调用的执行终态上。
    main_bridge->BeginTurn("turn-1", "external_user");
    main_bridge->RecordInput(UserMessage("去读文件"));
    const std::string parent_req = main_bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    main_bridge->OnRequestSent(parent_req);
    api::Message with_agent = AssistantWithToolCall("toolu-1");
    std::get<api::ToolUseBlock>(with_agent.content.back()).name = "agent";
    REQUIRE(main_bridge->OnOutputCompleted(parent_req, with_agent, "tool_use", "resp-p"));
    main_bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, "toolu-1"));
    main_bridge->AttachChildRun("toolu-1", (*child)->run_id());
    main_bridge->NoteChildTerminal((*child)->run_id(), terminal_hash);
    agent::ToolTraceEvent started = TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, "toolu-1");
    started.tool_name = "agent";
    main_bridge->OnToolTrace(started);
    agent::ToolTraceEvent finished = TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, "toolu-1");
    finished.tool_name = "agent";
    main_bridge->OnToolTrace(finished);
    api::Message agent_result;
    agent_result.role = api::Role::User;
    agent_result.content.push_back(api::ToolResultBlock{"toolu-1", "报告:42 行", false});
    main_bridge->OnToolResultsCommitted("batch-1", agent_result);
    main_bridge->EndTurn(true, false, "done");

    const auto main_report = trajectory::VerifyJournalFile(ledger->session_dir() / "main.jsonl");
    REQUIRE(main_report.ok);
    const auto lines = trajectory::ReadJournalLines(ledger->session_dir() / "main.jsonl");
    REQUIRE(lines.has_value());
    bool saw_child_edge = false;
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || parsed.value("kind", std::string()) != "tool.execution.finished") {
            continue;
        }
        // 边界引用:relations.child_run_id + result_ref 的子账终态 hash。
        if (parsed.value("relations", nlohmann::json::object()).value("child_run_id", std::string()) ==
                (*child)->run_id() &&
            parsed["payload"]["result_ref"].value("child_run_id", std::string()) == (*child)->run_id() &&
            parsed["payload"]["result_ref"].value("child_terminal_event_hash", std::string()) ==
                terminal_hash) {
            saw_child_edge = true;
        }
    }
    CHECK(saw_child_edge);
    // main.jsonl 不内联子账正文:子的 input 文本只出现在子文件。
    const auto main_text = [&] {
        std::ifstream file(ledger->session_dir() / "main.jsonl", std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }();
    CHECK(main_text.find("读文件并数行数") == std::string::npos);
    // main 一轮的事件数(只有父侧那一份事实)。
    const auto main_kinds = KindsOf(ledger->session_dir() / "main.jsonl");
    REQUIRE(main_kinds.size() == 12);
    CHECK(main_kinds[5] == "model.output.completed");
    CHECK(main_kinds[6] == "tool.execution.planned");
}

// 读一份子账 run.started 的 relations.parent_run_id(P1-2 嵌套轨迹边测试用)。
std::string ParentRunIdOf(const std::filesystem::path& stream) {
    const auto lines = trajectory::ReadJournalLines(stream);
    if (!lines.has_value() || lines->empty()) {
        return std::string();
    }
    const auto parsed = nlohmann::json::parse(lines->front(), nullptr, false);
    if (parsed.is_discarded()) {
        return std::string();
    }
    return parsed.value("relations", nlohmann::json::object()).value("parent_run_id", std::string());
}

TEST_CASE("SpawnSubagent:嵌套派工的 parent_run_id 指向父任务自己的 run,不冒充 main(P1-2)") {
    const auto root = FreshDir("lubancode-traj-p1-2-nested");
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.lubancode_version = "test";
    std::error_code ec;
    std::filesystem::create_directories(root / "repo", ec);
    auto ledger = TrajectorySessionLedger::Open(options);
    REQUIRE(ledger.has_value());

    // main 自己的 run id(main.jsonl 首行 run.started 的顶层 run_id)。
    const auto main_lines = trajectory::ReadJournalLines(ledger->session_dir() / "main.jsonl");
    REQUIRE(main_lines.has_value());
    REQUIRE_FALSE(main_lines->empty());
    const auto main_started = nlohmann::json::parse(main_lines->front(), nullptr, false);
    REQUIRE_FALSE(main_started.is_discarded());
    const std::string main_run_id = main_started.value("run_id", std::string());
    REQUIRE_FALSE(main_run_id.empty());

    // main 直派(parent_run_id 缺省 = 空串):relations.parent_run_id 落回
    // main_run_id——旧行为一字不改。
    auto direct_child = ledger->SpawnSubagent("toolu-1", "main 直派的孩子");
    REQUIRE(direct_child.has_value());
    const auto direct_path = ledger->session_dir() / "subagents" / ((*direct_child)->run_id() + ".jsonl");
    CHECK(ParentRunIdOf(direct_path) == main_run_id);

    // 嵌套派工:parent_run_id 显式传"直派孩子"自己的 run id——它是派出
    // 孙任务的那只子代理,relations.parent_run_id 必须认它,不能冒充 main
    //(单子 §12.3 第一条,"嵌套 headless 路的父亲是父任务的 run,不是 main")。
    auto grandchild = ledger->SpawnSubagent(/*parent_call_id=*/std::string(), "孙任务",
                                            (*direct_child)->run_id());
    REQUIRE(grandchild.has_value());
    CHECK((*grandchild)->run_id() != (*direct_child)->run_id());
    const auto grandchild_path = ledger->session_dir() / "subagents" / ((*grandchild)->run_id() + ".jsonl");
    const std::string grandchild_parent = ParentRunIdOf(grandchild_path);
    CHECK(grandchild_parent == (*direct_child)->run_id());
    CHECK(grandchild_parent != main_run_id);
}

TEST_CASE("SessionRuntime 轨迹档:恒开,旧档建档/轮末补抄路已删净") {
    // P0-2(Trajectory 升为唯一 Session):feature/env 开关已删,ledger 恒在;
    // P0-6:EnsureBegun/PersistNew/store 本体删除——旧路不复存在,这里只
    // 验轨迹账恒开。
    const auto root = FreshDir("lubancode-traj-p2-runtime");
    runtime::SessionRuntime::Options options;
    options.trajectory_workspace_identity = workspace::MakeFallbackIdentity(root / "repo");
    std::error_code ec;
    std::filesystem::create_directories(root / "repo", ec);
    runtime::SessionRuntime session(options);
    REQUIRE(session.trajectory() != nullptr);
    CHECK(std::filesystem::exists(session.trajectory()->session_dir() / "main.jsonl"));
}
