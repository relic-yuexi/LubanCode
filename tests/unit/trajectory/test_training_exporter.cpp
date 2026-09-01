// P0-5 Training Exporter(§十一/§十二/§16.5 训练门):episode schema、
// 四路裁断、origin 映射、质量轴、隐私扫描、manifest 与确定性 fingerprint。
// 验收原文:同一 Journal 重编字节一致;无证据成功进不了 success。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "hooks/hash.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/blob_store.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/training_exporter.hpp"

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
    scope.session_id = "20260831-000001-AAAAAA";
    scope.run_id = "main-0001";
    scope.run_kind = trajectory::RunKind::MainSession;
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

// 拼一枚已收口的主会话 stream。行为旋钮:
//   replay_level   环境快照档位("" = 不落环境事件)
//   with_tool      一枚只读工具往返
//   with_evidence  测试验证点(成功门)
//   fail_tool      工具失败 + is_error 结果 + EndTurn(false)
//   close_run      落 run.completed
std::filesystem::path BuildMainStream(const std::filesystem::path& dir,
                                      const std::string& replay_level, bool with_tool,
                                      bool with_evidence, bool fail_tool, bool close_run,
                                      const std::string& user_text = "跑测试,过了就收工") {
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(),
        [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    REQUIRE(recorder.has_value());
    REQUIRE(recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}},
                                      Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    if (!replay_level.empty()) {
        trajectory::BlobStore blobs(dir / "artifacts");
        const auto snapshot = blobs.Store("{\"os\":\"win\"}", "application/json",
                                          Durability::PowerLoss);
        REQUIRE(snapshot.has_value());
        trajectory::RecordRequest request;
        request.kind = EventKind::RunEnvironmentCaptured;
        request.scope = recorder->base_scope();
        request.payload = nlohmann::json{{"snapshot_ref", snapshot->ToJson()},
                                         {"replay_level", replay_level},
                                         {"gaps", nlohmann::json::array()}};
        REQUIRE(recorder->Record(std::move(request), Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);
    }
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, MainScope(),
                                                         TrajectoryTurnBridge::Identity{});
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage(user_text));
    const std::string request_id =
        bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    REQUIRE_FALSE(request_id.empty());
    bridge->OnRequestSent(request_id);
    if (with_tool) {
        REQUIRE(bridge->OnOutputCompleted(
            request_id, AssistantWithToolCall("call-1", "read_file", nlohmann::json{{"path", "src/a.cpp"}}),
            "tool_use", "resp-1"));
        bridge->OnToolTrace(StartedEvent("call-1", "read_file", nlohmann::json{{"path", "src/a.cpp"}}));
        if (fail_tool) {
            agent::ToolTraceEvent failed = FinishedEvent("call-1", "read_file");
            failed.outcome = agent::ToolOutcome::ToolError;
            failed.error_code = "file_missing";
            bridge->OnToolTrace(failed);
            CommitToolResult(*bridge, "call-1", "读不动:文件没了", true);
        } else {
            bridge->OnToolTrace(FinishedEvent("call-1", "read_file"));
            CommitToolResult(*bridge, "call-1", "42 行。");
        }
        const std::string second = bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
        REQUIRE_FALSE(second.empty());
        bridge->OnRequestSent(second);
        REQUIRE(bridge->OnOutputCompleted(second, UserMessage("收工。"), "end_turn", "resp-2"));
    } else {
        REQUIRE(bridge->OnOutputCompleted(request_id, UserMessage("好。"), "end_turn", "resp-1"));
    }
    if (with_evidence) {
        const std::string verification = bridge->BeginVerification("test_suite", "build/tests.exe", "ctest");
        REQUIRE_FALSE(verification.empty());
        bridge->FinishVerification(verification, true, nlohmann::json{{"passed", 3}});
    }
    bridge->EndTurn(!fail_tool, false, fail_tool ? "tool_failed" : "done");
    if (close_run) {
        REQUIRE(recorder->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss).status ==
                RecordReceipt::Status::Committed);
    }
    return dir / "main.jsonl";
}

}  // namespace

// ---------------------------------------------------------------------------
// 四路裁断(§11.3/§11.4/§11.5)
// ---------------------------------------------------------------------------

TEST_CASE("DecideEpisodeRoute:轴到路的分派表与挡的次序") {
    const auto routing_of = [] {
        trajectory::EpisodeRouting routing;
        routing.structure = "valid";
        routing.privacy = "passed";
        routing.replayability = "exact_offline";
        routing.completeness = "complete";
        routing.verification = "verified";
        routing.assessed_outcome = "succeeded";
        routing.turn_terminal = "turn.completed";
        routing.run_terminal = "run.completed";
        return routing;
    };
    // 全绿 -> success。
    CHECK(trajectory::DecideEpisodeRoute(routing_of(), nullptr) == trajectory::EpisodeRoute::Success);
    // 少证据:成功自述进不了 success(验收原文),落 excluded/unverified。
    {
        auto routing = routing_of();
        routing.verification = "unverified";
        std::vector<std::string> reasons;
        const auto route = trajectory::DecideEpisodeRoute(routing, &reasons);
        CHECK(route == trajectory::EpisodeRoute::Excluded);
        CHECK(std::find(reasons.begin(), reasons.end(), "verification.unverified") != reasons.end());
    }
    // 结构/隐私/replay 档/unknown side effect 各自一票否决,且优先于其余。
    {
        auto routing = routing_of();
        routing.structure = "truncated_tail";
        CHECK(trajectory::DecideEpisodeRoute(routing, nullptr) == trajectory::EpisodeRoute::Excluded);
    }
    {
        auto routing = routing_of();
        routing.privacy = "excluded";
        CHECK(trajectory::DecideEpisodeRoute(routing, nullptr) == trajectory::EpisodeRoute::Excluded);
    }
    {
        auto routing = routing_of();
        routing.replayability = "input_only";
        std::vector<std::string> reasons;
        CHECK(trajectory::DecideEpisodeRoute(routing, &reasons) == trajectory::EpisodeRoute::Excluded);
        CHECK(std::find(reasons.begin(), reasons.end(), "replay.level_below_floor") != reasons.end());
    }
    {
        auto routing = routing_of();
        routing.replayability = "unknown";
        std::vector<std::string> reasons;
        CHECK(trajectory::DecideEpisodeRoute(routing, &reasons) == trajectory::EpisodeRoute::Excluded);
        CHECK(std::find(reasons.begin(), reasons.end(), "replay.level_missing") != reasons.end());
    }
    {
        auto routing = routing_of();
        routing.unknown_side_effect = true;
        std::vector<std::string> reasons;
        CHECK(trajectory::DecideEpisodeRoute(routing, &reasons) == trajectory::EpisodeRoute::Excluded);
        CHECK(std::find(reasons.begin(), reasons.end(), "tool.unknown_side_effect") != reasons.end());
    }
    // 已知失败/取消,结构完整 -> failure(§11.3)。
    {
        auto routing = routing_of();
        routing.verification = "verified";
        routing.assessed_outcome = "failed";
        routing.turn_terminal = "turn.failed";
        CHECK(trajectory::DecideEpisodeRoute(routing, nullptr) == trajectory::EpisodeRoute::Failure);
    }
    {
        auto routing = routing_of();
        routing.verification = "unverified";
        routing.turn_terminal = "turn.cancelled";
        CHECK(trajectory::DecideEpisodeRoute(routing, nullptr) == trajectory::EpisodeRoute::Failure);
    }
    // 预算耗尽/人工打断/未收口 -> partial。
    {
        auto routing = routing_of();
        routing.completeness = "incomplete";
        routing.turn_terminal = "turn.cancelled";
        CHECK(trajectory::DecideEpisodeRoute(routing, nullptr) == trajectory::EpisodeRoute::Partial);
    }
    {
        auto routing = routing_of();
        routing.completeness = "incomplete";
        routing.turn_terminal = "";  // 崩在 turn 中途
        CHECK(trajectory::DecideEpisodeRoute(routing, nullptr) == trajectory::EpisodeRoute::Partial);
    }
    // 档不够时即便全绿也进不了训练集:挡的次序在 completeness 之前。
    {
        auto routing = routing_of();
        routing.replayability = "blocked";
        routing.completeness = "incomplete";
        CHECK(trajectory::DecideEpisodeRoute(routing, nullptr) == trajectory::EpisodeRoute::Excluded);
    }
}

// ---------------------------------------------------------------------------
// 全绿 success 链 + schema + 确定性
// ---------------------------------------------------------------------------

TEST_CASE("完整成功 turn 导 success:episode schema、origin 映射、manifest、字节确定") {
    const auto dir = FreshDir("lubancode-p5-success");
    BuildMainStream(dir, "exact", /*with_tool=*/true, /*with_evidence=*/true,
                    /*fail_tool=*/false, /*close_run=*/true);

    const auto episodes = trajectory::BuildSessionTrainingEpisodes(dir);
    REQUIRE(episodes.size() == 1);
    CHECK(episodes[0].route == trajectory::EpisodeRoute::Success);
    const auto& episode = episodes[0].episode;

    // §11.1 全形。
    CHECK(episode["schema"] == "lubancode.training.episode");
    CHECK(episode["schema_version"] == 1);
    CHECK(episode["episode_id"] == "main-0001:turn-1");
    CHECK(episode["source"]["workspace_key"] == "demo-000000000000");
    CHECK(episode["source"]["session_id"] == "20260831-000001-AAAAAA");
    CHECK(episode["source"]["run_id"] == "main-0001");
    CHECK(episode["source"]["run_kind"] == "main_session");
    CHECK(episode["source"]["turn_id"] == "turn-1");
    CHECK(episode["source"]["journal_last_hash"].get<std::string>().size() == 64);
    CHECK(episode["source"]["exporter_version"] == "trajectory-exporter-v1");
    CHECK(trajectory::IsHex64(episode["fingerprint"].get<std::string>()));

    // §11.2 origin 映射:user/assistant(tool_calls)/tool 各就各位。
    const auto& messages = episode["messages"];
    REQUIRE(messages.size() == 4);
    CHECK(messages[0]["role"] == "user");
    CHECK(messages[0]["origin"] == "external_user");
    CHECK(messages[0]["content"][0]["text"] == "跑测试,过了就收工");
    CHECK(messages[1]["role"] == "assistant");
    CHECK(messages[1]["origin"] == "provider_model");
    REQUIRE(messages[1]["tool_calls"].size() == 1);
    CHECK(messages[1]["tool_calls"][0]["call_id"] == "call-1");
    CHECK(messages[1]["tool_calls"][0]["name"] == "read_file");
    CHECK(messages[1]["tool_calls"][0]["arguments"]["path"] == "src/a.cpp");
    CHECK(messages[2]["role"] == "tool");
    CHECK(messages[2]["call_id"] == "call-1");
    CHECK(messages[2]["tool_name"] == "read_file");
    CHECK(messages[2]["is_error"] == false);
    CHECK(messages[2]["content"][0]["text"] == "42 行。");
    CHECK(messages[3]["role"] == "assistant");
    // steps 台账齐全;证据与裁断成对。
    REQUIRE(episode["steps"].size() == 1);
    CHECK(episode["steps"][0]["terminal_kind"] == "tool.execution.finished");
    CHECK(episode["steps"][0]["result_committed"] == true);
    REQUIRE(episode["evidence"].size() == 1);
    CHECK(episode["evidence"][0]["kind"] == "test_suite");
    CHECK(episode["outcome"]["assessed_outcome"] == "succeeded");
    REQUIRE(episode["outcome"]["evidence_refs"].size() == 1);
    CHECK(episode["outcome"]["evidence_refs"][0]["fresh"] == true);
    // 质量轴全绿(§11.4)。
    CHECK(episode["quality"]["structure"] == "valid");
    CHECK(episode["quality"]["outcome"] == "succeeded");
    CHECK(episode["quality"]["verification"] == "verified");
    CHECK(episode["quality"]["privacy"] == "passed");
    CHECK(episode["quality"]["replayability"] == "exact_offline");
    CHECK(episode["quality"]["completeness"] == "complete");
    CHECK(episode["quality"]["training_eligible"] == true);
    CHECK(episode["replay"]["replay_level"] == "exact");
    CHECK(trajectory::IsHex64(episode["replay"]["state_hash"].get<std::string>()));

    // 落盘 + manifest。
    const auto report = trajectory::ExportSessionTrainingV1(dir);
    REQUIRE(report.ok());
    const auto export_dir = dir / "exports" / "training-v1";
    CHECK(report.export_dir == export_dir);
    CHECK(report.counts.at("success") == 1);
    CHECK(report.episodes == 1);
    for (const char* name : {"success.jsonl", "failure.jsonl", "partial.jsonl", "excluded.jsonl",
                             "manifest.json"}) {
        CHECK(std::filesystem::exists(export_dir / name));
    }
    const auto manifest = nlohmann::json::parse(ReadFileText(export_dir / "manifest.json"));
    CHECK(manifest["schema"] == "lubancode.training.dataset");
    CHECK(manifest["format"] == "training-v1");
    CHECK(manifest["counts"]["success"] == 1);
    CHECK(manifest["authorization"]["auto_export_training"] == false);
    CHECK(manifest["authorization"]["source"] == "explicit_cli_command");
    CHECK(manifest["filter_rules"]["reasoning_policy"] == "excluded");
    CHECK(manifest["filter_rules"]["replay_level_floor"][0] == "exact");
    CHECK(manifest["source"]["workspace_key"] == "demo-000000000000");
    CHECK(manifest["source"]["source_last_event_wall_time_ms"].is_number());
    // 逐文件 sha256 对得上内容。
    bool manifest_file_ok = false;
    for (const auto& file : manifest["files"]) {
        if (file["name"] == "success.jsonl") {
            CHECK(file["episodes"] == 1);
            CHECK(file["sha256"] == hooks::Sha256Hex(ReadFileText(export_dir / "success.jsonl")));
            manifest_file_ok = true;
        }
    }
    CHECK(manifest_file_ok);

    // §16.5 验收:同一 Journal 连导两次,输出逐字节一致(含 manifest)。
    const std::string first_success = ReadFileText(export_dir / "success.jsonl");
    const std::string first_manifest = ReadFileText(export_dir / "manifest.json");
    const auto again = trajectory::ExportSessionTrainingV1(dir);
    REQUIRE(again.ok());
    CHECK(ReadFileText(export_dir / "success.jsonl") == first_success);
    CHECK(ReadFileText(export_dir / "manifest.json") == first_manifest);
    // Build 也是纯函数:重编 episode 逐位相同。
    const auto rebuilt = trajectory::BuildSessionTrainingEpisodes(dir);
    REQUIRE(rebuilt.size() == 1);
    CHECK(rebuilt[0].fingerprint == episodes[0].fingerprint);
    CHECK(rebuilt[0].episode == episodes[0].episode);
}

// ---------------------------------------------------------------------------
// 成功门(§11.5 验收原文)
// ---------------------------------------------------------------------------

TEST_CASE("无证据成功进不了 success:纯问答 turn 落 excluded/unverified") {
    const auto dir = FreshDir("lubancode-p5-unverified");
    BuildMainStream(dir, "source_exact_environment_partial", /*with_tool=*/false,
                    /*with_evidence=*/false, /*fail_tool=*/false, /*close_run=*/true);
    const auto episodes = trajectory::BuildSessionTrainingEpisodes(dir);
    REQUIRE(episodes.size() == 1);
    CHECK(episodes[0].route == trajectory::EpisodeRoute::Excluded);
    CHECK(std::find(episodes[0].reasons.begin(), episodes[0].reasons.end(),
                    "verification.unverified") != episodes[0].reasons.end());
    CHECK(episodes[0].episode["quality"]["verification"] == "unverified");
    CHECK(episodes[0].episode["quality"]["training_eligible"] == false);
    // turn.completed 自称 succeeded 不算数:assessed_outcome 须为空。
    CHECK(episodes[0].episode["outcome"]["claimed_outcome"] == "succeeded");
    CHECK(episodes[0].episode["outcome"]["assessed_outcome"].is_null());
}

TEST_CASE("工具失败正常收口进 failure;is_error 与 outcome 如实") {
    const auto dir = FreshDir("lubancode-p5-failure");
    BuildMainStream(dir, "exact", /*with_tool=*/true, /*with_evidence=*/true,
                    /*fail_tool=*/true, /*close_run=*/true);
    const auto episodes = trajectory::BuildSessionTrainingEpisodes(dir);
    REQUIRE(episodes.size() == 1);
    CHECK(episodes[0].route == trajectory::EpisodeRoute::Failure);
    const auto& episode = episodes[0].episode;
    // 失败工具不被 exporter 写成成功(§16.4)。
    CHECK(episode["steps"][0]["terminal_kind"] == "tool.execution.failed");
    CHECK(episode["messages"][2]["is_error"] == true);
    CHECK(episode["quality"]["structure"] == "valid");
}

TEST_CASE("unknown side effect 整枚 excluded") {
    const auto dir2 = FreshDir("lubancode-p5-unknown");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir2 / "main.jsonl", dir2 / "artifacts", MainScope(),
        [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    REQUIRE(recorder.has_value());
    recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}}, Durability::PowerLoss);
    trajectory::BlobStore blobs(dir2 / "artifacts");
    const auto snapshot = blobs.Store("{}", "application/json", Durability::PowerLoss);
    trajectory::RecordRequest env;
    env.kind = EventKind::RunEnvironmentCaptured;
    env.scope = recorder->base_scope();
    env.payload = nlohmann::json{{"snapshot_ref", snapshot->ToJson()},
                                 {"replay_level", "exact"},
                                 {"gaps", nlohmann::json::array()}};
    REQUIRE(recorder->Record(std::move(env), Durability::ProcessCrash).status ==
            RecordReceipt::Status::Committed);
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, MainScope(),
                                                         TrajectoryTurnBridge::Identity{});
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("删远端分支"));
    const std::string request_id =
        bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    bridge->OnRequestSent(request_id);
    REQUIRE(bridge->OnOutputCompleted(
        request_id, AssistantWithToolCall("call-x", "run_command", nlohmann::json{{"command", "rm -r build"}}),
        "tool_use", "resp-1"));
    bridge->OnToolTrace(StartedEvent("call-x", "run_command", nlohmann::json{{"command", "rm -r build"}}));
    agent::ToolTraceEvent unknown;
    unknown.kind = agent::ToolTraceEventKind::ExecutionFinished;
    unknown.execution_id = "item-call-x";
    unknown.tool_use_id = "call-x";
    unknown.tool_name = "run_command";
    unknown.batch_id = "batch-1";
    unknown.outcome = agent::ToolOutcome::UnknownAfterStart;
    unknown.error_code = "process_killed";
    bridge->OnToolTrace(unknown);
    bridge->EndTurn(false, false, "unknown_outcome");
    recorder->FinishRun(EventKind::RunFailed, "unknown_outcome", Durability::PowerLoss);

    const auto episodes = trajectory::BuildSessionTrainingEpisodes(dir2);
    REQUIRE(episodes.size() == 1);
    CHECK(episodes[0].route == trajectory::EpisodeRoute::Excluded);
    CHECK(std::find(episodes[0].reasons.begin(), episodes[0].reasons.end(),
                    "tool.unknown_side_effect") != episodes[0].reasons.end());
}

TEST_CASE("replay 档过滤:input_only/blocked/缺环境档都配不进训练集") {
    {
        const auto dir = FreshDir("lubancode-p5-inputonly");
        BuildMainStream(dir, "input_only", true, true, false, true);
        const auto episodes = trajectory::BuildSessionTrainingEpisodes(dir);
        REQUIRE(episodes.size() == 1);
        CHECK(episodes[0].route == trajectory::EpisodeRoute::Excluded);
        CHECK(std::find(episodes[0].reasons.begin(), episodes[0].reasons.end(),
                        "replay.level_below_floor") != episodes[0].reasons.end());
        CHECK(episodes[0].episode["quality"]["replayability"] == "input_only");
    }
    {
        const auto dir = FreshDir("lubancode-p5-noenv");
        BuildMainStream(dir, /*replay_level=*/"", true, true, false, true);
        const auto episodes = trajectory::BuildSessionTrainingEpisodes(dir);
        REQUIRE(episodes.size() == 1);
        CHECK(episodes[0].route == trajectory::EpisodeRoute::Excluded);
        CHECK(std::find(episodes[0].reasons.begin(), episodes[0].reasons.end(),
                        "replay.level_missing") != episodes[0].reasons.end());
    }
}

TEST_CASE("证据先落、改动在后:晚于最后修改的证据才算 fresh(§11.5)") {
    const auto dir = FreshDir("lubancode-p5-stale");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(),
        [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    REQUIRE(recorder.has_value());
    recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}}, Durability::PowerLoss);
    trajectory::BlobStore blobs(dir / "artifacts");
    const auto snapshot = blobs.Store("{}", "application/json", Durability::PowerLoss);
    trajectory::RecordRequest env;
    env.kind = EventKind::RunEnvironmentCaptured;
    env.scope = recorder->base_scope();
    env.payload = nlohmann::json{{"snapshot_ref", snapshot->ToJson()},
                                 {"replay_level", "exact"},
                                 {"gaps", nlohmann::json::array()}};
    REQUIRE(recorder->Record(std::move(env), Durability::ProcessCrash).status ==
            RecordReceipt::Status::Committed);
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, MainScope(),
                                                         TrajectoryTurnBridge::Identity{});
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage("先验,后改"));
    // 证据先落(盯 build/tests.exe)。
    const std::string verification = bridge->BeginVerification("test_suite", "build/tests.exe", "ctest");
    bridge->FinishVerification(verification, true, nlohmann::json{{"passed", 1}});
    // 再改另一个文件(不触发 subject 失效,只动 observed_after_seq 对照线)。
    const std::string request_id =
        bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    bridge->OnRequestSent(request_id);
    REQUIRE(bridge->OnOutputCompleted(
        request_id, AssistantWithToolCall("call-m", "edit_file", nlohmann::json{{"path", "src/other.cpp"}}),
        "tool_use", "resp-1"));
    bridge->OnToolTrace(StartedEvent("call-m", "edit_file", nlohmann::json{{"path", "src/other.cpp"}}));
    agent::ToolTraceEvent finished = FinishedEvent("call-m", "edit_file");
    finished.undo.path = "src/other.cpp";  // 副作用细账落上
    bridge->OnToolTrace(finished);
    CommitToolResult(*bridge, "call-m", "改好。");
    const std::string second = bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    bridge->OnRequestSent(second);
    REQUIRE(bridge->OnOutputCompleted(second, UserMessage("完。"), "end_turn", "resp-2"));
    bridge->EndTurn(true, false, "done");
    recorder->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss);

    const auto episodes = trajectory::BuildSessionTrainingEpisodes(dir);
    REQUIRE(episodes.size() == 1);
    CHECK(episodes[0].episode["quality"]["verification"] == "stale");
    CHECK(episodes[0].route == trajectory::EpisodeRoute::Excluded);
    CHECK(std::find(episodes[0].reasons.begin(), episodes[0].reasons.end(),
                    "verification.stale") != episodes[0].reasons.end());
}

TEST_CASE("未收口 run 导 partial:预算耗尽/人工打断/崩溃在途") {
    const auto dir = FreshDir("lubancode-p5-partial");
    BuildMainStream(dir, "exact", true, true, false, /*close_run=*/false);
    const auto episodes = trajectory::BuildSessionTrainingEpisodes(dir);
    REQUIRE(episodes.size() == 1);
    CHECK(episodes[0].route == trajectory::EpisodeRoute::Partial);
    CHECK(episodes[0].episode["quality"]["completeness"] == "incomplete");
}

// ---------------------------------------------------------------------------
// 隐私扫描(§十二)
// ---------------------------------------------------------------------------

TEST_CASE("密钥命中进 excluded:报 source event id,导出件里没有密钥原文") {
    const std::string secret_line = "curl -H 'Authorization: Bearer eyJhbGciOi9ub25lJ9.deadbeef' https://x";
    const auto dir = FreshDir("lubancode-p5-secret");
    BuildMainStream(dir, "exact", false, false, false, true, "帮我跑 " + secret_line);
    const auto episodes = trajectory::BuildSessionTrainingEpisodes(dir);
    REQUIRE(episodes.size() == 1);
    CHECK(episodes[0].route == trajectory::EpisodeRoute::Excluded);
    CHECK(episodes[0].episode["quality"]["privacy"] == "excluded");
    // 报 source event id(§十二:不改 Journal,只报账)。
    REQUIRE(episodes[0].episode.contains("privacy_findings"));
    const auto& finding = episodes[0].episode["privacy_findings"][0];
    CHECK(finding["code"].get<std::string>().find("privacy.secret.") == 0);
    CHECK(finding["source_event_id"].get<std::string>().rfind("main-0001:evt-", 0) == 0);
    // 正文整包扣下:excluded.jsonl 不当泄密出口。
    CHECK(episodes[0].episode["messages"].empty());

    const auto report = trajectory::ExportSessionTrainingV1(dir);
    REQUIRE(report.ok());
    const auto export_dir = dir / "exports" / "training-v1";
    for (const char* name : {"success.jsonl", "failure.jsonl", "partial.jsonl", "excluded.jsonl",
                             "manifest.json"}) {
        CHECK(ReadFileText(export_dir / name).find("eyJhbGciOi9ub25lJ9") == std::string::npos);
    }
    // manifest 记排除缘由(§13.1)。
    const auto manifest = nlohmann::json::parse(ReadFileText(export_dir / "manifest.json"));
    CHECK(manifest["counts"]["excluded"] == 1);
    bool reason_counted = false;
    for (auto it = manifest["exclusion_reasons"].begin();
         it != manifest["exclusion_reasons"].end(); ++it) {
        if (it.key().rfind("privacy.secret.", 0) == 0) {
            reason_counted = true;
        }
    }
    CHECK(reason_counted);
}

TEST_CASE("绝对路径/个人路径命中 excluded;URL 与仓库相对路径不误伤") {
    const auto dir = FreshDir("lubancode-p5-paths");
    BuildMainStream(dir, "exact", false, false, false, true,
                    "看看 C:\\Users\\bob\\secret.txt 与文档 /etc/hosts");
    const auto episodes = trajectory::BuildSessionTrainingEpisodes(dir);
    REQUIRE(episodes.size() == 1);
    CHECK(episodes[0].route == trajectory::EpisodeRoute::Excluded);
    std::set<std::string> codes;
    for (const auto& finding : episodes[0].episode["privacy_findings"]) {
        codes.insert(finding["code"].get<std::string>());
    }
    CHECK(codes.count("privacy.personal_path") == 1);
    CHECK(codes.count("privacy.absolute_path") == 1);

    // 反例:URL 与相对路径不当路径泄漏。
    const auto clean = FreshDir("lubancode-p5-paths-clean");
    BuildMainStream(clean, "exact", false, false, false, true,
                    "照 https://example.com/docs 与 src/main.cpp 改");
    const auto clean_episodes = trajectory::BuildSessionTrainingEpisodes(clean);
    REQUIRE(clean_episodes.size() == 1);
    CHECK_FALSE(clean_episodes[0].episode.contains("privacy_findings"));
    CHECK(clean_episodes[0].episode["quality"]["privacy"] == "passed");
}

// ---------------------------------------------------------------------------
// thinking/宿主旁路/分账
// ---------------------------------------------------------------------------

TEST_CASE("thinking 默认剔除;开 include_reasoning 才带,且 config hash 变") {
    const auto make = [](const std::filesystem::path& dir) {
        auto recorder = trajectory::TrajectoryRecorder::Start(
            dir / "main.jsonl", dir / "artifacts", MainScope(),
            [] {
                trajectory::RecorderOptions options;
                options.event_schema_version = 2;
                return options;
            }());
        recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}}, Durability::PowerLoss);
        trajectory::BlobStore blobs(dir / "artifacts");
        const auto snapshot = blobs.Store("{}", "application/json", Durability::PowerLoss);
        trajectory::RecordRequest env;
        env.kind = EventKind::RunEnvironmentCaptured;
        env.scope = recorder->base_scope();
        env.payload = nlohmann::json{{"snapshot_ref", snapshot->ToJson()},
                                     {"replay_level", "exact"},
                                     {"gaps", nlohmann::json::array()}};
        recorder->Record(std::move(env), Durability::ProcessCrash);
        auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, MainScope(),
                                                             TrajectoryTurnBridge::Identity{});
        bridge->BeginTurn("turn-1", "external_user");
        bridge->RecordInput(UserMessage("想想"));
        const std::string request_id =
            bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
        bridge->OnRequestSent(request_id);
        REQUIRE(bridge->OnOutputCompleted(request_id,
                                          AssistantTextWithThinking("先看文件再动手。", "我看过了。"),
                                          "end_turn", "resp-1"));
        bridge->EndTurn(true, false, "done");
        recorder->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss);
    };
    const auto dir = FreshDir("lubancode-p5-thinking");
    make(dir);
    const auto plain = trajectory::BuildSessionTrainingEpisodes(dir);
    REQUIRE(plain.size() == 1);
    bool saw_thinking = false;
    for (const auto& message : plain[0].episode["messages"]) {
        for (const auto& block : message["content"]) {
            saw_thinking = saw_thinking || block["type"] == "thinking";
        }
    }
    CHECK_FALSE(saw_thinking);

    trajectory::TrainingExportOptions with_reasoning;
    with_reasoning.include_reasoning = true;
    const auto rich = trajectory::BuildSessionTrainingEpisodes(dir, with_reasoning);
    REQUIRE(rich.size() == 1);
    saw_thinking = false;
    for (const auto& message : rich[0].episode["messages"]) {
        for (const auto& block : message["content"]) {
            saw_thinking = saw_thinking || block["type"] == "thinking";
        }
    }
    CHECK(saw_thinking);
    // 配置进指纹(§11.7):同账不同配置,fingerprint 必不同。
    CHECK(rich[0].fingerprint != plain[0].fingerprint);
    CHECK(trajectory::ComputeExporterConfigHash(with_reasoning) !=
          trajectory::ComputeExporterConfigHash(trajectory::TrainingExportOptions{}));
}

TEST_CASE("宿主旁路小请求(compact 起名一类)不当 episode 编") {
    const auto dir = FreshDir("lubancode-p5-bypass");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir / "main.jsonl", dir / "artifacts", MainScope(),
        [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            return options;
        }());
    recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}}, Durability::PowerLoss);
    auto bypass = std::make_unique<TrajectoryBypassBridge>(*recorder, MainScope(),
                                                           TrajectoryTurnBridge::Identity{});
    agent::RequestPreparedContext ctx;
    ctx.purpose = accounting::RequestPurpose::CompactMap;
    api::Request request;
    request.model = "demo-large";
    request.messages.push_back(UserMessage("旧账:修了个 bug,跑了三遍测试。"));
    const std::string request_id = bypass->OnRequestPrepared(request, ctx);
    REQUIRE_FALSE(request_id.empty());
    bypass->OnRequestSent(request_id);
    REQUIRE(bypass->OnOutputCompleted(request_id, UserMessage("摘要:修了个 bug"), "end_turn", "resp-x"));
    recorder->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss);

    const auto episodes = trajectory::BuildSessionTrainingEpisodes(dir);
    CHECK(episodes.empty());
}

TEST_CASE("子代理分账:child episode 只出现一次,main 只带边界引用") {
    const auto root = FreshDir("lubancode-p5-subagent");
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.lubancode_version = "test";
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

    // 子账:一轮问答,正常收口。
    auto child = ledger->SpawnSubagent("toolu-1", "读文件并数行数");
    REQUIRE(child.has_value());
    auto& child_bridge = (*child)->turn_bridge();
    child_bridge.BeginTurn("turn-1", "external_user");
    child_bridge.RecordInput(UserMessage("读文件并数行数"));
    const std::string child_request =
        child_bridge.OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    child_bridge.OnRequestSent(child_request);
    REQUIRE(child_bridge.OnOutputCompleted(child_request, UserMessage("报告:42 行"), "end_turn", "resp-c"));
    const std::string verification = child_bridge.BeginVerification("line_count", "out/count.txt", "wc");
    child_bridge.FinishVerification(verification, true, nlohmann::json{{"lines", 42}});
    child_bridge.EndTurn(true, false, "done");
    const std::string terminal_hash = (*child)->Finish(true, "done");
    CHECK_FALSE(terminal_hash.empty());

    // 父账:agent 工具边界引用子账,不内联正文。
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

    const auto episodes = trajectory::BuildSessionTrainingEpisodes(ledger->session_dir());
    REQUIRE(episodes.size() == 2);
    // main 的 episode 与 child 的 episode 各一枚;child 正文只出现一次。
    std::string child_episode_text;
    const std::string marker = "读文件并数行数";
    int marker_count = 0;
    for (const auto& episode : episodes) {
        const std::string text = episode.episode.dump();
        if (text.find(marker) != std::string::npos) {
            child_episode_text = text;
            ++marker_count;
        }
    }
    CHECK(marker_count == 1);
    CHECK_FALSE(child_episode_text.empty());
    // main episode 带边界引用(child_run_id + 终态 hash),不带子输入正文。
    for (const auto& episode : episodes) {
        if (episode.episode["source"]["run_id"].get<std::string>() !=
            (*child)->run_id()) {
            const std::string text = episode.episode.dump();
            CHECK(text.find(marker) == std::string::npos);
            REQUIRE(episode.episode["steps"].size() == 1);
            CHECK(episode.episode["steps"][0]["child_run_id"] == (*child)->run_id());
            CHECK(episode.episode["steps"][0]["child_terminal_event_hash"] == terminal_hash);
        }
    }
    // 非本工作区 git 仓(临时目录):ledger 环境快照如实降档,两枚都进不了
    // 训练集——replay 档过滤在 child 身上同样生效。
    CHECK(episodes[0].route == trajectory::EpisodeRoute::Excluded);
    CHECK(episodes[1].route == trajectory::EpisodeRoute::Excluded);
}

// ---------------------------------------------------------------------------
// 完整性与守门
// ---------------------------------------------------------------------------

TEST_CASE("尾行截断:整 stream 明报 excluded/structure.truncated_tail,不折正文") {
    const auto dir = FreshDir("lubancode-p5-truncated");
    BuildMainStream(dir, "exact", true, true, false, true);
    const auto stream = dir / "main.jsonl";
    const auto lines = trajectory::ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    // fold 认的截断形:末行完整、只缺收尾换行(§16.3 的可恢复缺口)。
    std::string rebuilt;
    for (std::size_t i = 0; i + 1 < lines->size(); ++i) {
        rebuilt += (*lines)[i] + "\n";
    }
    rebuilt += (*lines)[lines->size() - 1];  // 无换行
    std::ofstream out(stream, std::ios::binary | std::ios::trunc);
    out << rebuilt;
    out.close();

    const auto episodes = trajectory::BuildSessionTrainingEpisodes(dir);
    REQUIRE(episodes.size() == 1);
    CHECK(episodes[0].route == trajectory::EpisodeRoute::Excluded);
    CHECK(episodes[0].episode["quality"]["structure"] == "truncated_tail");
    CHECK(std::find(episodes[0].reasons.begin(), episodes[0].reasons.end(),
                    "structure.truncated_tail") != episodes[0].reasons.end());
    // §16.3:已验证前缀照折(正文在),只是进不了 success。
    CHECK_FALSE(episodes[0].episode["messages"].empty());
}

TEST_CASE("blob 缺失不进 success;超限 blob 不内联") {
    const auto dir = FreshDir("lubancode-p5-blobmissing");
    BuildMainStream(dir, "exact", true, true, false, true);
    // 删掉 artifacts 下全部 blob(超限正文/快照都指着它们)。
    std::error_code ec;
    std::filesystem::remove_all(dir / "artifacts", ec);
    std::filesystem::create_directories(dir / "artifacts", ec);
    // 普通内联正文不靠 blob,episode 仍完整;但环境快照 blob 没了——档位
    // 仍以 Journal 记录为准,成功门只对 episode 自身正文验 blob。此例正文
    // 全内联,行为应是 success 不受影响。
    const auto episodes = trajectory::BuildSessionTrainingEpisodes(dir);
    REQUIRE(episodes.size() == 1);
    CHECK(episodes[0].route == trajectory::EpisodeRoute::Success);

    // 真正的缺正文:把 tool result 换成超限正文(offload 成 blob)再删 blob。
    const auto dir2 = FreshDir("lubancode-p5-blobmissing2");
    auto recorder = trajectory::TrajectoryRecorder::Start(
        dir2 / "main.jsonl", dir2 / "artifacts", MainScope(),
        [] {
            trajectory::RecorderOptions options;
            options.event_schema_version = 2;
            options.blobs.inline_limit = 32;  // 32 字节就 offload
            return options;
        }());
    recorder->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}}, Durability::PowerLoss);
    trajectory::BlobStore blobs(dir2 / "artifacts");
    const auto snapshot = blobs.Store("{}", "application/json", Durability::PowerLoss);
    trajectory::RecordRequest env;
    env.kind = EventKind::RunEnvironmentCaptured;
    env.scope = recorder->base_scope();
    env.payload = nlohmann::json{{"snapshot_ref", snapshot->ToJson()},
                                 {"replay_level", "exact"},
                                 {"gaps", nlohmann::json::array()}};
    recorder->Record(std::move(env), Durability::ProcessCrash);
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, MainScope(),
                                                         TrajectoryTurnBridge::Identity{});
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage(std::string(200, 'x')));  // 超限输入正文 -> blob
    const std::string request_id =
        bridge->OnRequestPrepared(api::Request{}, agent::RequestPreparedContext{});
    bridge->OnRequestSent(request_id);
    REQUIRE(bridge->OnOutputCompleted(request_id, UserMessage("收到。"), "end_turn", "resp-1"));
    const std::string verification = bridge->BeginVerification("t", "s", "p");
    bridge->FinishVerification(verification, true, nlohmann::json{{"ok", true}});
    bridge->EndTurn(true, false, "done");
    recorder->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss);
    // 删输入正文 blob,只留环境快照。
    for (const auto& entry : std::filesystem::directory_iterator(dir2 / "artifacts" / "sha256")) {
        for (const auto& blob : std::filesystem::directory_iterator(entry)) {
            const std::string name = blob.path().filename().string();
            if (name != snapshot->sha256) {
                std::filesystem::remove(blob, ec);
            }
        }
    }
    const auto missing = trajectory::BuildSessionTrainingEpisodes(dir2);
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].route == trajectory::EpisodeRoute::Excluded);
    CHECK(missing[0].episode["quality"]["structure"] == "blob_missing");
}

TEST_CASE("flag 依赖与空账:没账报错不造假;磁盘门不足一个字节不写") {
    // 目录不存在。
    {
        const auto report = trajectory::ExportSessionTrainingV1(FreshDir("lubancode-p5-none") / "nope");
        CHECK(report.error_code == "export.no_session_dir");
    }
    // 目录在,没开过 trajectory:没有 JSONL。
    {
        const auto dir = FreshDir("lubancode-p5-empty");
        const auto report = trajectory::ExportSessionTrainingV1(dir);
        CHECK(report.error_code == "export.no_streams");
        CHECK_FALSE(std::filesystem::exists(dir / "exports"));
    }
    // 磁盘门不足(§12.2):不落盘。
    {
        const auto dir = FreshDir("lubancode-p5-disk");
        BuildMainStream(dir, "exact", true, true, false, true);
        trajectory::TrainingExportOptions options;
        options.min_free_bytes = std::numeric_limits<std::uint64_t>::max();
        const auto report = trajectory::ExportSessionTrainingV1(dir, options);
        CHECK(report.error_code == "export.storage_exhausted");
        CHECK_FALSE(std::filesystem::exists(dir / "exports" / "training-v1" / "success.jsonl"));
    }
}

TEST_CASE("export-workspace:逐 session 各导,汇总账齐;没账报空不造假") {
    const auto root = FreshDir("lubancode-p5-workspace");
    const auto sessions = root / "sessions";
    std::filesystem::create_directories(sessions / "20260831-090000-WORK01");
    std::filesystem::create_directories(sessions / "20260831-100000-WORK02");
    BuildMainStream(sessions / "20260831-090000-WORK01", "exact", true, true, false, true);
    BuildMainStream(sessions / "20260831-100000-WORK02", "input_only", false, false, false, true);
    const auto report = trajectory::ExportWorkspaceTrainingV1(root);
    REQUIRE(report.ok());
    CHECK(report.counts.at("success") == 1);
    CHECK(report.counts.at("excluded") == 1);
    CHECK(report.episodes == 2);
    CHECK(std::filesystem::exists(sessions / "20260831-090000-WORK01" / "exports" / "training-v1" /
                                  "manifest.json"));
    CHECK(std::filesystem::exists(sessions / "20260831-100000-WORK02" / "exports" / "training-v1" /
                                  "manifest.json"));

    // sessions/ 在却一场可导的都没有:报空不造假(红线:flag 关的会话没账)。
    const auto empty_root = FreshDir("lubancode-p5-workspace-empty");
    std::filesystem::create_directories(empty_root / "sessions");
    const auto empty = trajectory::ExportWorkspaceTrainingV1(empty_root);
    CHECK(empty.error_code == "export.no_streams");
}
