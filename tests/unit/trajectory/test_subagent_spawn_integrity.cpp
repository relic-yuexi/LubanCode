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

std::string MainRunIdOf(const TrajectorySessionLedger& ledger) {
    const auto lines = trajectory::ReadJournalLines(ledger.session_dir() / "main.jsonl");
    if (!lines.has_value() || lines->empty()) {
        return std::string();
    }
    const auto parsed = nlohmann::json::parse(lines->front(), nullptr, false);
    if (parsed.is_discarded()) {
        return std::string();
    }
    return parsed.value("run_id", std::string());
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

TEST_CASE("SpawnSubagent:run.started 被 schema 类拒绝——结构化失败,无 0 字节 .jsonl") {
    const auto root = FreshDir("lubancode-traj-spawn-reject");
    auto ledger = OpenLedger(root, [] { return std::string("schema.payload_missing_field"); });
    REQUIRE(ledger.has_value());

    const auto child = ledger->SpawnSubagent("toolu-1", "读文件并数行数");
    REQUIRE_FALSE(child.has_value());
    const SubagentSpawnFailure& failure = child.error();
    CHECK(failure.stage == "run_started");
    CHECK(failure.error_code.find("trajectory.subagent_run_started") == 0);
    CHECK(failure.error_code.find("schema.payload_missing_field") != std::string::npos);
    CHECK(failure.reserved_run_id.rfind("agent-1-", 0) == 0);
    CHECK_FALSE(failure.retryable);  // schema 类拒绝不可重试

    // 错误码没吞:recent_io_errors 里留着阶段与码。
    const auto errors = ledger->recent_io_errors();
    REQUIRE_FALSE(errors.empty());
    bool seen = false;
    for (const std::string& note : errors) {
        if (note.find("subagent.start_failed:run_started") != std::string::npos &&
            note.find("schema.payload_missing_field") != std::string::npos) {
            seen = true;
        }
    }
    CHECK(seen);

    // P0-C:正式 .jsonl 只在 run.started 提交事务里创建——一枚都没有。
    CHECK(SubagentJsonlFiles(*ledger).empty());
    // 失败事实可进父账(P0-B):typed 事件落得下。
    ledger->NoteSubagentStartFailed(failure, std::string(), "toolu-1", std::string());
    const auto main_kinds = KindsOf(ledger->session_dir() / "main.jsonl");
    REQUIRE(std::find(main_kinds.begin(), main_kinds.end(), "subagent.run.start_failed") !=
            main_kinds.end());
}

TEST_CASE("SpawnSubagent:run.started I/O 失败(目标名被占)——无残留,retryable,verify 过") {
    const auto root = FreshDir("lubancode-traj-spawn-iofail");
    auto ledger = OpenLedger(root);
    REQUIRE(ledger.has_value());
    const std::string main_run_id = MainRunIdOf(*ledger);
    REQUIRE_FALSE(main_run_id.empty());

    // 占名:同名的"目录"让独占创建必败(io.create_failed)。
    const std::string reserved = "agent-1-" + main_run_id;
    const auto jam = ledger->session_dir() / "subagents" / (reserved + ".jsonl");
    std::error_code ec;
    std::filesystem::create_directories(jam, ec);
    REQUIRE(std::filesystem::is_directory(jam));

    const auto child = ledger->SpawnSubagent("toolu-1", "读文件并数行数");
    REQUIRE_FALSE(child.has_value());
    CHECK(child.error().stage == "run_started");
    CHECK(child.error().error_code.find("io.create_failed") != std::string::npos);
    CHECK(child.error().retryable);  // I/O 类失败可重试
    CHECK(child.error().reserved_run_id == reserved);

    // 所有权凭据:目录不是"未开卷残留",清理不碰它;也没有别的 .jsonl 诞生。
    CHECK(std::filesystem::is_directory(jam));
    CHECK(SubagentJsonlFiles(*ledger).empty());

    // P0-B:失败事实进父账 typed 事件;io 细节里的绝对路径按 <session_dir>
    // 占位符 redact,不抄敏感路径。(turn 绑定由整合 2 的真实接线覆盖——
    // 这里 main 没开轮,turn_id 如实不带。)
    ledger->NoteSubagentStartFailed(child.error(), std::string(), "toolu-1", std::string());
    const auto lines = trajectory::ReadJournalLines(ledger->session_dir() / "main.jsonl");
    REQUIRE(lines.has_value());
    bool saw_event = false;
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || parsed.value("kind", std::string()) != "subagent.run.start_failed") {
            continue;
        }
        saw_event = true;
        const auto& payload = parsed["payload"];
        CHECK(payload["stage"] == "run_started");
        CHECK(payload["error_code"].get<std::string>().find("io.create_failed") != std::string::npos);
        CHECK(payload["parent_run_id"] == main_run_id);  // 空串解析回 main_run_id
        CHECK(payload["parent_call_id"] == "toolu-1");
        CHECK(payload["reserved_run_id"] == reserved);
        CHECK(payload["stream_ref"] == "subagents/" + reserved + ".jsonl");
        CHECK(payload["retryable"] == true);
        CHECK_FALSE(parsed.contains("turn_id"));
        const std::string detail = payload.value("detail", std::string());
        CHECK(detail.find("<session_dir>") != std::string::npos);
        CHECK(detail.find(ledger->session_dir().generic_string()) == std::string::npos);
    }
    CHECK(saw_event);

    // 占名的是目录,不是正式 stream——session verify 照样过。
    const auto report = ledger->VerifySession();
    CHECK(report.error_code.empty());
}

TEST_CASE("SpawnSubagent:注入 io.append_failed(flush 类)——IoFailed 收口,retryable") {
    const auto root = FreshDir("lubancode-traj-spawn-flushfail");
    auto ledger = OpenLedger(root, [] { return std::string("io.append_failed"); });
    REQUIRE(ledger.has_value());

    const auto child = ledger->SpawnSubagent("toolu-1", "读文件并数行数");
    REQUIRE_FALSE(child.has_value());
    CHECK(child.error().stage == "run_started");
    CHECK(child.error().error_code.find("io.append_failed") != std::string::npos);
    CHECK(child.error().retryable);  // append/flush 类失败可重试(schema 类不可)
    // 无正式子账诞生,verify 照过。
    CHECK(SubagentJsonlFiles(*ledger).empty());
    const auto report = ledger->VerifySession();
    CHECK(report.error_code.empty());
}

TEST_CASE("SpawnSubagent:目标名被 0 字节文件占住——残留按凭据清走,不冒充 Journal") {
    const auto root = FreshDir("lubancode-traj-spawn-zerobyte");
    auto ledger = OpenLedger(root);
    REQUIRE(ledger.has_value());
    const std::string main_run_id = MainRunIdOf(*ledger);
    REQUIRE_FALSE(main_run_id.empty());
    // 真实 I/O 失败形状:预留名上已有一枚 0 字节文件,独占创建必败。
    const std::string reserved = "agent-1-" + main_run_id;
    const auto jam = ledger->session_dir() / "subagents" / (reserved + ".jsonl");
    std::error_code ec;
    std::filesystem::create_directories(jam.parent_path(), ec);
    { std::ofstream file(jam, std::ios::binary); }
    REQUIRE(std::filesystem::exists(jam));

    const auto child = ledger->SpawnSubagent("toolu-1", "读文件并数行数");
    REQUIRE_FALSE(child.has_value());
    CHECK(child.error().stage == "run_started");
    CHECK(child.error().error_code.find("io.create_failed") != std::string::npos);
    CHECK(child.error().retryable);
    // 所有权凭据清理:0 字节残留被 fail_out 清走,subagents/ 不剩任何 .jsonl。
    CHECK_FALSE(std::filesystem::exists(jam));
    CHECK(SubagentJsonlFiles(*ledger).empty());
    const auto report = ledger->VerifySession();
    CHECK(report.error_code.empty());
}

TEST_CASE("SpawnSubagent:正常开卷——文件只在 run.started 之后存在;0 字节残留按凭据清") {
    const auto root = FreshDir("lubancode-traj-spawn-normal");
    auto ledger = OpenLedger(root);
    REQUIRE(ledger.has_value());

    const auto child = ledger->SpawnSubagent("toolu-1", "读文件并数行数");
    REQUIRE(child.has_value());
    const auto path = ledger->session_dir() / "subagents" / ((*child)->run_id() + ".jsonl");
    REQUIRE(std::filesystem::exists(path));
    const auto report = trajectory::VerifyJournalFile(path);
    CHECK(report.ok);
    CHECK(report.events >= 1);  // run.started 已提交,不是空账

    // DiscardUncommittedStream 的所有权凭据:0 字节才清,有内容/目录不碰。
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
