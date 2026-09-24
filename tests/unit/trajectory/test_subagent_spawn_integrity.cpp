// 子代理空轨迹与父账污染修复单(5.1):SpawnSubagent 故障注入、OnToolTrace
// ownership 门、dangling 收口只认已声明调用、schema 字段级 message 过境。
// 全部走真 recorder + 临时目录,断言落在事件归属与盘上文件,不只数红字。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/tool_trace.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"

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

std::optional<TrajectorySessionLedger> OpenLedger(const std::filesystem::path& root,
                                                  std::function<std::optional<std::string>()> fault = {}) {
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

// subagents/ 下 *.jsonl 正式 stream 的文件名清单。

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantWithToolCall(const std::string& call_id, const std::string& name = "read_file") {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{"我先看看。"});
    api::ToolUseBlock call;
    call.id = call_id;
    call.name = name;
    call.input = nlohmann::json{{"path", "README.md"}};
    message.content.push_back(std::move(call));
    return message;
}

agent::ToolTraceEvent TraceEvent(agent::ToolTraceEventKind kind, const std::string& call_id,
                                 const std::string& tool_name = "read_file") {
    agent::ToolTraceEvent event;
    event.kind = kind;
    event.execution_id = "item-1";
    event.tool_use_id = call_id;
    event.tool_name = tool_name;
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

// ---------------------------------------------------------------------------
// 5.1 SpawnSubagent fault injection
// ---------------------------------------------------------------------------

// (退役,V3-LEGACY-01)原此处有"SpawnSubagent:run.started 被 schema 类拒绝"案:env 注入 0 开 v2 父场,经
// ledger 级 SpawnSubagent 的 v2 故障钩子/占名术验失败分档与凭据清理。写口
// 退役后 v2 父场造不出(v2 故障钩子只挂 v2 派工路, SpawnSubagentV3 无此
// 注入口;占名术亦随 subagents/<run_id>.jsonl 平铺布局退役)——该前提在
// 公开口径下无法再造。v3 派工的五步失败语义(fail closed/无残留)由
// SpawnSubagentV3 的生产代码与 v3 子账册守;旧盘 v2 活场的派工故障回归
// 随恢复收养夹具另立(旧档消费路径)。

// (退役,V3-LEGACY-01)原此处有"SpawnSubagent:run.started I/O 失败(目标名被占)"案:env 注入 0 开 v2 父场,经
// ledger 级 SpawnSubagent 的 v2 故障钩子/占名术验失败分档与凭据清理。写口
// 退役后 v2 父场造不出(v2 故障钩子只挂 v2 派工路, SpawnSubagentV3 无此
// 注入口;占名术亦随 subagents/<run_id>.jsonl 平铺布局退役)——该前提在
// 公开口径下无法再造。v3 派工的五步失败语义(fail closed/无残留)由
// SpawnSubagentV3 的生产代码与 v3 子账册守;旧盘 v2 活场的派工故障回归
// 随恢复收养夹具另立(旧档消费路径)。

// (退役,V3-LEGACY-01)原此处有"SpawnSubagent:注入 io.append_failed"案:env 注入 0 开 v2 父场,经
// ledger 级 SpawnSubagent 的 v2 故障钩子/占名术验失败分档与凭据清理。写口
// 退役后 v2 父场造不出(v2 故障钩子只挂 v2 派工路, SpawnSubagentV3 无此
// 注入口;占名术亦随 subagents/<run_id>.jsonl 平铺布局退役)——该前提在
// 公开口径下无法再造。v3 派工的五步失败语义(fail closed/无残留)由
// SpawnSubagentV3 的生产代码与 v3 子账册守;旧盘 v2 活场的派工故障回归
// 随恢复收养夹具另立(旧档消费路径)。

// (退役,V3-LEGACY-01)原此处有"SpawnSubagent:目标名被 0 字节文件占住"案:env 注入 0 开 v2 父场,经
// ledger 级 SpawnSubagent 的 v2 故障钩子/占名术验失败分档与凭据清理。写口
// 退役后 v2 父场造不出(v2 故障钩子只挂 v2 派工路, SpawnSubagentV3 无此
// 注入口;占名术亦随 subagents/<run_id>.jsonl 平铺布局退役)——该前提在
// 公开口径下无法再造。v3 派工的五步失败语义(fail closed/无残留)由
// SpawnSubagentV3 的生产代码与 v3 子账册守;旧盘 v2 活场的派工故障回归
// 随恢复收养夹具另立(旧档消费路径)。

TEST_CASE("SpawnSubagent:正常开卷(v3 五步)——子账目录立得住;0 字节残留按凭据清") {
    const auto root = FreshDir("lubancode-traj-spawn-normal");
    auto ledger = OpenLedger(root);
    REQUIRE(ledger.has_value());

    const auto child = ledger->SpawnSubagent("toolu-1", "读文件并数行数");
    REQUIRE(child.has_value());
    // V3-LEGACY-01 后父场唯一 v3:子账是 subagents/<childSessionId>/
    // <childSessionId>.jsonl(首行 system,五步开卷)。枚举定位。
    std::filesystem::path path;
    {
        std::error_code walk_ec;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(ledger->session_dir() / "subagents",
                                                           walk_ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
                path = entry.path();
            }
        }
    }
    REQUIRE_FALSE(path.empty());
    {
        std::ifstream head(path, std::ios::binary);
        std::string first_line;
        std::getline(head, first_line);
        if (!first_line.empty() && first_line.back() == '\r') first_line.pop_back();
        const auto first = nlohmann::json::parse(first_line, nullptr, false);
        REQUIRE_FALSE(first.is_discarded());
        CHECK(first.value("type", std::string()) == "message");
        CHECK(first.at("message").value("role", std::string()) == "system");
    }
    // 父账:subagent.spawn.requested 已落(派工事实,childRef 指子账路径)。
    bool saw_spawn_requested = false;
    {
        const std::filesystem::path main_stream = ledger->session_dir() /
            std::filesystem::path(ledger->session_id() + ".jsonl");
        std::ifstream in(main_stream, std::ios::binary);
        REQUIRE(in.is_open());
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const auto row = nlohmann::json::parse(line, nullptr, false);
            if (row.is_discarded() || row.value("type", std::string()) != "event" ||
                row.value("kind", std::string()) != "subagent.spawn.requested") {
                continue;
            }
            saw_spawn_requested = true;
        }
    }
    CHECK(saw_spawn_requested);

    // DiscardUncommittedStream 的所有权凭据:0 字节才清,有内容/目录不碰
    //(格式无关,原样保留)。
    const auto zero = ledger->session_dir() / "subagents" / "zero.jsonl";
    { std::ofstream file(zero, std::ios::binary); }
    CHECK(trajectory::DiscardUncommittedStream(zero));
    CHECK_FALSE(std::filesystem::exists(zero));
    CHECK_FALSE(trajectory::DiscardUncommittedStream(path));  // 有字节的账不动
    CHECK_FALSE(trajectory::DiscardUncommittedStream(ledger->session_dir() / "no-such.jsonl"));
}

// ---------------------------------------------------------------------------
// 5.1 OnToolTrace ownership 门 / dangling 收口
// ---------------------------------------------------------------------------

namespace {

// 开一只绑真 recorder 的桥,把一轮"声明 toolu-1"的账走到位。
struct BridgeHarness {
    std::filesystem::path dir;
    std::optional<trajectory::TrajectoryRecorder> recorder;
    std::optional<TrajectoryTurnBridge> bridge;
    std::string request_id;

    explicit BridgeHarness(const char* tag) : dir(FreshDir(tag)) {
        trajectory::EventScope scope;
        scope.workspace_key = "demo-000000000000";
        scope.session_id = "20260902-113716-L0O6LI";
        scope.run_id = "main-0001";
        scope.run_kind = trajectory::RunKind::MainSession;
        scope.visibility = {trajectory::Visibility::HostOnly};
        trajectory::RecorderOptions options;
        options.event_schema_version = 2;
        auto started = trajectory::TrajectoryRecorder::Start(dir / "main.jsonl", dir / "artifacts",
                                                             scope, std::move(options));
        REQUIRE(started.has_value());
        recorder = std::move(*started);
        REQUIRE(recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}},
                                          trajectory::Durability::PowerLoss)
                    .status == RecordReceipt::Status::Committed);
        bridge.emplace(*recorder, scope, TrajectoryTurnBridge::Identity{"demo", "responses", "terminal"});
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("去读文件"));
        request_id = bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
        REQUIRE_FALSE(request_id.empty());
        bridge->OnRequestSent(request_id);
    }
};

}  // namespace

TEST_CASE("ownership 门:陌生 call trace 不入册,收轮不报 dangling,账干净") {
    BridgeHarness h("lubancode-traj-ownership");
    REQUIRE(h.bridge->OnOutputCompleted(h.request_id, AssistantWithToolCall("toolu-1"), "tool_use",
                                        "resp-1"));

    // 陌生 call trace(子代理回灌形状):进了诊断投影,calls_ 不认。
    const agent::ToolTraceEvent stranger = TraceEvent(agent::ToolTraceEventKind::Scheduled, "ghost-9");
    h.bridge->OnToolTrace(stranger);
    h.bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, "ghost-9"));
    const auto notes = h.bridge->unowned_trace_notes();
    REQUIRE_FALSE(notes.empty());
    CHECK(notes.front().find("trajectory.unowned_tool_trace") == 0);
    CHECK(notes.front().find("call_id=ghost-9") != std::string::npos);
    CHECK(notes.front().find("tool_name=read_file") != std::string::npos);
    CHECK(notes.front().find("run_id=main-0001") != std::string::npos);
    CHECK(notes.front().find("turn_id=turn-1") != std::string::npos);

    // toolu-1 正常走完;收轮成功,不因 ghost-9 报 dangling。
    h.bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, "toolu-1"));
    h.bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, "toolu-1"));
    api::Message results;
    results.role = api::Role::User;
    results.content.push_back(api::ToolResultBlock{"toolu-1", "看完了。", false});
    h.bridge->OnToolResultsCommitted("batch-1", results);
    h.bridge->EndTurn(true, false, "done");

    for (const std::string& note : h.bridge->recent_errors()) {
        CHECK(note.find("dangling") == std::string::npos);
    }
    // 账上没有 ghost-9 的任何事件;整本验链过。
    const auto lines = trajectory::ReadJournalLines(h.dir / "main.jsonl");
    REQUIRE(lines.has_value());
    for (const std::string& line : *lines) {
        CHECK(line.find("ghost-9") == std::string::npos);
    }
    CHECK(trajectory::VerifyJournalFile(h.dir / "main.jsonl").ok);
}

TEST_CASE("dangling 收口:已声明调用 planned 后直接取消,仍合法补 cancelled") {
    BridgeHarness h("lubancode-traj-dangling-declared");
    REQUIRE(h.bridge->OnOutputCompleted(h.request_id, AssistantWithToolCall("toolu-1"), "tool_use",
                                        "resp-1"));
    h.bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, "toolu-1"));
    // 不 started 不 finished,轮直接收——dangling 补 tool.execution.cancelled。
    h.bridge->EndTurn(false, false, "failed");

    const auto lines = trajectory::ReadJournalLines(h.dir / "main.jsonl");
    REQUIRE(lines.has_value());
    bool saw_cancelled = false;
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || parsed.value("kind", std::string()) != "tool.execution.cancelled") {
            continue;
        }
        if (parsed.value("call_id", std::string()) == "toolu-1") {
            saw_cancelled = true;
            CHECK(parsed["payload"]["reason"] == "turn_closed_unresolved");
            CHECK(parsed.value("request_id", std::string()) == h.request_id);
        }
    }
    CHECK(saw_cancelled);
    CHECK(trajectory::VerifyJournalFile(h.dir / "main.jsonl").ok);
}

TEST_CASE("schema 拒绝带字段级 message:receipt 说得出缺哪个字段(P0-B)") {
    const auto dir = FreshDir("lubancode-traj-field-message");
    trajectory::EventScope scope;
    scope.workspace_key = "demo-000000000000";
    scope.session_id = "20260902-113716-L0O6LI";
    scope.run_id = "main-0001";
    scope.run_kind = trajectory::RunKind::MainSession;
    scope.visibility = {trajectory::Visibility::HostOnly};
    auto recorder = trajectory::TrajectoryRecorder::Start(dir / "main.jsonl", dir / "artifacts", scope);
    REQUIRE(recorder.has_value());
    REQUIRE(recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}},
                                      trajectory::Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);

    // payload 缺必填(trigger):稳定码 + 字段级人话都要有。
    trajectory::RecordRequest bad;
    bad.kind = trajectory::EventKind::TurnStarted;
    bad.scope = recorder->base_scope();
    bad.scope.turn_id = "turn-1";
    bad.payload = nlohmann::json::object();
    const auto receipt = recorder->Record(std::move(bad), trajectory::Durability::ProcessCrash);
    CHECK(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(receipt.error_code == "schema.payload_missing_field");
    CHECK(receipt.error_message.find("trigger") != std::string::npos);

    // id 三档:turn.started 禁 request_id,带上即拒,人话点名 request_id。
    trajectory::RecordRequest bad_id;
    bad_id.kind = trajectory::EventKind::TurnStarted;
    bad_id.scope = recorder->base_scope();
    bad_id.scope.turn_id = "turn-1";
    bad_id.scope.request_id = "req-1";
    bad_id.payload = nlohmann::json{{"trigger", "external_user"}};
    const auto receipt_id = recorder->Record(std::move(bad_id), trajectory::Durability::ProcessCrash);
    CHECK(receipt_id.status == RecordReceipt::Status::Rejected);
    CHECK(receipt_id.error_code == "schema.forbidden_field");
    CHECK(receipt_id.error_message.find("request_id") != std::string::npos);

    // 不二次落坏账:拒绝的事件没进链,整本依旧干净。
    CHECK(trajectory::VerifyJournalFile(dir / "main.jsonl").ok);
    const auto lines = trajectory::ReadJournalLines(dir / "main.jsonl");
    REQUIRE(lines.has_value());
    CHECK(lines->size() == 1);
}
