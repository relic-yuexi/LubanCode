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
#include "trajectory/v3/reader.hpp"  // VerifyV3File(V3-LEGACY-01 后 v3 账面)
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
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
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


// V3-LEGACY-01 后父场唯一 v3:子账是 subagents/<childSessionId>/
// <childSessionId>.jsonl。回子账完整路径列表。
std::vector<std::filesystem::path> SubagentAccountPaths(const TrajectorySessionLedger& ledger) {
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    const auto dir = ledger.session_dir() / "subagents";
    if (!std::filesystem::exists(dir, ec)) {
        return paths;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") {
            paths.push_back(entry.path());
        }
    }
    return paths;
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

    // 子账(v3 五步开卷):独立 <childSessionId>.jsonl,首行 system,委派
    // user 与模型往返在内,整卷验得过。
    const auto sub_paths = SubagentAccountPaths(*w.ledger);
    REQUIRE(sub_paths.size() == 1);
    const auto sub_path = sub_paths[0];
    CHECK(lubancode::trajectory::v3::VerifyV3File(sub_path).ok);
    bool sub_saw_delegation = false;
    bool sub_saw_assistant = false;
    {
        std::ifstream in(sub_path, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const auto row = nlohmann::json::parse(line, nullptr, false);
            if (row.is_discarded() || row.value("type", std::string()) != "message") {
                continue;
            }
            const std::string role = row.at("message").value("role", std::string());
            sub_saw_delegation = sub_saw_delegation ||
                                 (role == "user" && row.value("origin", std::string()) == "parent_agent");
            sub_saw_assistant = sub_saw_assistant || role == "assistant";
        }
    }
    CHECK(sub_saw_delegation);
    CHECK(sub_saw_assistant);

    // 父账(v3):自己那一枚 user 输入与一份数模型输出;子的轮内正文不
    // 混进父账;派工/链接两枚事实在(subagent.spawn.requested/linked)。
    const auto main_path = w.ledger->session_dir() /
                           std::filesystem::path(w.ledger->session_id() + ".jsonl");
    CHECK(lubancode::trajectory::v3::VerifyV3File(main_path).ok);
    int parent_user_inputs = 0;
    int parent_assistant = 0;
    bool saw_spawn = false;
    bool saw_linked = false;
    {
        std::ifstream in(main_path, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const auto row = nlohmann::json::parse(line, nullptr, false);
            if (row.is_discarded()) {
                continue;
            }
            if (row.value("type", std::string()) == "message") {
                const std::string role = row.at("message").value("role", std::string());
                parent_user_inputs += (role == "user" && row.value("origin", std::string()) !=
                                                            "parent_agent") ? 1 : 0;
                parent_assistant += role == "assistant" ? 1 : 0;
                continue;
            }
            const std::string kind = row.value("kind", std::string());
            saw_spawn = saw_spawn || kind == "subagent.spawn.requested";
            saw_linked = saw_linked || kind == "subagent.linked";
        }
    }
    CHECK(parent_user_inputs == 1);
    CHECK(parent_assistant == 1);
    CHECK(saw_spawn);
    CHECK(saw_linked);
    // 父桥没吃进任何无主 trace。
    CHECK(w.main_bridge->unowned_trace_notes().empty());
}

// (退役,V3-LEGACY-01)原此处有"整合 2:子账启动失败——fail closed,父账
// verify 过,无空子账"案:靠 v2 派工路的 subagent_start_fault 故障钩子注入
// schema 拒绝。写口退役后 v2 父场造不出,SpawnSubagentV3 无此注入口——
// fail closed 语义由 v3 五步的生产代码(落稳才返检查点)与 utf8_gate 册的
// 正常路守;旧盘 v2 活场的派工故障回归随恢复收养夹具另立。

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

    // 子账(v3):开过卷就有内容(不是 0 字节),整卷验得过,任务有终态
    //(completed/cancelled/failed 任一——ESC 掐流时收口在取消侧,只钉
    //"有终态")。
    const auto sub_paths = SubagentAccountPaths(*w.ledger);
    REQUIRE(sub_paths.size() == 1);
    CHECK(lubancode::trajectory::v3::VerifyV3File(sub_paths[0]).ok);
    // (口径修正)ESC 掐流时子桥的 Finish 不被调用——子账停在"跑到一半"
    // 的崩溃形状,不伪造 task 终态;非空 + 验卷过即"开过卷有内容"。

    // 父账(v3):整卷验得过;没有 dangling 补账失败(schema.missing_field
    // 一族不许再出现),也没有无主 trace 诊断。
    const auto main_path = w.ledger->session_dir() /
                           std::filesystem::path(w.ledger->session_id() + ".jsonl");
    CHECK(lubancode::trajectory::v3::VerifyV3File(main_path).ok);
    for (const std::string& note : w.main_bridge->recent_errors()) {
        CHECK(note.find("schema.missing_field") == std::string::npos);
        CHECK(note.find("dangling") == std::string::npos);
    }
    CHECK(w.main_bridge->unowned_trace_notes().empty());
}
