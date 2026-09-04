// Harness Exporter(One-shot 轨迹指定输出单 §四/§八):行 schema、
// messages/requests/tools 投影、usage、outcome 分型、隐私脱敏、子流关联、
// 原子写与收据账。验收原文:API key/Authorization/provider secret 与命中
// 红线的 thinking 正文不出 JSONL;写失败保旧文件、无 .tmp 残件。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "hooks/hash.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/harness_exporter.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/replay.hpp"

using namespace lubancode;
using namespace lubancode::runtime;
using trajectory::Actor;
using trajectory::Durability;
using trajectory::EventKind;
using trajectory::RecordReceipt;

namespace {

std::filesystem::path FreshDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

trajectory::EventScope MainScope() {
    trajectory::EventScope scope;
    scope.workspace_key = "demo-000000000000";
    scope.session_id = "20260904-000001-HARness";
    scope.run_id = "main-0001";
    scope.run_kind = trajectory::RunKind::OneShot;
    scope.visibility = {trajectory::Visibility::HostOnly};
    return scope;
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantTextWithThinking(const std::string& thinking, const std::string& text) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::ThinkingBlock{thinking, "sig"});
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantWithToolCall(const std::string& call_id, const std::string& tool,
                                   const nlohmann::json& arguments) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{"干活。"});
    api::ToolUseBlock call;
    call.id = call_id;
    call.name = tool;
    call.input = arguments;
    message.content.push_back(std::move(call));
    return message;
}

agent::ToolTraceEvent StartedEvent(const std::string& call_id, const std::string& tool,
                                   nlohmann::json effective_arguments) {
    agent::ToolTraceEvent event;
    event.kind = agent::ToolTraceEventKind::ExecutionStarted;
    event.execution_id = "item-" + call_id;
    event.tool_use_id = call_id;
    event.tool_name = tool;
    event.batch_id = "batch-1";
    event.sequence_in_batch = 0;
    event.effective_input_sha256 = std::string(64, '0');
    event.effect_class = agent::EffectClass::LocalReversible;
    event.effective_arguments = std::move(effective_arguments);
    event.timestamp_ms = 1759000000000LL;
    return event;
}

agent::ToolTraceEvent FinishedEvent(const std::string& call_id, const std::string& tool) {
    agent::ToolTraceEvent event;
    event.kind = agent::ToolTraceEventKind::ExecutionFinished;
    event.execution_id = "item-" + call_id;
    event.tool_use_id = call_id;
    event.tool_name = tool;
    event.batch_id = "batch-1";
    event.sequence_in_batch = 0;
    event.outcome = agent::ToolOutcome::Succeeded;
    event.duration_ms = 42;
    event.result_ref.kind = agent::ToolResultRef::Kind::Inline;
    event.result_ref.sha256 = std::string(64, '1');
    event.result_ref.bytes = 12;
    event.timestamp_ms = 1759000000042LL;
    return event;
}

void CommitToolResult(TrajectoryTurnBridge& bridge, const std::string& call_id,
                      const std::string& text, bool is_error = false) {
    api::Message result;
    result.role = api::Role::User;
    result.content.push_back(api::ToolResultBlock{call_id, text, is_error});
    bridge.OnToolResultsCommitted("batch-1", result);
}

std::string ReadFileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::vector<nlohmann::json> ReadJsonl(const std::filesystem::path& path) {
    std::vector<nlohmann::json> out;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        out.push_back(nlohmann::json::parse(line, nullptr, false));
    }
    return out;
}

// 环境快照 blob:与 BuildEnvironmentCapturePayload 的快照同键(provider/
// wire/model/model_parameters/config_snapshot_redacted/lubancode_version)。
std::string EnvironmentSnapshotJson() {
    nlohmann::json snapshot;
    snapshot["lubancode_version"] = "test-0.26.199";
    snapshot["provider"] = "demo";
    snapshot["wire"] = "responses";
    snapshot["model"] = "demo-large";
    snapshot["model_parameters"] = nlohmann::json{{"reasoning_effort", "high"}};
    snapshot["config_snapshot_redacted"] =
        nlohmann::json{{"max_output_tokens", 8192}, {"request_timeout_sec", 120}};
    snapshot["cwd"] = "/workspace/task";
    snapshot["replay_level"] = "exact";
    snapshot["gaps"] = nlohmann::json::array();
    return snapshot.dump();
}

// 拼一枚 one-shot 主 stream。旋钮:
//   with_tool     一枚 read_file 往返(两步模型请求)
//   fail_tool     工具失败(nonzero 退出码)+ is_error 结果 + EndTurn(false)
//   timeout_tool  工具超时(process.timeout)
//   close_run     落 run.completed
//   with_usage    每次请求落 model.usage.recorded(v2)
//   with_thinking 助手输出带 thinking 块
std::filesystem::path BuildHarnessStream(const std::filesystem::path& dir, bool with_tool,
                                         bool fail_tool, bool timeout_tool, bool close_run,
                                         bool with_usage, bool with_thinking,
                                         const std::string& user_text = "跑测试,过了就收工") {
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(),
        [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    REQUIRE(recorder.has_value());
    REQUIRE(recorder->WriteRunStarted(nlohmann::json{{"run_kind", "one_shot"}},
                                      Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    {
        trajectory::BlobStore blobs(dir / "artifacts");
        const auto snapshot = blobs.Store(EnvironmentSnapshotJson(), "application/json",
                                          Durability::PowerLoss);
        REQUIRE(snapshot.has_value());
        trajectory::RecordRequest request;
        request.kind = EventKind::RunEnvironmentCaptured;
        request.scope = recorder->base_scope();
        request.payload = nlohmann::json{{"snapshot_ref", snapshot->ToJson()},
                                         {"replay_level", "exact"},
                                         {"gaps", nlohmann::json::array()}};
        REQUIRE(recorder->Record(std::move(request), Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);
    }
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, MainScope(),
                                                         TrajectoryTurnBridge::Identity{});
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage(user_text));
    const auto record_usage = [&](const std::string& request_id, std::int64_t input,
                                  std::int64_t output) {
        if (!with_usage) {
            return;
        }
        api::Usage usage;
        usage.input_tokens = input;
        usage.output_tokens = output;
        usage.cache_read_tokens = 128;
        usage.cache_creation_tokens = 0;
        // schema 红线:reasoning 不得大于 output,给个零头。
        usage.output_reasoning_tokens = output / 4;
        bridge->OnUsageRecorded(request_id, usage, /*reported_by_provider=*/true, "resp-x");
    };
    const std::string request_id =
        bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    REQUIRE_FALSE(request_id.empty());
    bridge->OnRequestSent(request_id);
    if (with_tool) {
        // 失败/超时走 run_command 形(exit_code 只在 started 带了 command
        // 入参、finished 的 details 带了 exit_code 时才落账,BuildSideEffects
        // 的细账规矩);成功路走 read_file。
        const bool command_shape = fail_tool || timeout_tool;
        const std::string tool_name = command_shape ? "run_command" : "read_file";
        const nlohmann::json tool_args =
            command_shape ? nlohmann::json{{"command", fail_tool ? "make test" : "sleep 99"}}
                          : nlohmann::json{{"path", "src/a.cpp"}};
        REQUIRE(bridge->OnOutputCompleted(
            request_id, AssistantWithToolCall("call-1", tool_name, tool_args), "tool_use", "resp-1"));
        record_usage(request_id, 1000, 50);
        bridge->OnToolTrace(StartedEvent("call-1", tool_name, tool_args));
        if (fail_tool) {
            agent::ToolTraceEvent failed = FinishedEvent("call-1", tool_name);
            failed.outcome = agent::ToolOutcome::ProcessExitNonzero;
            failed.error_code = "process.exit_nonzero";
            failed.details = nlohmann::json{{"exit_code", 2}};
            bridge->OnToolTrace(failed);
            CommitToolResult(*bridge, "call-1", "exit 2:测试没过", true);
        } else if (timeout_tool) {
            agent::ToolTraceEvent timed_out = FinishedEvent("call-1", tool_name);
            timed_out.outcome = agent::ToolOutcome::TimedOut;
            timed_out.error_code = "process.timeout";
            bridge->OnToolTrace(timed_out);
            CommitToolResult(*bridge, "call-1", "超时:30s 上限", true);
        } else {
            bridge->OnToolTrace(FinishedEvent("call-1", tool_name));
            CommitToolResult(*bridge, "call-1", "42 行。");
        }
        const std::string second = bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
        REQUIRE_FALSE(second.empty());
        bridge->OnRequestSent(second);
        REQUIRE(bridge->OnOutputCompleted(
            second, with_thinking ? AssistantTextWithThinking("想想", "收工。") : UserMessage("收工。"),
            "end_turn", "resp-2"));
        record_usage(second, 2000, 80);
    } else {
        REQUIRE(bridge->OnOutputCompleted(
            request_id,
            with_thinking ? AssistantTextWithThinking("想想", "好。") : UserMessage("好。"),
            "end_turn", "resp-1"));
        record_usage(request_id, 1000, 50);
    }
    bridge->EndTurn(!fail_tool && !timeout_tool, false,
                    fail_tool ? "tool_failed" : timeout_tool ? "tool_timeout" : "done");
    if (close_run) {
        REQUIRE(recorder->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss).status ==
                RecordReceipt::Status::Committed);
    }
    return dir / "main.jsonl";
}

}  // namespace

// ---------------------------------------------------------------------------
// outcome 分型(纯函数,§四六型)
// ---------------------------------------------------------------------------

TEST_CASE("ClassifyHarnessOutcome:六型分派表") {
    trajectory::HarnessOutcomeInputs inputs;
    CHECK(std::string(trajectory::ClassifyHarnessOutcome(inputs)) == "unknown");  // 验链不过

    inputs.fold_ok = true;
    inputs.run_terminal = "run.completed";
    inputs.turn_terminals = {"turn.completed"};
    CHECK(std::string(trajectory::ClassifyHarnessOutcome(inputs)) == "success");

    inputs.truncated_tail = true;
    CHECK(std::string(trajectory::ClassifyHarnessOutcome(inputs)) == "partial");
    inputs.truncated_tail = false;

    inputs.run_terminal.clear();  // close 没写成 run terminal(崩溃前缀)
    CHECK(std::string(trajectory::ClassifyHarnessOutcome(inputs)) == "partial");

    inputs.run_terminal = "run.completed";
    inputs.turn_terminals = {"turn.completed", ""};  // 有 turn 没收口
    CHECK(std::string(trajectory::ClassifyHarnessOutcome(inputs)) == "partial");

    inputs.turn_terminals = {"turn.completed", "turn.failed"};
    CHECK(std::string(trajectory::ClassifyHarnessOutcome(inputs)) == "failure");

    inputs.failure_reasons = {"budget exhausted after 12 turns"};
    CHECK(std::string(trajectory::ClassifyHarnessOutcome(inputs)) == "budget_exhausted");
    inputs.failure_reasons.clear();

    inputs.turn_terminals = {"turn.cancelled"};
    CHECK(std::string(trajectory::ClassifyHarnessOutcome(inputs)) == "cancelled");
    inputs.run_terminal = "run.cancelled";
    CHECK(std::string(trajectory::ClassifyHarnessOutcome(inputs)) == "cancelled");
}

// ---------------------------------------------------------------------------
// 行 schema 与投影主体
// ---------------------------------------------------------------------------

TEST_CASE("纯文本 run:一行一枚 run,schema/环境/messages/outcome 齐") {
    const auto dir = FreshDir("lubancode-h1-plain");
    BuildHarnessStream(dir, /*with_tool=*/false, false, false, /*close_run=*/true, true, false);

    const auto records = trajectory::BuildSessionHarnessRecords(dir, {}, /*process_exit_code=*/0);
    REQUIRE(records.size() == 1);
    const auto& record = records[0];
    CHECK(record["schema"] == "lubancode.harness.trajectory");
    CHECK(record["schema_version"] == 1);
    CHECK(record["exporter_version"] == "harness-exporter-v1");
    CHECK(record["session_id"] == "20260904-000001-HARness");
    CHECK(record["workspace_key"] == "demo-000000000000");
    CHECK(record["run_id"] == "main-0001");
    CHECK(record["run_kind"] == "one_shot");
    CHECK(record["parent_run_id"].is_null());

    // 环境快照四件 + 生效参数(§四)。
    CHECK(record["environment"]["provider"] == "demo");
    CHECK(record["environment"]["wire"] == "responses");
    CHECK(record["environment"]["model"] == "demo-large");
    CHECK(record["environment"]["lubancode_version"] == "test-0.26.199");
    CHECK(record["environment"]["model_parameters"]["reasoning_effort"] == "high");
    CHECK(record["environment"]["config_snapshot_redacted"]["max_output_tokens"] == 8192);

    // messages:user + assistant,顺序即事件序。
    REQUIRE(record["messages"].size() == 2);
    CHECK(record["messages"][0]["role"] == "user");
    CHECK(record["messages"][0]["origin"] == "external_user");
    CHECK(record["messages"][0]["content"][0]["text"] == "跑测试,过了就收工");
    CHECK(record["messages"][1]["role"] == "assistant");
    CHECK(record["messages"][1]["stop_reason"] == "end_turn");
    CHECK(record["messages"][1]["content"][0]["text"] == "好。");

    // usage(v2 model.usage.recorded)与汇总。
    REQUIRE(record["requests"].size() == 1);
    CHECK(record["requests"][0]["usage"]["input_tokens"] == 1000);
    CHECK(record["requests"][0]["usage"]["output_tokens"] == 50);
    CHECK(record["requests"][0]["usage"]["cache_read_tokens"] == 128);
    CHECK(record["usage_totals"]["input_tokens"] == 1000);
    CHECK(record["usage_totals"]["output_tokens"] == 50);
    CHECK(record["usage_totals"]["requests_with_reported_usage"] == 1);

    // outcome:turn/run 双收口 -> success;进程退出码透传。
    CHECK(record["outcome"]["status"] == "success");
    CHECK(record["outcome"]["run_terminal"] == "run.completed");
    CHECK(record["outcome"]["session_ended"] == false);
    CHECK(record["outcome"]["process_exit_code"] == 0);

    // 来源锚(§四):stream/末 hash/replay level/config hash/导出时间。
    CHECK(record["source"]["stream"] == "main.jsonl");
    CHECK(record["source"]["journal_last_hash"].get<std::string>().size() == 64);
    CHECK(record["source"]["replay_level"] == "exact");
    CHECK(record["source"]["exporter_config_hash"].get<std::string>().size() == 64);
    CHECK(record["source"]["exported_at"].get<std::string>().find('T') != std::string::npos);
}

TEST_CASE("usage 缺席给 null,不拿 0 冒充;thinking 默认只留 ref") {
    const auto dir = FreshDir("lubancode-h1-nousage");
    BuildHarnessStream(dir, /*with_tool=*/false, false, false, /*close_run=*/true,
                       /*with_usage=*/false, /*with_thinking=*/true);

    const auto records = trajectory::BuildSessionHarnessRecords(dir);
    REQUIRE(records.size() == 1);
    CHECK(records[0]["requests"][0]["usage"].is_null());
    CHECK(records[0]["usage_totals"]["requests_with_reported_usage"] == 0);
    CHECK(records[0]["usage_totals"]["input_tokens"] == 0);

    // thinking 投影按现有隐私策略:默认只留 ref + 省略缘由(§四)。
    bool saw_thinking_ref = false;
    for (const auto& block : records[0]["messages"][1]["content"]) {
        if (block.value("type", std::string()) == "thinking_ref") {
            saw_thinking_ref = true;
            CHECK(block["omitted_reason"] == "thinking_not_authorized");
            CHECK(block["source_event_id"].get<std::string>().size() > 0);
        }
    }
    REQUIRE(saw_thinking_ref);

    // 显式授权才带正文;配置变了 config hash 必变。
    trajectory::HarnessExportOptions with_thinking;
    with_thinking.include_thinking = true;
    const auto rich = trajectory::BuildSessionHarnessRecords(dir, with_thinking);
    REQUIRE(rich.size() == 1);
    bool saw_thinking_text = false;
    for (const auto& block : rich[0]["messages"][1]["content"]) {
        if (block.value("type", std::string()) == "thinking") {
            saw_thinking_text = block["text"] == "想想";
        }
    }
    CHECK(saw_thinking_text);
    CHECK(trajectory::ComputeHarnessConfigHash(with_thinking) !=
          trajectory::ComputeHarnessConfigHash(trajectory::HarnessExportOptions{}));
}

TEST_CASE("单工具往返:tool call/result 成对,tools 台账带有效入参与退出码") {
    const auto dir = FreshDir("lubancode-h1-tool");
    BuildHarnessStream(dir, /*with_tool=*/true, false, false, /*close_run=*/true, true, false);

    const auto records = trajectory::BuildSessionHarnessRecords(dir);
    REQUIRE(records.size() == 1);
    const auto& record = records[0];
    // messages:user -> assistant(tool_calls) -> tool -> assistant。
    REQUIRE(record["messages"].size() == 4);
    CHECK(record["messages"][1]["tool_calls"].size() == 1);
    CHECK(record["messages"][1]["tool_calls"][0]["call_id"] == "call-1");
    CHECK(record["messages"][1]["tool_calls"][0]["name"] == "read_file");
    CHECK(record["messages"][1]["tool_calls"][0]["arguments"]["path"] == "src/a.cpp");
    CHECK(record["messages"][2]["role"] == "tool");
    CHECK(record["messages"][2]["call_id"] == "call-1");
    CHECK(record["messages"][2]["tool_name"] == "read_file");
    CHECK(record["messages"][2]["is_error"] == false);
    CHECK(record["messages"][2]["content"][0]["text"] == "42 行。");
    CHECK(record["messages"][3]["role"] == "assistant");

    // tools 台账:两步请求、工具一枚,细账齐。
    REQUIRE(record["tools"].size() == 1);
    CHECK(record["tools"][0]["tool_name"] == "read_file");
    CHECK(record["tools"][0]["effective_arguments"]["path"] == "src/a.cpp");
    CHECK(record["tools"][0]["terminal_kind"] == "tool.execution.finished");
    CHECK(record["tools"][0]["outcome"] == "succeeded");
    CHECK(record["tools"][0]["result"]["is_error"] == false);
    CHECK(record["requests"].size() == 2);
    CHECK(record["request_retry_summary"]["requests"] == 2);
    CHECK(record["request_retry_summary"]["failed_outputs"] == 0);
    CHECK(record["outcome"]["status"] == "success");
}

TEST_CASE("非零命令退出:error_code 透传,outcome 落 failure;成功命令带 exit_code") {
    const auto dir = FreshDir("lubancode-h1-nonzero");
    BuildHarnessStream(dir, /*with_tool=*/true, /*fail_tool=*/true, false,
                       /*close_run=*/true, true, false);

    const auto records = trajectory::BuildSessionHarnessRecords(dir, {}, /*process_exit_code=*/1);
    REQUIRE(records.size() == 1);
    const auto& tool = records[0]["tools"][0];
    CHECK(tool["terminal_kind"] == "tool.execution.failed");
    // 非零退出在 canonical 账上的事实是 error_code/reason(退出码数值只在
    // Succeeded 支的 command 细账里落,见下桩)。
    CHECK(tool["error_code"] == "process.exit_nonzero");
    CHECK(tool["reason"] == "process.exit_nonzero");
    CHECK(tool["effective_arguments"]["command"] == "make test");
    CHECK(records[0]["messages"][2]["is_error"] == true);
    CHECK(records[0]["messages"][2]["content"][0]["text"] == "exit 2:测试没过");
    CHECK(records[0]["outcome"]["status"] == "failure");
    CHECK(records[0]["outcome"]["process_exit_code"] == 1);
    REQUIRE(records[0]["outcome"].contains("failure_reasons"));
    CHECK(records[0]["outcome"]["failure_reasons"][0] == "tool_failed");

    // exit_code 数值透传:Succeeded 的命令工具,details 带 exit_code 才落账
    //(BuildSideEffects 的 §9.3 细账规矩)。
    const auto dir2 = FreshDir("lubancode-h1-exitcode");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir2 / "main.jsonl", dir2 / "artifacts", MainScope(),
        [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    REQUIRE(recorder.has_value());
    recorder->WriteRunStarted(nlohmann::json{{"run_kind", "one_shot"}}, Durability::PowerLoss);
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, MainScope(),
                                                         TrajectoryTurnBridge::Identity{});
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("跑 make"));
    const std::string request_id =
        bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    bridge->OnRequestSent(request_id);
    REQUIRE(bridge->OnOutputCompleted(
        request_id, AssistantWithToolCall("call-1", "run_command", nlohmann::json{{"command", "make"}}),
        "tool_use", "resp-1"));
    bridge->OnToolTrace(StartedEvent("call-1", "run_command", nlohmann::json{{"command", "make"}}));
    agent::ToolTraceEvent finished = FinishedEvent("call-1", "run_command");
    finished.details = nlohmann::json{{"exit_code", 0}};
    bridge->OnToolTrace(finished);
    CommitToolResult(*bridge, "call-1", "[退出码 0]\nok");
    const std::string closing = bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    bridge->OnRequestSent(closing);
    REQUIRE(bridge->OnOutputCompleted(closing, UserMessage("过了。"), "end_turn", "resp-2"));
    bridge->EndTurn(true, false, "done");
    recorder->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss);
    const auto records2 = trajectory::BuildSessionHarnessRecords(dir2);
    REQUIRE(records2.size() == 1);
    CHECK(records2[0]["tools"][0]["exit_code"] == 0);
    CHECK(records2[0]["outcome"]["status"] == "success");
}

TEST_CASE("工具超时:timeout 稳定码透传,outcome 落 failure") {
    const auto dir = FreshDir("lubancode-h1-timeout");
    BuildHarnessStream(dir, /*with_tool=*/true, false, /*timeout_tool=*/true,
                       /*close_run=*/true, true, false);

    const auto records = trajectory::BuildSessionHarnessRecords(dir);
    REQUIRE(records.size() == 1);
    const auto& tool = records[0]["tools"][0];
    CHECK(tool["terminal_kind"] == "tool.execution.failed");
    CHECK(tool["error_code"] == "process.timeout");
    CHECK(tool["effective_arguments"]["command"] == "sleep 99");
    CHECK(records[0]["outcome"]["status"] == "failure");
}

TEST_CASE("API 重试:失败请求留 output_state=failed,重试摘要计数") {
    const auto dir = FreshDir("lubancode-h1-retry");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(),
        [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    REQUIRE(recorder.has_value());
    recorder->WriteRunStarted(nlohmann::json{{"run_kind", "one_shot"}}, Durability::PowerLoss);
    {
        trajectory::BlobStore blobs(dir / "artifacts");
        const auto snapshot =
            blobs.Store(EnvironmentSnapshotJson(), "application/json", Durability::PowerLoss);
        trajectory::RecordRequest env;
        env.kind = EventKind::RunEnvironmentCaptured;
        env.scope = recorder->base_scope();
        env.payload = nlohmann::json{{"snapshot_ref", snapshot->ToJson()},
                                     {"replay_level", "exact"},
                                     {"gaps", nlohmann::json::array()}};
        recorder->Record(std::move(env), Durability::ProcessCrash);
    }
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, MainScope(),
                                                         TrajectoryTurnBridge::Identity{});
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("问点啥"));
    const std::string first = bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    bridge->OnRequestSent(first);
    bridge->OnOutputFailed(first, "api.timeout");  // 第一次失败
    const std::string second = bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    REQUIRE_FALSE(second.empty());
    bridge->OnRequestSent(second);
    REQUIRE(bridge->OnOutputCompleted(second, UserMessage("重试成功。"), "end_turn", "resp-2"));
    bridge->EndTurn(true, false, "done");
    recorder->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss);

    const auto records = trajectory::BuildSessionHarnessRecords(dir);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0]["requests"].size() == 2);
    CHECK(records[0]["requests"][0]["output_state"] == "failed");
    CHECK(records[0]["requests"][1]["output_state"] == "completed");
    CHECK(records[0]["request_retry_summary"]["failed_outputs"] == 1);
    CHECK(records[0]["outcome"]["status"] == "success");
}

TEST_CASE("未收口与尾行截断:如实 partial,已验证前缀正文照投影") {
    {
        const auto dir = FreshDir("lubancode-h1-partial");
        BuildHarnessStream(dir, /*with_tool=*/false, false, false, /*close_run=*/false, true, false);
        const auto records = trajectory::BuildSessionHarnessRecords(dir);
        REQUIRE(records.size() == 1);
        CHECK(records[0]["outcome"]["status"] == "partial");
        CHECK(records[0]["outcome"]["run_terminal"].is_null());
    }
    {
        const auto dir = FreshDir("lubancode-h1-truncated");
        BuildHarnessStream(dir, /*with_tool=*/false, false, false, /*close_run=*/true, true, false);
        const auto stream = dir / "main.jsonl";
        const auto lines = trajectory::ReadJournalLines(stream);
        REQUIRE(lines.has_value());
        std::string rebuilt;
        for (std::size_t i = 0; i + 1 < lines->size(); ++i) {
            rebuilt += (*lines)[i] + "\n";
        }
        rebuilt += (*lines)[lines->size() - 1];  // 末行无换行:fold 认的截断形
        std::ofstream out(stream, std::ios::binary | std::ios::trunc);
        out << rebuilt;
        out.close();
        const auto records = trajectory::BuildSessionHarnessRecords(dir);
        REQUIRE(records.size() == 1);
        CHECK(records[0]["outcome"]["status"] == "partial");
        CHECK(records[0]["source"]["integrity"]["truncated_tail"] == true);
        CHECK_FALSE(records[0]["messages"].empty());  // 前缀照折,不装没事
    }
}

TEST_CASE("验链失败:存根行 outcome=unknown,不折正文不装 clean success") {
    const auto dir = FreshDir("lubancode-h1-broken");
    BuildHarnessStream(dir, /*with_tool=*/false, false, false, /*close_run=*/true, true, false);
    // 篡改一枚事件正文,hash chain 立刻对不上。
    const auto lines = trajectory::ReadJournalLines(dir / "main.jsonl");
    REQUIRE(lines.has_value());
    REQUIRE(lines->size() > 3);
    std::string tampered = (*lines)[2];
    const auto pos = tampered.find("\"wall_time_ms\"");
    REQUIRE(pos != std::string::npos);
    tampered.replace(pos + 14, 1, "9");
    std::ofstream out(dir / "main.jsonl", std::ios::binary | std::ios::trunc);
    out << (*lines)[0] << "\n" << (*lines)[1] << "\n" << tampered << "\n";
    for (std::size_t i = 3; i < lines->size(); ++i) {
        out << (*lines)[i] << "\n";
    }
    out.close();

    const auto records = trajectory::BuildSessionHarnessRecords(dir);
    REQUIRE(records.size() == 1);
    CHECK(records[0]["outcome"]["status"] == "unknown");
    CHECK(records[0]["source"]["fold_error"] == "replay.verify_failed");
    CHECK(records[0]["messages"].empty());
}

TEST_CASE("多工具同轮:call/result 逐对,tools 台账各就各位") {
    const auto dir = FreshDir("lubancode-h1-multitool");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(),
        [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    REQUIRE(recorder.has_value());
    recorder->WriteRunStarted(nlohmann::json{{"run_kind", "one_shot"}}, Durability::PowerLoss);
    {
        trajectory::BlobStore blobs(dir / "artifacts");
        const auto snapshot =
            blobs.Store(EnvironmentSnapshotJson(), "application/json", Durability::PowerLoss);
        trajectory::RecordRequest env;
        env.kind = EventKind::RunEnvironmentCaptured;
        env.scope = recorder->base_scope();
        env.payload = nlohmann::json{{"snapshot_ref", snapshot->ToJson()},
                                     {"replay_level", "exact"},
                                     {"gaps", nlohmann::json::array()}};
        recorder->Record(std::move(env), Durability::ProcessCrash);
    }
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, MainScope(),
                                                         TrajectoryTurnBridge::Identity{});
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("两件一起办"));
    const std::string request_id =
        bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    bridge->OnRequestSent(request_id);
    // 一枚 assistant 输出带两枚 tool call(并行批)。
    api::Message batch;
    batch.role = api::Role::Assistant;
    api::ToolUseBlock first;
    first.id = "call-a";
    first.name = "read_file";
    first.input = nlohmann::json{{"path", "a.cpp"}};
    api::ToolUseBlock second;
    second.id = "call-b";
    second.name = "search";
    second.input = nlohmann::json{{"pattern", "TODO"}};
    batch.content.push_back(std::move(first));
    batch.content.push_back(std::move(second));
    REQUIRE(bridge->OnOutputCompleted(request_id, batch, "tool_use", "resp-1"));
    bridge->OnToolTrace(StartedEvent("call-a", "read_file", nlohmann::json{{"path", "a.cpp"}}));
    bridge->OnToolTrace(StartedEvent("call-b", "search", nlohmann::json{{"pattern", "TODO"}}));
    bridge->OnToolTrace(FinishedEvent("call-a", "read_file"));
    bridge->OnToolTrace(FinishedEvent("call-b", "search"));
    CommitToolResult(*bridge, "call-a", "a 有 10 行。");
    CommitToolResult(*bridge, "call-b", "TODO 三处。");
    const std::string closing = bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    bridge->OnRequestSent(closing);
    REQUIRE(bridge->OnOutputCompleted(closing, UserMessage("办完了。"), "end_turn", "resp-2"));
    bridge->EndTurn(true, false, "done");
    recorder->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss);

    const auto records = trajectory::BuildSessionHarnessRecords(dir);
    REQUIRE(records.size() == 1);
    const auto& record = records[0];
    REQUIRE(record["tools"].size() == 2);
    CHECK(record["tools"][0]["call_id"] == "call-a");
    CHECK(record["tools"][0]["effective_arguments"]["path"] == "a.cpp");
    CHECK(record["tools"][1]["call_id"] == "call-b");
    CHECK(record["tools"][1]["effective_arguments"]["pattern"] == "TODO");
    // 两枚 call 与两枚 result 在 messages 里逐对成双(§八验收原文)。
    CHECK(record["messages"][1]["tool_calls"].size() == 2);
    CHECK(record["messages"][2]["role"] == "tool");
    CHECK(record["messages"][2]["call_id"] == "call-a");
    CHECK(record["messages"][3]["role"] == "tool");
    CHECK(record["messages"][3]["call_id"] == "call-b");
    CHECK(record["outcome"]["status"] == "success");
}

// ---------------------------------------------------------------------------
// 子流关联(§四:逐流各一行,parent_run_id 相连)
// ---------------------------------------------------------------------------

TEST_CASE("子代理子流:两行 record,child 的 parent_run_id 指回 main") {
    const auto root = FreshDir("lubancode-h1-subagent");
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.lubancode_version = "test";
    options.one_shot = true;
    std::error_code ec;
    std::filesystem::create_directories(root / "repo", ec);
    auto ledger = TrajectorySessionLedger::Open(options);
    REQUIRE(ledger.has_value());
    ledger->CaptureEnvironment([] {
        TrajectorySessionLedger::EnvironmentFacts facts;
        facts.provider = "demo";
        facts.wire = "responses";
        facts.model = "demo-large";
        facts.system_prompt = "你是 LubanCode。";
        facts.toolset.toolset_sha256 = std::string(64, 'e');
        facts.toolset.tool_count = 4;
        return facts;
    }());

    auto child = ledger->SpawnSubagent("toolu-1", "读文件并数行数");
    REQUIRE(child.has_value());
    auto& child_bridge = (*child)->turn_bridge();
    child_bridge.BeginTurn("turn-1", "external_user");
    child_bridge.RecordInput(UserMessage("读文件并数行数"));
    const std::string child_request =
        child_bridge.OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    child_bridge.OnRequestSent(child_request);
    REQUIRE(child_bridge.OnOutputCompleted(child_request, UserMessage("报告:42 行"), "end_turn", "resp-c"));
    child_bridge.EndTurn(true, false, "done");
    const std::string terminal_hash = (*child)->Finish(true, "done");
    CHECK_FALSE(terminal_hash.empty());

    auto main_bridge = ledger->NewTurnBridge({"demo", "responses", "terminal"});
    REQUIRE(main_bridge != nullptr);
    main_bridge->BeginTurn("turn-1", "external_user");
    main_bridge->RecordInput(UserMessage("去读文件"));
    const std::string parent_request =
        main_bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    main_bridge->OnRequestSent(parent_request);
    api::Message with_agent = AssistantWithToolCall("toolu-1", "agent", nlohmann::json{{"task", "读文件"}});
    REQUIRE(main_bridge->OnOutputCompleted(parent_request, with_agent, "tool_use", "resp-p"));
    main_bridge->AttachChildRun("toolu-1", (*child)->run_id());
    main_bridge->NoteChildTerminal((*child)->run_id(), terminal_hash);
    main_bridge->OnToolTrace(StartedEvent("toolu-1", "agent", nlohmann::json{{"task", "读文件"}}));
    main_bridge->OnToolTrace(FinishedEvent("toolu-1", "agent"));
    CommitToolResult(*main_bridge, "toolu-1", "报告:42 行");
    main_bridge->EndTurn(true, false, "done");

    const auto records = trajectory::BuildSessionHarnessRecords(ledger->session_dir());
    REQUIRE(records.size() == 2);
    // 行序:main.jsonl + subagents/<run>.jsonl 按字典序,main 在前。
    CHECK(records[0]["source"]["stream"] == "main.jsonl");
    CHECK(records[0]["run_kind"] == "one_shot");
    CHECK(records[0]["parent_run_id"].is_null());
    // main 行的 agent 工具带子流引用,不内联子正文。
    bool child_link_found = false;
    for (const auto& tool : records[0]["tools"]) {
        if (tool.value("call_id", std::string()) == "toolu-1") {
            child_link_found = tool.contains("child_run_id");
            CHECK(tool["child_run_id"] == (*child)->run_id());
            CHECK(tool["child_terminal_event_hash"] == terminal_hash);
        }
    }
    CHECK(child_link_found);

    // child 行:run_kind=subagent,parent_run_id 指回 main run。
    const auto& child_record = records[1];
    CHECK(child_record["source"]["stream"].get<std::string>().rfind("subagents/", 0) == 0);
    CHECK(child_record["run_kind"] == "subagent");
    CHECK(child_record["run_id"] == (*child)->run_id());
    CHECK(child_record["parent_run_id"] == records[0]["run_id"]);
    // 子正文只在子行出现一次。
    const std::string marker = "读文件并数行数";
    CHECK(records[1].dump().find(marker) != std::string::npos);
    CHECK(records[0].dump().find(marker) == std::string::npos);
}

// ---------------------------------------------------------------------------
// 隐私(§八:API key/Authorization/provider secret 不出 JSONL)
// ---------------------------------------------------------------------------

TEST_CASE("secret 命中:正文脱敏不丢行,privacy_findings 留稳定码") {
    const std::string secret_line =
        "curl -H 'Authorization: Bearer eyJhbGciOi9ub25lJ9.deadbeef' https://x";
    const auto dir = FreshDir("lubancode-h1-secret");
    BuildHarnessStream(dir, /*with_tool=*/false, false, false, /*close_run=*/true, true, false,
                       "帮我跑 " + secret_line);

    const auto target = dir / "logs" / "trajectory.jsonl";
    const auto report = trajectory::ExportSessionHarnessV1(dir, target);
    REQUIRE(report.ok());
    CHECK(report.records == 1);
    CHECK(report.sha256.size() == 64);
    CHECK(report.target == target);

    // 文件里没有密钥原文;行还在(脱敏是处置面,丢行不是)。
    const std::string content = ReadFileText(target);
    CHECK(content.find("eyJhbGciOi9ub25lJ9") == std::string::npos);
    const auto lines = ReadJsonl(target);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0]["outcome"]["status"] == "success");
    REQUIRE(lines[0].contains("privacy_findings"));
    bool secret_code = false;
    for (const auto& finding : lines[0]["privacy_findings"]) {
        if (finding["code"].get<std::string>().rfind("privacy.secret.", 0) == 0) {
            secret_code = true;
        }
    }
    CHECK(secret_code);
    // sha256 收据对得上内容。
    CHECK(report.sha256 == hooks::Sha256Hex(content));
}

TEST_CASE("thinking 带正文时也过 secret 脱敏") {
    const auto dir = FreshDir("lubancode-h1-thinksecret");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(),
        [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    recorder->WriteRunStarted(nlohmann::json{{"run_kind", "one_shot"}}, Durability::PowerLoss);
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, MainScope(),
                                                         TrajectoryTurnBridge::Identity{});
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("想想"));
    const std::string request_id =
        bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    bridge->OnRequestSent(request_id);
    REQUIRE(bridge->OnOutputCompleted(
        request_id,
        AssistantTextWithThinking("api_key=sk-live-1234567890abcdef 拿这把钥匙", "好。"),
        "end_turn", "resp-1"));
    bridge->EndTurn(true, false, "done");
    recorder->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss);

    trajectory::HarnessExportOptions options;
    options.include_thinking = true;
    const auto records = trajectory::BuildSessionHarnessRecords(dir, options);
    REQUIRE(records.size() == 1);
    const std::string dumped = records[0].dump();
    CHECK(dumped.find("sk-live-1234567890abcdef") == std::string::npos);
    CHECK(dumped.find("REDACTED") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 大结果与原子性(§三/§八)
// ---------------------------------------------------------------------------

TEST_CASE("大工具结果只内联 head 摘要,不无上限膨胀") {
    const auto dir = FreshDir("lubancode-h1-bigresult");
    BuildHarnessStream(dir, /*with_tool=*/true, false, false, /*close_run=*/true, true, false,
                       "跑测试");
    trajectory::HarnessExportOptions options;
    options.max_inline_tool_result_bytes = 4;  // "42 行。" 9 字节,必超
    const auto records = trajectory::BuildSessionHarnessRecords(dir, options);
    REQUIRE(records.size() == 1);
    const auto& result_block = records[0]["messages"][2]["content"][0];
    CHECK(result_block["type"] == "text_ref");
    CHECK(result_block["truncated"] == true);
    CHECK(result_block["bytes"] == std::string("42 行。").size());
    CHECK(result_block["head"].get<std::string>().size() <= 16);
}

TEST_CASE("原子写:无 .tmp 残件;目标只改点名文件;空账不造假") {
    const auto dir = FreshDir("lubancode-h1-atomic");
    BuildHarnessStream(dir, false, false, false, true, true, false);
    const auto logs = dir / "logs";
    std::filesystem::create_directories(logs);
    // 同目录放一份无辜文件:导出不得碰它。
    const auto bystander = logs / "bystander.txt";
    {
        std::ofstream out(bystander, std::ios::binary);
        out << "别动我";
    }
    const auto target = logs / "trajectory.jsonl";
    const auto report = trajectory::ExportSessionHarnessV1(dir, target);
    REQUIRE(report.ok());
    CHECK(std::filesystem::exists(target));
    CHECK(ReadFileText(bystander) == "别动我");
    // 目录里没有 .tmp 残件。
    for (const auto& entry : std::filesystem::directory_iterator(logs)) {
        CHECK(entry.path().extension() != ".tmp");
    }
    // 重导:原子替换旧成品,仍无残件。
    const auto again = trajectory::ExportSessionHarnessV1(dir, target);
    REQUIRE(again.ok());
    for (const auto& entry : std::filesystem::directory_iterator(logs)) {
        CHECK(entry.path().extension() != ".tmp");
    }

    // 空账/没目录:稳定码,不造假。
    const auto empty = trajectory::ExportSessionHarnessV1(FreshDir("lubancode-h1-empty"),
                                                          logs / "e.jsonl");
    CHECK(empty.error_code == "export.no_streams");
    CHECK_FALSE(std::filesystem::exists(logs / "e.jsonl"));
}

TEST_CASE("写失败:目标形状坏给稳定码,不留半截成品") {
    const auto dir = FreshDir("lubancode-h1-wfail");
    BuildHarnessStream(dir, false, false, false, true, true, false);
    const auto logs = dir / "logs";
    std::filesystem::create_directories(logs);
    // 目标是目录:replace 失败,报 write_failed。
    std::filesystem::create_directories(logs / "iamdir.jsonl");
    const auto as_dir = trajectory::ExportSessionHarnessV1(dir, logs / "iamdir.jsonl");
    CHECK(as_dir.error_code == "export.write_failed");
    CHECK_FALSE(as_dir.ok());
    // 父路径撞上普通文件:mkdir 失败。
    std::ofstream blocker(logs / "blocker");
    blocker.close();
    const auto blocked = trajectory::ExportSessionHarnessV1(dir, logs / "blocker" / "t.jsonl");
    CHECK(blocked.error_code == "export.write_failed");
    // 磁盘门:一个字节不写(§12.2 同款判据)。
    trajectory::HarnessExportOptions starved;
    starved.min_free_bytes = std::numeric_limits<std::uint64_t>::max();
    const auto exhausted =
        trajectory::ExportSessionHarnessV1(dir, logs / "starved.jsonl", starved);
    CHECK(exhausted.error_code == "export.storage_exhausted");
    CHECK_FALSE(std::filesystem::exists(logs / "starved.jsonl"));
}

#ifdef _WIN32
TEST_CASE("写失败保旧文件(Windows):目标只读挡住替换,旧成品原样可读") {
    const auto dir = FreshDir("lubancode-h1-keepold");
    BuildHarnessStream(dir, false, false, false, true, true, false);
    const auto logs = dir / "logs";
    std::filesystem::create_directories(logs);
    const auto target = logs / "trajectory.jsonl";
    REQUIRE(trajectory::ExportSessionHarnessV1(dir, target).ok());
    const std::string old_content = ReadFileText(target);
    CHECK_FALSE(old_content.empty());
    // 只读挡住 MoveFileEx 的替换:再导一次必失败。
    std::filesystem::permissions(target, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::group_read |
                                             std::filesystem::perms::others_read);
    const auto failed = trajectory::ExportSessionHarnessV1(dir, target);
    std::error_code perm_ec;
    std::filesystem::permissions(target, std::filesystem::perms::owner_all, perm_ec);
    CHECK(failed.error_code == "export.write_failed");
    // 旧成品一字节没动(§三:失败不留下冒充成品的半截 JSONL)。
    CHECK(ReadFileText(target) == old_content);
}
#endif
