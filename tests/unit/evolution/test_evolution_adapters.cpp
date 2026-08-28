// 五路只读 adapter 的单测:临时目录造假账本喂进去,钉输入口径、outcome
// 分档、指纹与脱敏。假密钥/假 cookie 全程查无;模型思考原文压根不收。

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "agent/tool_trace.hpp"
#include "evolution/adapters.hpp"
#include "memory/project_memory.hpp"
#include "sessions/goal_session.hpp"
#include "skills/workflow_recorder.hpp"
#include "workflow/journal.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_evolution_adapter_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    const fs::path& Get() const { return dir_; }

private:
    fs::path dir_;
};

const char* kFakeToken = "sk-ADAPTERFAKE000111";
const char* kFakeCookie = "COOKIEFAKE222333";

std::string Dump(const std::vector<lubancode::evolution::EvolutionObservation>& observations) {
    std::string full;
    for (const auto& observation : observations) {
        full += lubancode::evolution::SerializeObservation(observation);
    }
    return full;
}

// 一枚失败工具调用的三道栅栏(scheduled/started/finished)。
lubancode::agent::ToolTraceEvent FailedExecution(const std::string& execution_id,
                                                 const std::string& tool,
                                                 lubancode::agent::ToolOutcome outcome,
                                                 const std::string& error_code) {
    lubancode::agent::ToolTraceEvent event;
    event.kind = lubancode::agent::ToolTraceEventKind::ExecutionFinished;
    event.thread_id = "s1";
    event.turn_id = "t1";
    event.batch_id = "b1";
    event.execution_id = execution_id;
    event.item_id = execution_id;
    event.tool_use_id = "toolu-" + execution_id;
    event.tool_name = tool;
    event.outcome = outcome;
    event.error_code = error_code;
    event.fallback_message = "假失败";
    event.seq = 1;
    event.timestamp_ms = 1750000000000LL;
    return event;
}

}  // namespace

// ---------------------------------------------------------------------------
// recording
// ---------------------------------------------------------------------------

TEST_CASE("adapter.recording:完整录制件出一条观察,验证过即 success") {
    TempDir temp;
    lubancode::skills::RecordingStartInfo info;
    info.name = "provider-audit";
    info.goal = "排查 provider 绑定误判,密钥 " + std::string(kFakeToken) + " 不外泄";
    info.variables = {"provider_id"};
    info.acceptance = "ctest 全绿";
    info.cwd = "D:/nowhere";
    auto recorder = lubancode::skills::WorkflowRecorder::Start(temp.Get(), info);
    REQUIRE(recorder.has_value());
    nlohmann::json input;
    input["path"] = "src/main.cpp";
    input["api_key"] = kFakeToken;  // 录制件入盘前已脱敏;观察侧再守一道
    recorder->RecordToolCall("read_file", input, "item-1", "toolu-1");
    recorder->RecordToolResult("read_file", false, "第一行摘要", "", "", "item-1");
    recorder->RecordToolCall("run_command", nlohmann::json{{"cmd", "ctest"}}, "item-2", "toolu-2");
    const auto stopped = recorder->Stop("ctest 全绿,验证通过");
    REQUIRE(stopped.has_value());

    const auto statuses = lubancode::skills::ListRecordings(temp.Get());
    REQUIRE(statuses.size() == 1);
    lubancode::evolution::RecordingMaterial material;
    material.status = statuses.front();
    material.events = lubancode::skills::ReadRecordingEvents(statuses.front().dir);
    const auto observations = lubancode::evolution::ObservationsFromRecording(material);

    REQUIRE(observations.size() == 1);
    const auto& observation = observations.front();
    CHECK(observation.source == lubancode::evolution::ObservationSource::Recording);
    CHECK(observation.source_id == statuses.front().id);
    CHECK(observation.outcome == lubancode::evolution::ObservationOutcome::Success);
    CHECK(observation.details.at("tool_call_count").get<int>() == 2);
    CHECK(observation.details.at("tools") ==
          std::vector<std::string>({"read_file", "run_command"}));
    CHECK(observation.details.at("variables") == std::vector<std::string>{"provider_id"});
    CHECK(observation.fingerprint.rfind("fp-", 0) == 0);
    CHECK(observation.id.rfind("obs-", 0) == 0);
    REQUIRE(observation.evidence.size() == 1);
    CHECK(observation.evidence[0].ref.find("events.jsonl") != std::string::npos);

    // 脱敏:观察全文查无假密钥。
    CHECK(Dump(observations).find(kFakeToken) == std::string::npos);
}

TEST_CASE("adapter.recording:半截录制件不收;无验证不是 success") {
    TempDir temp;
    // 两场录制件名字分开:录制件 id 以秒级时间戳起头,同名同秒会撞目录。
    lubancode::skills::RecordingStartInfo info;
    info.name = "half-a";
    info.goal = "没停录的示范";
    info.acceptance = "x";
    info.cwd = "D:/nowhere";
    auto recorder = lubancode::skills::WorkflowRecorder::Start(temp.Get(), info);
    REQUIRE(recorder.has_value());
    recorder->RecordToolCall("read_file", nlohmann::json{{"path", "a"}}, "item-1", "toolu-1");
    // 不 Stop:events.jsonl 里没有 record_stop。
    const auto statuses = lubancode::skills::ListRecordings(temp.Get());
    REQUIRE(statuses.size() == 1);
    lubancode::evolution::RecordingMaterial material;
    material.status = statuses.front();
    material.events = lubancode::skills::ReadRecordingEvents(statuses.front().dir);
    CHECK(lubancode::evolution::ObservationsFromRecording(material).empty());

    // 停了但没给验证口述:outcome 落 unknown(没有产物证据的成功不收)。
    info.name = "half-b";
    auto second = lubancode::skills::WorkflowRecorder::Start(temp.Get(), info);
    REQUIRE(second.has_value());
    REQUIRE(second->Stop("").has_value());
    const auto statuses2 = lubancode::skills::ListRecordings(temp.Get());
    // ListRecordings 按 id 倒序,最新在前。
    lubancode::evolution::RecordingMaterial material2;
    material2.status = statuses2.front();
    material2.events = lubancode::skills::ReadRecordingEvents(statuses2.front().dir);
    const auto observations = lubancode::evolution::ObservationsFromRecording(material2);
    REQUIRE(observations.size() == 1);
    CHECK(observations.front().outcome == lubancode::evolution::ObservationOutcome::Unknown);
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

TEST_CASE("adapter.run:终态分档与节点序列进指纹") {
    TempDir temp;
    lubancode::workflow::RunJournal::StartInfo start;
    start.run_id = "run-1";
    start.workflow_id = "audit-flow";
    start.workflow_version = "1.2.0";
    start.content_hash = "sha256:abcd";
    start.cwd = "D:/nowhere";
    start.definition_json = "{}";
    auto journal = lubancode::workflow::RunJournal::Start(temp.Get(), start);
    REQUIRE(journal.has_value());
    journal->Append(lubancode::workflow::kEventRunStarted, "", 0, nlohmann::json::object());
    journal->Append(lubancode::workflow::kEventNodeStarted, "collect", 0, nlohmann::json::object());
    journal->Append(lubancode::workflow::kEventNodeRetrying, "collect", 1, nlohmann::json::object());
    journal->Append(lubancode::workflow::kEventNodeStarted, "collect", 1, nlohmann::json::object());
    journal->Append(lubancode::workflow::kEventNodeStarted, "report", 0, nlohmann::json::object());
    journal->Append(lubancode::workflow::kEventCheckpointSaved, "", 0, nlohmann::json::object());
    journal->Finish("succeeded", nlohmann::json::object());

    const auto runs = lubancode::workflow::ListRuns(temp.Get());
    REQUIRE(runs.size() == 1);
    const auto events = lubancode::workflow::ReadJournalEvents(runs.front().dir);
    const auto observations = lubancode::evolution::ObservationsFromRun(runs.front(), events);
    REQUIRE(observations.size() == 1);
    const auto& observation = observations.front();
    CHECK(observation.source == lubancode::evolution::ObservationSource::Run);
    CHECK(observation.source_id == "run-1");
    CHECK(observation.outcome == lubancode::evolution::ObservationOutcome::Success);
    CHECK(observation.details.at("final_state").get<std::string>() == "succeeded");
    // 连续同名折叠:collect,collect(重试),report -> collect,report。
    CHECK(observation.details.at("nodes") == std::vector<std::string>({"collect", "report"}));
    CHECK(observation.details.at("retry_count").get<int>() == 1);
    CHECK(observation.details.at("checkpoint_count").get<int>() == 1);

    // 同 workflow 同形状的另一场 run(不同 run_id、不同时间):同指纹——
    // "哪一场"不进指纹,"哪一类"进。
    lubancode::workflow::RunJournal::StartInfo second_start;
    second_start.run_id = "run-2";
    second_start.workflow_id = "audit-flow";
    second_start.workflow_version = "1.2.0";
    second_start.content_hash = "sha256:abcd";
    second_start.cwd = "D:/elsewhere";
    second_start.definition_json = "{}";
    auto journal2 = lubancode::workflow::RunJournal::Start(temp.Get(), second_start);
    REQUIRE(journal2.has_value());
    journal2->Append(lubancode::workflow::kEventRunStarted, "", 0, nlohmann::json::object());
    journal2->Append(lubancode::workflow::kEventNodeStarted, "collect", 0, nlohmann::json::object());
    journal2->Append(lubancode::workflow::kEventNodeStarted, "report", 0, nlohmann::json::object());
    journal2->Finish("failed", nlohmann::json::object());
    const auto runs2 = lubancode::workflow::ListRuns(temp.Get());
    REQUIRE(runs2.size() == 2);
    const auto events2 = lubancode::workflow::ReadJournalEvents(runs2.front().dir);
    const auto observations2 = lubancode::evolution::ObservationsFromRun(runs2.front(), events2);
    REQUIRE(observations2.size() == 1);
    CHECK(observations2.front().outcome == lubancode::evolution::ObservationOutcome::Failure);
    // 终态不同 → 不同指纹(失败路是形状的一部分)。
    CHECK(observations2.front().fingerprint != observation.fingerprint);
    CHECK(observations2.front().id != observation.id);
}

TEST_CASE("adapter.run:未收场的 run 落 unknown,节点进账") {
    lubancode::workflow::RunStatus run;  // 不落盘,直接喂结构
    run.run_id = "run-open";
    run.workflow_id = "w";
    run.workflow_version = "0.1.0";
    run.final_state = "";
    const auto observations = lubancode::evolution::ObservationsFromRun(run, {});
    REQUIRE(observations.size() == 1);
    CHECK(observations.front().outcome == lubancode::evolution::ObservationOutcome::Unknown);

    lubancode::workflow::RunStatus cancelled = run;
    cancelled.run_id = "run-cancel";
    cancelled.final_state = "interrupted";
    const auto observations2 = lubancode::evolution::ObservationsFromRun(cancelled, {});
    REQUIRE(observations2.size() == 1);
    CHECK(observations2.front().outcome == lubancode::evolution::ObservationOutcome::Partial);
}

// ---------------------------------------------------------------------------
// goal
// ---------------------------------------------------------------------------

TEST_CASE("adapter.goal:objective/iteration/evidence/判词逐项进账") {
    std::vector<lubancode::sessions::GoalSessionEvent> events;
    lubancode::sessions::GoalSessionEvent created;
    created.type = "goal_v1";
    created.event = "created";
    created.goal_id = "goal-3";
    created.revision = 1;
    created.payload["objective"] = "修好 provider 绑定,密钥 " + std::string(kFakeToken) + " 不外泄";
    created.timestamp_ms = 1750000000000LL;
    events.push_back(created);

    lubancode::sessions::GoalSessionEvent scheduled;
    scheduled.type = "goal_iteration_v1";
    scheduled.event = "scheduled";
    scheduled.goal_id = "goal-3";
    scheduled.iteration_id = "goal-3/iter-1";
    scheduled.revision = 1;
    events.push_back(scheduled);

    lubancode::sessions::GoalSessionEvent evidence;
    evidence.type = "goal_evidence_v1";
    evidence.event = "observed";
    evidence.goal_id = "goal-3";
    evidence.iteration_id = "goal-3/iter-1";
    evidence.payload["evidence_id"] = "ev-1";
    evidence.payload["kind"] = "command_exit";
    evidence.timestamp_ms = 1750000001000LL;
    events.push_back(evidence);

    lubancode::sessions::GoalSessionEvent evaluated;
    evaluated.type = "goal_evaluation_v1";
    evaluated.event = "evaluated";
    evaluated.goal_id = "goal-3";
    evaluated.iteration_id = "goal-3/iter-1";
    evaluated.revision = 1;
    nlohmann::json evaluation;
    evaluation["id"] = "eval-1";
    evaluation["decision"] = "achieved";
    evaluation["summary"] = "验收全过";
    evaluated.payload["evaluation"] = evaluation;
    evaluated.timestamp_ms = 1750000002000LL;
    events.push_back(evaluated);

    const auto observations =
        lubancode::evolution::ObservationsFromGoalEvents("s1.jsonl", events);
    REQUIRE(observations.size() == 1);
    const auto& observation = observations.front();
    CHECK(observation.source == lubancode::evolution::ObservationSource::Goal);
    CHECK(observation.source_id == "goal-3");
    CHECK(observation.outcome == lubancode::evolution::ObservationOutcome::Success);
    CHECK(observation.details.at("iteration_count").get<int>() == 1);
    CHECK(observation.details.at("evidence_count").get<int>() == 1);
    CHECK(observation.details.at("last_decision").get<std::string>() == "achieved");
    CHECK(observation.details.at("verdict").get<std::string>() == "验收全过");
    CHECK(observation.created_at == lubancode::evolution::FormatEpochMsLocal(1750000002000LL));
    CHECK(Dump(observations).find(kFakeToken) == std::string::npos);

    // 判词分档:blocked → failure;needs_user/continue → partial;没判词 → unknown。
    std::vector<lubancode::sessions::GoalSessionEvent> blocked_events = events;
    blocked_events.back().payload["evaluation"]["decision"] = "blocked";
    blocked_events.back().payload["evaluation"]["summary"] = "碰墙";
    const auto blocked =
        lubancode::evolution::ObservationsFromGoalEvents("s1.jsonl", blocked_events);
    REQUIRE(blocked.size() == 1);
    CHECK(blocked.front().outcome == lubancode::evolution::ObservationOutcome::Failure);

    std::vector<lubancode::sessions::GoalSessionEvent> open_events = {created, scheduled};
    const auto open = lubancode::evolution::ObservationsFromGoalEvents("s1.jsonl", open_events);
    REQUIRE(open.size() == 1);
    CHECK(open.front().outcome == lubancode::evolution::ObservationOutcome::Unknown);

    // 同 objective 不同 goal id:同指纹("哪一只"不进指纹)。
    std::vector<lubancode::sessions::GoalSessionEvent> other_goal = events;
    for (auto& event : other_goal) {
        event.goal_id = "goal-9";
    }
    const auto other = lubancode::evolution::ObservationsFromGoalEvents("s2.jsonl", other_goal);
    REQUIRE(other.size() == 1);
    CHECK(other.front().fingerprint == observation.fingerprint);
    CHECK(other.front().id != observation.id);
}

// ---------------------------------------------------------------------------
// tooltrace
// ---------------------------------------------------------------------------

TEST_CASE("adapter.tooltrace:只收明确失败,输入哈希与取消不进") {
    lubancode::agent::ToolExecutionLedger ledger;
    ledger.Fold(FailedExecution("item-1", "run_command",
                                lubancode::agent::ToolOutcome::ProcessExitNonzero,
                                "process.exit_nonzero"));
    lubancode::agent::ToolTraceEvent ok = FailedExecution(
        "item-2", "read_file", lubancode::agent::ToolOutcome::Succeeded, "");
    ledger.Fold(ok);
    lubancode::agent::ToolTraceEvent cancelled = FailedExecution(
        "item-3", "web_fetch", lubancode::agent::ToolOutcome::CancelledDuringRun, "");
    ledger.Fold(cancelled);
    lubancode::agent::ToolTraceEvent denied = FailedExecution(
        "item-4", "run_command", lubancode::agent::ToolOutcome::PermissionDeclined,
        "permission.declined");
    ledger.Fold(denied);

    const auto observations =
        lubancode::evolution::ObservationsFromToolTrace("s1.jsonl", ledger);
    REQUIRE(observations.size() == 2);  // exit_nonzero + permission.declined
    CHECK(observations[0].source_id == "item-1");
    CHECK(observations[0].outcome == lubancode::evolution::ObservationOutcome::Failure);
    CHECK(observations[0].details.at("error_code").get<std::string>() ==
          "process.exit_nonzero");
    CHECK(observations[1].source_id == "item-4");

    // 同工具同错误码的另一枚 execution:同指纹(同类失败聚合)。
    lubancode::agent::ToolExecutionLedger ledger2;
    ledger2.Fold(FailedExecution("item-77", "run_command",
                                 lubancode::agent::ToolOutcome::ProcessExitNonzero,
                                 "process.exit_nonzero"));
    const auto same_kind = lubancode::evolution::ObservationsFromToolTrace("s2.jsonl", ledger2);
    REQUIRE(same_kind.size() == 1);
    CHECK(same_kind.front().fingerprint == observations[0].fingerprint);
    CHECK(same_kind.front().id != observations[0].id);

    // effective_input_sha256 不进指纹(内容哈希进指纹会让同类失配):
    // 不同 sha、同工具同错误码 → 指纹不变(上面 ledger2 没设 sha,已经同)。
    // 这里反向钉:同 execution 不同 outcome 文本(error_code 空)→ 靠 outcome 分。
    lubancode::agent::ToolExecutionLedger ledger3;
    lubancode::agent::ToolTraceEvent no_code =
        FailedExecution("item-8", "run_command",
                        lubancode::agent::ToolOutcome::TimedOut, "");
    ledger3.Fold(no_code);
    const auto other_kind = lubancode::evolution::ObservationsFromToolTrace("s3.jsonl", ledger3);
    REQUIRE(other_kind.size() == 1);
    CHECK(other_kind.front().fingerprint != observations[0].fingerprint);
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

TEST_CASE("adapter.memory:只收 active,kind+标题进指纹") {
    std::vector<lubancode::memory::MemoryEntry> entries;
    lubancode::memory::MemoryEntry fact;
    fact.id = "fact-1";
    fact.kind = lubancode::memory::MemoryKind::Fact;
    fact.title = "这个项目只用 uv";
    fact.summary = "包管理只用 uv,cookie=" + std::string(kFakeCookie) + " 不入库";
    fact.file = "fact-1.md";
    fact.status = "active";
    fact.updated_at = "2026-08-28 10:00:00";
    fact.confidence = "user-stated";
    entries.push_back(fact);

    lubancode::memory::MemoryEntry archived = fact;
    archived.id = "fact-2";
    archived.status = "archived";
    entries.push_back(archived);

    lubancode::memory::MemoryEntry feedback = fact;
    feedback.id = "fb-1";
    feedback.kind = lubancode::memory::MemoryKind::Feedback;
    feedback.title = "每笔合并进 main 即 patch+1";
    feedback.status = "active";
    entries.push_back(feedback);

    const auto observations =
        lubancode::evolution::ObservationsFromMemory(entries, "project", "D:/mem");
    REQUIRE(observations.size() == 2);  // archived 不收
    CHECK(observations[0].source == lubancode::evolution::ObservationSource::Memory);
    CHECK(observations[0].source_id == "fact-1");
    CHECK(observations[0].outcome == lubancode::evolution::ObservationOutcome::Unknown);
    CHECK(observations[1].source_id == "fb-1");
    CHECK(observations[0].fingerprint != observations[1].fingerprint);
    REQUIRE(observations[0].evidence.size() == 1);
    CHECK(observations[0].evidence[0].ref == "D:/mem/fact-1.md");
    CHECK(Dump(observations).find(kFakeCookie) == std::string::npos);
}
