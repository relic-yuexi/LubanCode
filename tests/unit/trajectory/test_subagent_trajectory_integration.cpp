// 子代理空轨迹单 5.2 整合测试:真 TrajectorySessionLedger + ToolTraceHub +
// AgentTool 接线,三场各验事件归属(不只数红字):
//   1. 子账正常:内层事实只在子 JSONL;父账只有边界引用与终态对账。
//   2. 子账启动失败:agent 工具 fail closed;父 main verify 通过;无空子账。
//   3. 子代理运行中 ESC:父、子各自收口;无 missing_field;无空 stream。
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/tool_trace.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "runtime/trajectory_session.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "trajectory/journal.hpp"

using namespace lubancode;
using namespace lubancode::runtime;
using trajectory::RecordReceipt;

namespace {

std::filesystem::path FreshDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// 按脚本吐事件的假后端(与 unit/agent/test_agent_tool.cpp 同一套写法)。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

// 一发就断(Cancelled):模拟 ESC 掐流。
class CancelledBackend : public api::Backend {
public:
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)on_event;
        (void)cancel;
        captured_requests.push_back(request);
        return std::unexpected(api::Error{api::ErrorKind::Cancelled, "用户按 ESC 打断", 0});
    }
};

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {api::MessageStart{"msg", "model"}, api::TextDelta{text}, api::ContentBlockDone{0},
            api::MessageDone{"end_turn", api::Usage{}}};
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

std::vector<std::string> SubagentJsonlFiles(const TrajectorySessionLedger& ledger) {
    std::vector<std::string> names;
    std::error_code ec;
    const auto dir = ledger.session_dir() / "subagents";
    if (!std::filesystem::exists(dir, ec)) {
        return names;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") {
            names.push_back(entry.path().filename().generic_string());
        }
    }
    return names;
}

std::optional<TrajectorySessionLedger> OpenLedger(
    const std::filesystem::path& root, std::function<std::optional<std::string>()> fault = {}) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.lubancode_version = "test";
    options.subagent_start_fault = std::move(fault);
    std::error_code ec;
    std::filesystem::create_directories(root / "repo", ec);
    auto ledger = TrajectorySessionLedger::Open(std::move(options));
    if (!ledger.has_value()) {
        return std::nullopt;
    }
    return std::move(*ledger);
}

agent::ToolTraceEvent AgentCallEvent(agent::ToolTraceEventKind kind, const std::string& call_id) {
    agent::ToolTraceEvent event;
    event.kind = kind;
    event.execution_id = "item-agent-1";
    event.tool_use_id = call_id;
    event.tool_name = "agent";
    event.batch_id = "batch-1";
    event.sequence_in_batch = 0;
    event.timestamp_ms = 1759000000000LL;
    if (kind == agent::ToolTraceEventKind::ExecutionStarted) {
        event.effective_input_sha256 = std::string(64, '0');
        event.effect_class = agent::EffectClass::InProcessUnknown;
        event.effective_arguments = nlohmann::json{{"prompt", "把仓库数一遍"}};
    } else if (kind == agent::ToolTraceEventKind::ExecutionFinished) {
        event.outcome = agent::ToolOutcome::Succeeded;
        event.duration_ms = 50;
        event.result_ref.kind = agent::ToolResultRef::Kind::Inline;
        event.result_ref.sha256 = std::string(64, '2');
        event.result_ref.bytes = 20;
    }
    return event;
}

// 装配一体:ledger + main bridge + hub + agent 工具,外加父轮的手动边界
//(模型声明 agent 调用 -> scheduled -> started -> [execute] -> finished ->
// result committed -> end turn),与 turn_runner 的接线同构。
struct Wiring {
    IdAuthority ids;
    std::unique_ptr<TrajectorySessionLedger> ledger;
    std::unique_ptr<TrajectoryTurnBridge> main_bridge;
    ToolTraceHub hub{ids};
    std::string parent_request_id;

    explicit Wiring(const char* tag, api::Backend& backend, tools::ToolRegistry& sub_registry,
                    std::function<std::optional<std::string>()> fault = {},
                    std::atomic<bool>* cancel = nullptr) {
        auto opened = OpenLedger(FreshDir(tag), std::move(fault));
        REQUIRE(opened.has_value());
        ledger = std::make_unique<TrajectorySessionLedger>(std::move(*opened));
        main_bridge = ledger->NewTurnBridge({"demo", "responses", "terminal"});
        REQUIRE(main_bridge != nullptr);
        hub.AttachTrajectory(main_bridge.get());

        tools::AgentTool::Hooks hooks;
        hooks.on_tool_trace = [this](const agent::ToolTraceEvent& event) { hub.OnTrace(event); };
        hooks.trajectory_spawn = [this](const std::string& task_label, const std::string& parent_run_id,
                                        SubagentSpawnFailure* failure_out) {
            const std::string parent_call_id = hub.current_agent_call_id();
            auto child = ledger->SpawnSubagent(parent_call_id, task_label, parent_run_id);
            if (!child.has_value()) {
                ledger->NoteSubagentStartFailed(child.error(), parent_run_id, parent_call_id,
                                                main_bridge->current_turn_id());
                if (failure_out != nullptr) {
                    *failure_out = child.error();
                }
                return std::unique_ptr<TrajectorySubagentBridge>();
            }
            if (parent_run_id.empty() && !parent_call_id.empty()) {
                main_bridge->AttachChildRun(parent_call_id, (*child)->run_id());
            }
            return std::move(*child);
        };
        hooks.trajectory_child_finished = [this](const std::string& run_id, const std::string& hash) {
            main_bridge->NoteChildTerminal(run_id, hash);
        };
        hooks.cancel = cancel;
        tool = std::make_unique<tools::AgentTool>(backend, sub_registry, "/work/dir");
        tool->SetHooks(std::move(hooks));

        // 父轮开张:模型声明一枚 agent 调用,走到 started。
        main_bridge->BeginTurn("turn-1", "external_user");
        api::Message input;
        input.role = api::Role::User;
        input.content.push_back(api::TextBlock{"派一只子代理去数仓库"});
        main_bridge->RecordInput(input);
        parent_request_id = main_bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
        REQUIRE_FALSE(parent_request_id.empty());
        main_bridge->OnRequestSent(parent_request_id);
        api::Message assistant;
        assistant.role = api::Role::Assistant;
        assistant.content.push_back(api::TextBlock{"这就去。"});
        api::ToolUseBlock call;
        call.id = "toolu-parent";
        call.name = "agent";
        call.input = nlohmann::json{{"prompt", "把仓库数一遍"}};
        assistant.content.push_back(std::move(call));
        REQUIRE(main_bridge->OnOutputCompleted(parent_request_id, assistant, "tool_use", "resp-p"));
        hub.OnTrace(AgentCallEvent(agent::ToolTraceEventKind::Scheduled, "toolu-parent"));
        hub.OnTrace(AgentCallEvent(agent::ToolTraceEventKind::ExecutionStarted, "toolu-parent"));
    }

    tools::Tool::Result Execute(const std::string& prompt) {
        return tool->execute(nlohmann::json{{"title", "数仓库"}, {"prompt", prompt}});
    }

    void FinishParentTurn(bool ok, bool cancelled, const agent::ToolTraceEvent& finished,
                          bool result_is_error, const std::string& result_text) {
        hub.OnTrace(finished);
        api::Message results;
        results.role = api::Role::User;
        results.content.push_back(api::ToolResultBlock{"toolu-parent", result_text, result_is_error});
        main_bridge->OnToolResultsCommitted("batch-1", results);
        main_bridge->EndTurn(ok, cancelled, cancelled ? "user_cancel" : (ok ? "done" : "failed"));
    }

    std::unique_ptr<tools::AgentTool> tool;

    Wiring(const Wiring&) = delete;
    Wiring& operator=(const Wiring&) = delete;
};

}  // namespace

TEST_CASE("整合 1:子账正常——内层事实在子 JSONL,父账只有边界与终态引用") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("子代理结论:一共 41 个文件")};
    tools::ToolRegistry sub_registry;
    Wiring w("lubancode-traj-int-normal", backend, sub_registry);

    const tools::Tool::Result result = w.Execute("把仓库数一遍");
    CHECK_FALSE(result.is_error);
    REQUIRE(backend.captured_requests.size() == 1);

    agent::ToolTraceEvent finished =
        AgentCallEvent(agent::ToolTraceEventKind::ExecutionFinished, "toolu-parent");
    w.FinishParentTurn(/*ok=*/true, /*cancelled=*/false, finished, /*result_is_error=*/false,
                       result.content);

    // 子账:独立 jsonl,链干净,有自己的模型边界。
    const auto sub_files = SubagentJsonlFiles(*w.ledger);
    REQUIRE(sub_files.size() == 1);
    const auto sub_path = w.ledger->session_dir() / "subagents" / sub_files[0];
    CHECK(trajectory::VerifyJournalFile(sub_path).ok);
    const auto sub_kinds = KindsOf(sub_path);
    CHECK(std::find(sub_kinds.begin(), sub_kinds.end(), "input.received") != sub_kinds.end());
    CHECK(std::find(sub_kinds.begin(), sub_kinds.end(), "model.output.completed") != sub_kinds.end());
    CHECK(sub_kinds.front() == "run.started");
    CHECK(sub_kinds.back() == "run.completed");

    // 父账:边界引用齐全。子代理的最终答复经 tool.result.committed 进父账
    //(父模型要读,这是父自己的事实);但子的轮内事件不得混进父账——
    // 父账只有自己那一枚 input 与一份数模型输出。
    const auto main_path = w.ledger->session_dir() / "main.jsonl";
    const auto main_kinds_all = KindsOf(main_path);
    CHECK(std::count(main_kinds_all.begin(), main_kinds_all.end(), "input.received") == 1);
    CHECK(std::count(main_kinds_all.begin(), main_kinds_all.end(), "model.request.sent") == 1);
    CHECK(std::count(main_kinds_all.begin(), main_kinds_all.end(), "model.output.completed") == 1);
    const auto lines = trajectory::ReadJournalLines(main_path);
    REQUIRE(lines.has_value());
    std::string child_run_id;
    std::string child_terminal_hash;
    bool saw_child_edge = false;
    bool saw_terminal_edge = false;
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded()) {
            continue;
        }
        const std::string kind = parsed.value("kind", std::string());
        const auto relations = parsed.value("relations", nlohmann::json::object());
        if (kind == "tool.execution.finished" && parsed.value("call_id", std::string()) == "toolu-parent") {
            child_run_id = relations.value("child_run_id", std::string());
            saw_child_edge = !child_run_id.empty();
            child_terminal_hash =
                parsed["payload"]["result_ref"].value("child_terminal_event_hash", std::string());
            saw_terminal_edge = !child_terminal_hash.empty();
        }
    }
    CHECK(saw_child_edge);
    CHECK(saw_terminal_edge);
    CHECK(child_run_id == sub_files[0].substr(0, sub_files[0].size() - 6));  // 去掉 .jsonl
    // 父桥没吃进任何无主 trace。
    CHECK(w.main_bridge->unowned_trace_notes().empty());
    // 整场 verify(父+子交叉核)通过。
    const auto report = w.ledger->VerifySession();
    CHECK(report.error_code.empty());
}

TEST_CASE("整合 2:子账启动失败——fail closed,父账 verify 过,无空子账") {
    FakeBackend backend;  // 子代理若被错误放行,会耗尽脚本报错——这里必须 0 请求
    tools::ToolRegistry sub_registry;
    Wiring w("lubancode-traj-int-spawnfail", backend, sub_registry,
             [] { return std::string("schema.payload_missing_field"); });

    const tools::Tool::Result result = w.Execute("把仓库数一遍");
    // P0-A:fail closed——子代理未执行(一个模型请求都没发)。
    CHECK(result.is_error);
    CHECK(result.content.find("trajectory.subagent_start_failed") != std::string::npos);
    CHECK(result.content.find("run_started") != std::string::npos);  // 阶段写进结果正文
    REQUIRE(backend.captured_requests.empty());

    // 父调用照常收口:failed + result committed(主调用可以继续别的)。
    agent::ToolTraceEvent finished =
        AgentCallEvent(agent::ToolTraceEventKind::ExecutionFinished, "toolu-parent");
    finished.outcome = agent::ToolOutcome::ToolError;  // 已 started 的失败终态
    finished.error_code = "trajectory.subagent_start_failed";
    w.FinishParentTurn(/*ok=*/true, /*cancelled=*/false, finished, /*result_is_error=*/true,
                       result.content);

    // P0-B:失败事实进父账 typed 事件。
    const auto main_path = w.ledger->session_dir() / "main.jsonl";
    const auto lines = trajectory::ReadJournalLines(main_path);
    REQUIRE(lines.has_value());
    bool saw_start_failed = false;
    bool saw_tool_failed = false;
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded()) {
            continue;
        }
        const std::string kind = parsed.value("kind", std::string());
        if (kind == "subagent.run.start_failed") {
            saw_start_failed = true;
            CHECK(parsed["payload"]["stage"] == "run_started");
            CHECK(parsed["payload"]["error_code"].get<std::string>()
                      .find("schema.payload_missing_field") != std::string::npos);
            CHECK(parsed.value("call_id", std::string()) == "toolu-parent");
        }
        if (kind == "tool.execution.failed" && parsed.value("call_id", std::string()) == "toolu-parent") {
            saw_tool_failed = true;
        }
    }
    CHECK(saw_start_failed);
    CHECK(saw_tool_failed);

    // P0-C:session 目录里没有 0 字节(或任何)子 .jsonl;verify 通过。
    CHECK(SubagentJsonlFiles(*w.ledger).empty());
    const auto report = w.ledger->VerifySession();
    CHECK(report.error_code.empty());
}

TEST_CASE("整合 3:子代理运行中 ESC——父子各自收口,无 missing_field,无空 stream") {
    CancelledBackend backend;
    std::atomic<bool> cancel_flag{false};
    tools::ToolRegistry sub_registry;
    Wiring w("lubancode-traj-int-esc", backend, sub_registry, {}, &cancel_flag);

    const tools::Tool::Result result = w.Execute("把仓库数一遍");
    REQUIRE(backend.captured_requests.size() == 1);  // 发了一笔就被 ESC 掐断

    agent::ToolTraceEvent finished =
        AgentCallEvent(agent::ToolTraceEventKind::ExecutionFinished, "toolu-parent");
    finished.outcome = agent::ToolOutcome::CancelledDuringRun;
    w.FinishParentTurn(/*ok=*/false, /*cancelled=*/true, finished, /*result_is_error=*/true,
                       "用户按 ESC 打断,该工具未执行");

    // 子账:开过卷就有内容(不是 0 字节),run 有终态,turn 有终态。
    // 注:ESC 掐流时 DriveReport.ok 仍为真(取消不落 error 文案),run 终态
    // 可能是 completed(收口在 turn.cancelled 上)——这里只钉"有终态"。
    const auto sub_files = SubagentJsonlFiles(*w.ledger);
    REQUIRE(sub_files.size() == 1);
    const auto sub_path = w.ledger->session_dir() / "subagents" / sub_files[0];
    CHECK(trajectory::VerifyJournalFile(sub_path).ok);
    const auto sub_kinds = KindsOf(sub_path);
    CHECK(sub_kinds.front() == "run.started");
    const bool has_run_terminal = sub_kinds.back() == "run.completed" || sub_kinds.back() == "run.failed";
    CHECK(has_run_terminal);
    CHECK(std::find(sub_kinds.begin(), sub_kinds.end(), "turn.started") != sub_kinds.end());
    CHECK(std::find(sub_kinds.begin(), sub_kinds.end(), "turn.cancelled") != sub_kinds.end());

    // 父账:turn.cancelled 收口;没有 dangling 补账失败(schema.missing_field
    // 一族不许再出现),也没有无主 trace 诊断。
    const auto main_path = w.ledger->session_dir() / "main.jsonl";
    const auto main_kinds = KindsOf(main_path);
    CHECK(std::find(main_kinds.begin(), main_kinds.end(), "turn.cancelled") != main_kinds.end());
    CHECK(trajectory::VerifyJournalFile(main_path).ok);
    for (const std::string& note : w.main_bridge->recent_errors()) {
        CHECK(note.find("schema.missing_field") == std::string::npos);
        CHECK(note.find("dangling") == std::string::npos);
    }
    CHECK(w.main_bridge->unowned_trace_notes().empty());
    const auto report = w.ledger->VerifySession();
    CHECK(report.error_code.empty());
}
