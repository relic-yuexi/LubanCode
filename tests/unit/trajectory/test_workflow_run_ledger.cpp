// workflow 会话归属统一单:TrajectorySessionLedger 编排账(SpawnWorkflowRun/
// SpawnNodeStream 即 ReserveWorkflowRun/ReserveWorkflowNodeStream 的消费方)。
// 六场:
//   1. reserve→consume 全链:编排 Journal 只见编排事实;node 账收模型/工具
//      事件(ownership:声明才进、无主拒);verify 全过且父子边逐位对账。
//   2. 无主 tool trace:node 桥不造册不落账(与子代理同门)。
//   3. node 账开张失败:fail closed,无 0 字节残留,verify 过。
//   4. 编排账开张失败:fail closed,无 stream,verify 过。
//   5. retry 新开 node 文件:attempt 1 与 attempt 2 不共写。
//   6. 旧 workflow-runs/ 路兼容:照写照读,不迁移不炸。
//   7. hash 对账负例:编排记了假 hash,verify 报 edge.child_hash_mismatch。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"
#include "workflow/journal.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

std::filesystem::path FreshDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::optional<TrajectorySessionLedger> OpenLedger(
    const std::filesystem::path& root,
    std::function<std::optional<std::string>()> run_fault = {},
    std::function<std::optional<std::string>()> node_fault = {}) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.lubancode_version = "test";
    options.workflow_start_fault = std::move(run_fault);
    options.workflow_node_start_fault = std::move(node_fault);
    std::error_code ec;
    std::filesystem::create_directories(root / "repo", ec);
    auto ledger = TrajectorySessionLedger::Open(std::move(options));
    if (!ledger.has_value()) {
        return std::nullopt;
    }
    return std::move(*ledger);
}

TrajectoryWorkflowRunBridge::DefinitionInfo MakeDefinitionInfo() {
    TrajectoryWorkflowRunBridge::DefinitionInfo info;
    info.workflow_id = "probe-flow";
    info.workflow_version = "1.0.0";
    info.content_hash = std::string(64, 'a');
    info.cwd = "D:/some/repo";
    info.definition_json = R"({"id":"probe-flow","nodes":{}})";
    return info;
}

std::vector<std::string> KindsOf(const std::filesystem::path& stream) {
    std::vector<std::string> kinds;
    const auto lines = trajectory::ReadJournalLines(stream);
    if (!lines.has_value()) {
        return kinds;
    }
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        kinds.push_back(parsed.is_discarded() ? std::string("<bad>")
                                              : parsed.value("kind", std::string()));
    }
    return kinds;
}

nlohmann::json FirstEventOf(const std::filesystem::path& stream, const std::string& kind) {
    const auto lines = trajectory::ReadJournalLines(stream);
    if (!lines.has_value()) {
        return nlohmann::json();
    }
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (!parsed.is_discarded() && parsed.value("kind", std::string()) == kind) {
            return parsed;
        }
    }
    return nlohmann::json();
}

std::vector<std::string> NodeJsonlFiles(const TrajectorySessionLedger& ledger,
                                        const std::string& workflow_run_id) {
    std::vector<std::string> names;
    std::error_code ec;
    const auto dir = ledger.session_dir() / "workflows" /
                     std::filesystem::path(workflow_run_id) / "nodes";
    if (!std::filesystem::exists(dir, ec)) {
        return names;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") {
            names.push_back(entry.path().filename().generic_string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

// node 轮内的一枚工具事实(与子代理整合册同一形状;call 由模型输出声明)。
agent::ToolTraceEvent ToolEvent(agent::ToolTraceEventKind kind, const std::string& call_id) {
    agent::ToolTraceEvent event;
    event.kind = kind;
    event.execution_id = "exec-" + call_id;
    event.tool_use_id = call_id;
    event.tool_name = "read_file";
    event.timestamp_ms = 1759000000000LL;
    if (kind == agent::ToolTraceEventKind::ExecutionStarted) {
        event.effective_input_sha256 = std::string(64, '0');
        event.effect_class = agent::EffectClass::ReadOnlyLocal;
        event.effective_arguments = nlohmann::json{{"path", "a.txt"}};
    } else if (kind == agent::ToolTraceEventKind::ExecutionFinished) {
        event.outcome = agent::ToolOutcome::Succeeded;
        event.duration_ms = 12;
        event.result_ref.kind = agent::ToolResultRef::Kind::Inline;
        event.result_ref.sha256 = std::string(64, '2');
        event.result_ref.bytes = 30;
    }
    return event;
}

// 把一次 node attempt 的模型/工具边界灌进 node 桥(与 host executor 的
// 接线同构:BeginTurn → input → prepared/sent/output(声明 tool call)→
// trace 三拍 → results → EndTurn)。
std::string DriveNodeAttempt(TrajectoryWorkflowNodeBridge& node, const std::string& call_id,
                             bool ok = true, const std::string& output_text = "节点结论") {
    auto& turn = node.turn_bridge();
    turn.BeginTurn("turn-1", "scheduled_host");
    api::Message input;
    input.role = api::Role::User;
    input.content.push_back(api::TextBlock{"干这一节点"});
    turn.RecordInput(input);
    const std::string request_id = turn.OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    REQUIRE_FALSE(request_id.empty());
    turn.OnRequestSent(request_id);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{ok ? output_text : "半截"});
    if (ok) {
        api::ToolUseBlock call;
        call.id = call_id;
        call.name = "read_file";
        call.input = nlohmann::json{{"path", "a.txt"}};
        assistant.content.push_back(std::move(call));
    }
    REQUIRE(turn.OnOutputCompleted(request_id, assistant, ok ? "tool_use" : "end_turn", "resp-n"));
    if (ok) {
        turn.OnToolTrace(ToolEvent(agent::ToolTraceEventKind::Scheduled, call_id));
        turn.OnToolTrace(ToolEvent(agent::ToolTraceEventKind::ExecutionStarted, call_id));
        turn.OnToolTrace(ToolEvent(agent::ToolTraceEventKind::ExecutionFinished, call_id));
        api::Message results;
        results.role = api::Role::User;
        results.content.push_back(api::ToolResultBlock{call_id, "30 字节", false});
        turn.OnToolResultsCommitted("batch-node", results);
    }
    turn.EndTurn(ok, false, ok ? std::string() : "model_refused");
    return node.Finish(ok, false, ok ? std::string() : "model_refused");
}

}  // namespace

TEST_CASE("编排 1:reserve→consume 全链——编排账只见编排,node 账收正文,verify 全过") {
    auto opened = OpenLedger(FreshDir("lubancode-traj-wf-full"));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger ledger = std::move(*opened);

    auto run = ledger.SpawnWorkflowRun("wf-run-0001", MakeDefinitionInfo());
    REQUIRE(run.has_value());
    CHECK((*run)->run_id() == "wf-run-0001");

    // node 派发 + attempt 账 + 终态(编排事实带 child hash)。
    REQUIRE((*run)->RecordNodeDispatched("gather", "wf-run-0001-gather-d1-a1", 1, "agent", -1,
                                         std::string(64, 'e')));
    auto node = (*run)->SpawnNodeStream("gather", "wf-run-0001-gather-d1-a1", 1, "agent", -1);
    REQUIRE(node.has_value());
    const std::string hash = DriveNodeAttempt(**node, "toolu-node-1");
    REQUIRE_FALSE(hash.empty());
    REQUIRE((*run)->RecordNodeCompleted("gather", "wf-run-0001-gather-d1-a1", 1, "success", 120, 7,
                                        std::string(), std::string(64, 'f'), hash));
    CHECK_FALSE((*run)->broken());
    CHECK_FALSE((*run)->Finish(true, false, std::string()).empty());

    // 目录形状(§3.6):workflow.jsonl + definition.json + nodes/。
    const auto run_dir = ledger.session_dir() / "workflows" / "wf-run-0001";
    CHECK(std::filesystem::exists(run_dir / "definition.json"));
    const auto orchestration = KindsOf(run_dir / "workflow.jsonl");
    REQUIRE(orchestration.size() >= 5);
    CHECK(orchestration.front() == "run.started");
    CHECK(orchestration.back() == "run.completed");
    // 编排 Journal 只见编排事实:node 内幕(模型/工具)一枚不进。
    for (const std::string& kind : orchestration) {
        CHECK(kind.rfind("model.", 0) != 0);
        CHECK(kind.rfind("tool.", 0) != 0);
        CHECK(kind != "input.received");
        CHECK(kind != "turn.started");
    }
    CHECK(std::find(orchestration.begin(), orchestration.end(), "workflow.definition.loaded") !=
          orchestration.end());
    CHECK(std::find(orchestration.begin(), orchestration.end(), "workflow.node.dispatched") !=
          orchestration.end());
    CHECK(std::find(orchestration.begin(), orchestration.end(), "workflow.node.completed") !=
          orchestration.end());
    // 派发事实带 child 引用;终态事实带 child hash。
    const auto dispatched = FirstEventOf(run_dir / "workflow.jsonl", "workflow.node.dispatched");
    CHECK(dispatched["relations"].value("child_run_id", std::string()) ==
          "wf-run-0001-gather-d1-a1");
    const auto completed = FirstEventOf(run_dir / "workflow.jsonl", "workflow.node.completed");
    CHECK(completed["payload"].value("child_terminal_event_hash", std::string()) == hash);

    // node 账:首行身份齐全(§3.6),模型/工具正文都在。
    const auto node_files = NodeJsonlFiles(ledger, "wf-run-0001");
    REQUIRE(node_files.size() == 1);
    CHECK(node_files[0] == "wf-run-0001-gather-d1-a1.jsonl");
    const auto node_kinds = KindsOf(run_dir / "nodes" / node_files[0]);
    REQUIRE(node_kinds.size() >= 10);
    CHECK(node_kinds.front() == "run.started");
    CHECK(node_kinds.back() == "run.completed");
    CHECK(std::find(node_kinds.begin(), node_kinds.end(), "model.request.sent") != node_kinds.end());
    CHECK(std::find(node_kinds.begin(), node_kinds.end(), "tool.execution.finished") !=
          node_kinds.end());
    const auto node_started =
        FirstEventOf(run_dir / "nodes" / node_files[0], "run.started");
    CHECK(node_started["payload"].value("run_kind", std::string()) == "workflow_node");
    CHECK(node_started["payload"].value("workflow_run_id", std::string()) == "wf-run-0001");
    CHECK(node_started["payload"].value("workflow_id", std::string()) == "probe-flow");
    CHECK(node_started["payload"].value("node_id", std::string()) == "gather");
    CHECK(node_started["payload"].value("node_kind", std::string()) == "agent");
    CHECK(node_started["payload"].value("attempt", 0) == 1);
    CHECK(node_started["relations"].value("parent_run_id", std::string()) == "wf-run-0001");

    // 整场 verify:main + 编排 + node 三层,父子边逐位对账。
    const auto report = ledger.VerifySession();
    CHECK(report.error_code.empty());
    REQUIRE(report.streams.size() == 3);
    // 编排账的父是本场 main run(main.jsonl 首行的 run_id)。
    const auto main_lines = trajectory::ReadJournalLines(ledger.session_dir() / "main.jsonl");
    REQUIRE(main_lines.has_value());
    const auto main_first = nlohmann::json::parse(main_lines->front(), nullptr, false);
    REQUIRE_FALSE(main_first.is_discarded());
    const std::string main_run_id = main_first.value("run_id", std::string());
    CHECK_FALSE(main_run_id.empty());
    int workflow_edges = 0;
    int node_edges = 0;
    for (const auto& edge : report.child_edges) {
        CHECK(edge.error_code.empty());
        if (edge.child_run_id == "wf-run-0001") {
            ++workflow_edges;
            CHECK(edge.parent_run_id == main_run_id);
            CHECK(edge.background_spawn);  // 后台派工:main 不落派发边
        } else {
            ++node_edges;
            CHECK(edge.parent_run_id == "wf-run-0001");
            CHECK(edge.child_terminal_hash == hash);
        }
    }
    CHECK(workflow_edges == 1);
    CHECK(node_edges == 1);
}

TEST_CASE("编排 2:无主 tool trace——node 桥不造册不落账") {
    auto opened = OpenLedger(FreshDir("lubancode-traj-wf-unowned"));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger ledger = std::move(*opened);
    auto run = ledger.SpawnWorkflowRun("wf-run-0002", MakeDefinitionInfo());
    REQUIRE(run.has_value());
    REQUIRE((*run)->RecordNodeDispatched("t", "wf-run-0002-t-d1-a1", 1, "tool", -1, ""));
    auto node = (*run)->SpawnNodeStream("t", "wf-run-0002-t-d1-a1", 1, "tool", -1);
    REQUIRE(node.has_value());
    auto& turn = (*node)->turn_bridge();
    // 开轮但不声明——宿主合成的调用直接灌 trace:一枚都不落(ownership
    // 门:声明才进,无主拒;关轮状态在 turn_open_ 门就折返,测不到这里)。
    turn.BeginTurn("turn-1", "scheduled_host");
    turn.OnToolTrace(ToolEvent(agent::ToolTraceEventKind::Scheduled, "host-call-1"));
    turn.OnToolTrace(ToolEvent(agent::ToolTraceEventKind::ExecutionStarted, "host-call-1"));
    turn.OnToolTrace(ToolEvent(agent::ToolTraceEventKind::ExecutionFinished, "host-call-1"));
    CHECK_FALSE(turn.unowned_trace_notes().empty());
    turn.EndTurn(true, false, std::string());
    (void)(*node)->Finish(true, false, std::string());
    (void)(*run)->Finish(true, false, std::string());

    const auto node_kinds = KindsOf(ledger.session_dir() / "workflows" / "wf-run-0002" / "nodes" /
                                    "wf-run-0002-t-d1-a1.jsonl");
    for (const std::string& kind : node_kinds) {
        CHECK(kind.rfind("tool.", 0) != 0);
    }
    CHECK(ledger.VerifySession().error_code.empty());
}

TEST_CASE("编排 3:node 账开张失败——fail closed,无 0 字节残留,无孤儿边") {
    auto opened = OpenLedger(FreshDir("lubancode-traj-wf-nodefail"),
                             /*run_fault=*/{}, [] { return std::string("io.create_failed"); });
    REQUIRE(opened.has_value());
    TrajectorySessionLedger ledger = std::move(*opened);
    auto run = ledger.SpawnWorkflowRun("wf-run-0003", MakeDefinitionInfo());
    REQUIRE(run.has_value());
    // runtime 的次序:先开卷后派发——开卷失败时连派发事实都不落,免造
    // "父账声明派发却无子文件"的孤儿边。
    auto node = (*run)->SpawnNodeStream("t", "wf-run-0003-t-d1-a1", 1, "tool", -1);
    REQUIRE_FALSE(node.has_value());
    CHECK(node.error().stage == "run_started");
    // io.* 注入码按 IoFailed 收:可重试(schema 类才不可重试)。
    CHECK(node.error().retryable);
    // P0-C 同款:失败不留 0 字节正式 stream。
    CHECK(NodeJsonlFiles(ledger, "wf-run-0003").empty());
    // 调用方(runtime)按合同补不带 child 引用的 node.failed 事实后收口。
    REQUIRE((*run)->RecordNodeFailed("t", "wf-run-0003-t-d1-a1", 1,
                                     node.error().error_code, node.error().detail, 0, 0,
                                     std::string()));
    (void)(*run)->Finish(false, false, "trajectory_node_start_failed");
    CHECK(ledger.VerifySession().error_code.empty());
}

TEST_CASE("编排 4:编排账开张失败——fail closed,无 stream,verify 过") {
    auto opened = OpenLedger(FreshDir("lubancode-traj-wf-runfail"),
                             [] { return std::string("io.create_failed"); });
    REQUIRE(opened.has_value());
    TrajectorySessionLedger ledger = std::move(*opened);
    auto run = ledger.SpawnWorkflowRun("wf-run-0004", MakeDefinitionInfo());
    REQUIRE_FALSE(run.has_value());
    CHECK(run.error().stage == "run_started");
    CHECK(run.error().retryable);
    const auto workflows_dir = ledger.session_dir() / "workflows";
    std::error_code ec;
    int jsonl_count = 0;
    if (std::filesystem::exists(workflows_dir, ec)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(workflows_dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") {
                ++jsonl_count;
            }
        }
    }
    CHECK(jsonl_count == 0);
    CHECK(ledger.VerifySession().error_code.empty());
}

TEST_CASE("编排 5:retry 新开 node 文件——attempt 1 与 attempt 2 不共写") {
    auto opened = OpenLedger(FreshDir("lubancode-traj-wf-retry"));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger ledger = std::move(*opened);
    auto run = ledger.SpawnWorkflowRun("wf-run-0005", MakeDefinitionInfo());
    REQUIRE(run.has_value());

    // attempt 1:失败收口 + retrying 事实。
    REQUIRE((*run)->RecordNodeDispatched("flaky", "wf-run-0005-flaky-d1-a1", 1, "tool", -1, ""));
    auto a1 = (*run)->SpawnNodeStream("flaky", "wf-run-0005-flaky-d1-a1", 1, "tool", -1);
    REQUIRE(a1.has_value());
    DriveNodeAttempt(**a1, "toolu-a1", /*ok=*/false);
    (*run)->RecordNodeRetrying("flaky", "wf-run-0005-flaky-d1-a1", 1, 3, "rate_limited");
    // attempt 2:成功。
    REQUIRE((*run)->RecordNodeDispatched("flaky", "wf-run-0005-flaky-d1-a2", 2, "tool", -1, ""));
    auto a2 = (*run)->SpawnNodeStream("flaky", "wf-run-0005-flaky-d1-a2", 2, "tool", -1);
    REQUIRE(a2.has_value());
    const std::string hash2 = DriveNodeAttempt(**a2, "toolu-a2");
    REQUIRE((*run)->RecordNodeCompleted("flaky", "wf-run-0005-flaky-d1-a2", 2, "success", 90, 5,
                                        std::string(), std::string(64, 'c'), hash2));
    (void)(*run)->Finish(true, false, std::string());

    const auto files = NodeJsonlFiles(ledger, "wf-run-0005");
    REQUIRE(files.size() == 2);
    CHECK(files[0] == "wf-run-0005-flaky-d1-a1.jsonl");
    CHECK(files[1] == "wf-run-0005-flaky-d1-a2.jsonl");
    const auto kinds1 = KindsOf(ledger.session_dir() / "workflows" / "wf-run-0005" / "nodes" / files[0]);
    const auto kinds2 = KindsOf(ledger.session_dir() / "workflows" / "wf-run-0005" / "nodes" / files[1]);
    CHECK(kinds1.back() == "run.failed");
    CHECK(kinds2.back() == "run.completed");
    const auto orchestration = KindsOf(ledger.session_dir() / "workflows" / "wf-run-0005" /
                                       "workflow.jsonl");
    CHECK(std::find(orchestration.begin(), orchestration.end(), "workflow.node.retrying") !=
          orchestration.end());
    CHECK(ledger.VerifySession().error_code.empty());
}

TEST_CASE("编排 6:旧 workflow-runs/ 路兼容——照写照读,不迁移不炸") {
    const auto root = FreshDir("lubancode-traj-wf-legacy");
    workflow::RunJournal::StartInfo info;
    info.run_id = "run-legacy-1";
    info.workflow_id = "probe-flow";
    info.workflow_version = "1.0.0";
    info.content_hash = std::string(64, '9');
    info.cwd = "D:/repo";
    info.definition_json = R"({"id":"probe-flow"})";
    auto journal = workflow::RunJournal::Start(root / "workflow-runs", info);
    REQUIRE(journal.has_value());
    journal->Append(workflow::kEventRunStarted, "", 0, nlohmann::json{{"state", "running"}});
    journal->Append(workflow::kEventNodeCompleted, "x", 1,
                    nlohmann::json{{"outcome", "success"}});
    journal->Finish("succeeded", nlohmann::json{{"tokens", 3}});

    // 旧读口全绿:ListRuns 排得出、事件读得回(Start/Finish 自带 run 边界
    // 事件,按内容断言,不数数)。
    const auto runs = workflow::ListRuns(root / "workflow-runs");
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].run_id == "run-legacy-1");
    CHECK(runs[0].final_state == "succeeded");
    const auto events = workflow::ReadJournalEvents(runs[0].dir);
    REQUIRE(events.size() >= 3);
    CHECK(events.front().type == workflow::kEventRunStarted);
    CHECK(events.back().type == workflow::kEventRunCompleted);
    bool saw_node = false;
    for (const auto& event : events) {
        if (event.type == workflow::kEventNodeCompleted && event.node_id == "x") {
            saw_node = true;
        }
    }
    CHECK(saw_node);
}

TEST_CASE("编排 7:hash 对账负例——编排记了假 hash,verify 报边裂") {
    auto opened = OpenLedger(FreshDir("lubancode-traj-wf-hashmiss"));
    REQUIRE(opened.has_value());
    TrajectorySessionLedger ledger = std::move(*opened);
    auto run = ledger.SpawnWorkflowRun("wf-run-0007", MakeDefinitionInfo());
    REQUIRE(run.has_value());
    REQUIRE((*run)->RecordNodeDispatched("t", "wf-run-0007-t-d1-a1", 1, "tool", -1, ""));
    auto node = (*run)->SpawnNodeStream("t", "wf-run-0007-t-d1-a1", 1, "tool", -1);
    REQUIRE(node.has_value());
    const std::string hash = DriveNodeAttempt(**node, "toolu-x");
    REQUIRE_FALSE(hash.empty());
    // 假 hash:与 node 文件实读的终态 hash 对不上。
    REQUIRE((*run)->RecordNodeCompleted("t", "wf-run-0007-t-d1-a1", 1, "success", 10, 1,
                                        std::string(), std::string(64, 'd'),
                                        std::string(64, 'd')));
    (void)(*run)->Finish(true, false, std::string());

    const auto report = ledger.VerifySession();
    CHECK_FALSE(report.error_code.empty());
    bool saw_mismatch = false;
    for (const auto& edge : report.child_edges) {
        if (edge.child_run_id == "wf-run-0007-t-d1-a1" &&
            edge.error_code == "edge.child_hash_mismatch") {
            saw_mismatch = true;
        }
    }
    CHECK(saw_mismatch);
}
