// 采集器的端到端:临时目录里搭一个迷你 <home>/.lubancode(recordings、
// workflow-runs、sessions),连会话档里的 assistant 思考原文也塞上,钉两件
// 硬事:观察追得到原始账(来源 ID 与文件),全文查无密钥与模型思考原文。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "api/types.hpp"
#include "agent/tool_trace.hpp"
#include "evolution/collector.hpp"
#include "evolution/observation_store.hpp"
#include "memory/project_memory.hpp"
#include "sessions/goal_session.hpp"
#include "sessions/session_store.hpp"
#include "skills/workflow_recorder.hpp"
#include "workflow/journal.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_evolution_collect_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

const char* kFakeToken = "sk-COLLECTFAKE444555";
// 假的模型思考原文(长篇推理)。观察里查不到它——压根不收。
const char* kFakeThinking = "INNER-THINKING-请让我一步步推理这个 provider 绑定问题首先我要……";

// 造一场会话档:meta + 一条带思考原文的 assistant 消息 + goal 事件 + 失败
// 工具栅栏。返回文件路径。
std::string WriteSession(const fs::path& sessions_dir, const std::string& session_id) {
    std::error_code ec;
    fs::create_directories(sessions_dir, ec);
    const fs::path file = sessions_dir / (session_id + ".jsonl");
    std::ofstream out(file, std::ios::binary);

    lubancode::sessions::SessionMeta meta;
    meta.version = 1;
    meta.wire = "anthropic";
    meta.model = "fake-model";
    meta.cwd = "D:/nowhere";
    meta.started_at = "2026-08-28 09:00:00";
    out << lubancode::sessions::SerializeSessionMeta(meta) << "\n";

    // 一条 assistant 消息:thinking 块(模型思考原文,不收)+ 文本块。
    lubancode::api::Message assistant;
    assistant.role = lubancode::api::Role::Assistant;
    lubancode::api::ThinkingBlock thinking;
    thinking.text = kFakeThinking;
    assistant.content.push_back(thinking);
    lubancode::api::TextBlock text;
    text.text = "答话正文(观察也不收消息正文)";
    assistant.content.push_back(text);
    out << lubancode::sessions::SerializeSessionMessage(assistant, "2026-08-28 09:00:01") << "\n";

    // goal 事件:created + iteration + evidence + evaluated(achieved)。
    lubancode::sessions::GoalSessionEvent created;
    created.type = "goal_v1";
    created.event = "created";
    created.goal_id = "goal-1";
    created.revision = 1;
    created.payload["objective"] = "修好绑定,token " + std::string(kFakeToken) + " 不外泄";
    created.timestamp_ms = 1750000000000LL;
    out << lubancode::sessions::SerializeGoalEvent(created, "2026-08-28 09:00:02") << "\n";

    lubancode::sessions::GoalSessionEvent evidence;
    evidence.type = "goal_evidence_v1";
    evidence.event = "observed";
    evidence.goal_id = "goal-1";
    evidence.iteration_id = "goal-1/iter-1";
    evidence.payload["evidence_id"] = "ev-1";
    evidence.payload["kind"] = "command_exit";
    out << lubancode::sessions::SerializeGoalEvent(evidence, "2026-08-28 09:00:03") << "\n";

    lubancode::sessions::GoalSessionEvent evaluated;
    evaluated.type = "goal_evaluation_v1";
    evaluated.event = "evaluated";
    evaluated.goal_id = "goal-1";
    evaluated.iteration_id = "goal-1/iter-1";
    evaluated.revision = 1;
    nlohmann::json evaluation;
    evaluation["id"] = "eval-1";
    evaluation["decision"] = "achieved";
    evaluation["summary"] = "验收全绿";
    evaluated.payload["evaluation"] = evaluation;
    out << lubancode::sessions::SerializeGoalEvent(evaluated, "2026-08-28 09:00:04") << "\n";

    // 失败工具栅栏:scheduled/started/finished 三道,error_code 进账。
    for (const auto kind : {lubancode::agent::ToolTraceEventKind::Scheduled,
                            lubancode::agent::ToolTraceEventKind::ExecutionStarted,
                            lubancode::agent::ToolTraceEventKind::ExecutionFinished}) {
        lubancode::agent::ToolTraceEvent event;
        event.kind = kind;
        event.thread_id = session_id;
        event.turn_id = "t1";
        event.batch_id = "b1";
        event.execution_id = "item-91";
        event.item_id = "item-91";
        event.tool_use_id = "toolu-91";
        event.tool_name = "run_command";
        if (kind == lubancode::agent::ToolTraceEventKind::ExecutionStarted) {
            event.effective_input_sha256 =
                "sha256:" + std::string(64, '0');  // 内容哈希:不进指纹也不进观察
        }
        if (kind == lubancode::agent::ToolTraceEventKind::ExecutionFinished) {
            event.outcome = lubancode::agent::ToolOutcome::ProcessExitNonzero;
            event.error_code = "process.exit_nonzero";
            event.fallback_message = "退出码 1";
        }
        out << lubancode::agent::SerializeToolTraceEvent(event, "2026-08-28 09:00:05") << "\n";
    }
    return file.string();
}

}  // namespace

TEST_CASE("采集:五路合拢,追得到原始账,查无密钥与思考原文") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings_root = home / "recordings";
    const fs::path runs_root = home / "workflow-runs";
    const fs::path sessions_dir = home / "sessions";

    // 一场录制件。
    lubancode::skills::RecordingStartInfo record_info;
    record_info.name = "demo";
    record_info.goal = "示范排查,密钥 " + std::string(kFakeToken) + " 莫泄";
    record_info.variables = {"region"};
    record_info.acceptance = "ctest 绿";
    record_info.cwd = "D:/nowhere";
    auto recorder = lubancode::skills::WorkflowRecorder::Start(recordings_root, record_info);
    REQUIRE(recorder.has_value());
    recorder->RecordToolCall("read_file", nlohmann::json{{"path", "a.cpp"}}, "item-1", "toolu-1");
    REQUIRE(recorder->Stop("全绿").has_value());

    // 一场 workflow run。
    lubancode::workflow::RunJournal::StartInfo start;
    start.run_id = "run-1";
    start.workflow_id = "wf";
    start.workflow_version = "1.0.0";
    start.content_hash = "sha256:" + std::string(64, '0');
    start.cwd = "D:/nowhere";
    start.definition_json = "{}";
    auto journal = lubancode::workflow::RunJournal::Start(runs_root, start);
    REQUIRE(journal.has_value());
    journal->Append(lubancode::workflow::kEventRunStarted, "", 0, nlohmann::json::object());
    journal->Append(lubancode::workflow::kEventNodeStarted, "a", 0, nlohmann::json::object());
    journal->Finish("succeeded", nlohmann::json::object());

    // 一场会话档(goal + 失败工具)。
    const std::string session_file = WriteSession(sessions_dir, "20260828-090000-demo");

    // 一条已接受 memory。
    lubancode::evolution::CollectSources sources;
    sources.recordings_root = recordings_root;
    sources.workflow_runs_root = runs_root;
    sources.sessions_dir = sessions_dir.string();
    lubancode::memory::MemoryEntry fact;
    fact.id = "fact-1";
    fact.kind = lubancode::memory::MemoryKind::Feedback;
    fact.title = "验收先跑 ctest -C Debug";
    fact.file = "fact-1.md";
    fact.status = "active";
    fact.updated_at = "2026-08-28 08:00:00";
    lubancode::evolution::MemoryLayer layer;
    layer.entries = {fact};
    layer.layer_label = "project";
    layer.dir_utf8 = (home / "memory" / "project-x").string();
    sources.memory_layers.push_back(std::move(layer));

    lubancode::evolution::CollectReport report;
    const auto observations = lubancode::evolution::CollectObservations(sources, &report);

    // 五路各出多少:recording 1 + run 1 + goal 1 + tooltrace 1 + memory 1。
    REQUIRE(observations.size() == 5);
    CHECK(report.recordings_scanned == 1);
    CHECK(report.runs_scanned == 1);
    CHECK(report.sessions_scanned == 1);
    CHECK(report.memory_entries == 1);
    CHECK(report.observations == 5);

    std::string all;
    for (const auto& observation : observations) {
        all += lubancode::evolution::SerializeObservation(observation);
    }

    // ---- 密钥与思考原文:全文查无(验收线)----
    CHECK(all.find(kFakeToken) == std::string::npos);
    CHECK(all.find(kFakeThinking) == std::string::npos);
    CHECK(all.find("INNER-THINKING") == std::string::npos);

    // ---- 追得到原始账:来源 ID 与文件都指得回 ----
    bool saw_goal = false;
    bool saw_trace = false;
    bool saw_recording = false;
    bool saw_run = false;
    bool saw_memory = false;
    for (const auto& observation : observations) {
        if (observation.source == lubancode::evolution::ObservationSource::Goal) {
            saw_goal = true;
            CHECK(observation.source_id == "goal-1");
            CHECK(observation.source_ref == session_file);
            CHECK(observation.outcome == lubancode::evolution::ObservationOutcome::Success);
        } else if (observation.source == lubancode::evolution::ObservationSource::ToolTrace) {
            saw_trace = true;
            CHECK(observation.source_id == "item-91");
            CHECK(observation.source_ref == session_file);
            CHECK_FALSE(observation.evidence.empty());
        } else if (observation.source == lubancode::evolution::ObservationSource::Recording) {
            saw_recording = true;
            CHECK(observation.source_ref.find("recordings") != std::string::npos);
            CHECK(observation.evidence.front().ref.find("events.jsonl") != std::string::npos);
        } else if (observation.source == lubancode::evolution::ObservationSource::Run) {
            saw_run = true;
            CHECK(observation.source_id == "run-1");
        } else if (observation.source == lubancode::evolution::ObservationSource::Memory) {
            saw_memory = true;
            CHECK(observation.source_id == "fact-1");
        }
    }
    CHECK(saw_goal);
    CHECK(saw_trace);
    CHECK(saw_recording);
    CHECK(saw_run);
    CHECK(saw_memory);

    // ---- 落账与重采幂等 ----
    lubancode::evolution::ObservationStore store(home / "evolution" / "observations");
    for (const auto& observation : observations) {
        REQUIRE(store.Append(observation).has_value());
    }
    CHECK(store.Load().size() == 5);
    // 再采一回:id 全部命中 DuplicateId,账不翻倍。
    const auto again = lubancode::evolution::CollectObservations(sources, nullptr);
    CHECK(again.size() == 5);
    for (const auto& observation : again) {
        const auto status = store.Append(observation);
        REQUIRE(status.has_value());
        CHECK(*status == lubancode::evolution::ObservationStore::AppendStatus::DuplicateId);
    }
    CHECK(store.Load().size() == 5);
}

TEST_CASE("采集:空根不扫、坏档跳过、上限生效") {
    TempDir temp;
    lubancode::evolution::CollectSources sources;  // 全空:不崩,零观察。
    lubancode::evolution::CollectReport report;
    CHECK(lubancode::evolution::CollectObservations(sources, &report).empty());
    CHECK(report.observations == 0);

    // 会话目录里塞一份坏档(不是 JSONL):跳过计数,不抛。
    const fs::path sessions_dir = temp.Get() / "sessions";
    std::error_code ec;
    fs::create_directories(sessions_dir, ec);
    {
        std::ofstream out(sessions_dir / "20260828-100000-bad.jsonl", std::ios::binary);
        out << "not a session file at all\n";
    }
    sources.sessions_dir = sessions_dir.string();
    sources.max_sessions = 5;
    const auto observations = lubancode::evolution::CollectObservations(sources, &report);
    CHECK(observations.empty());
    CHECK(report.sessions_scanned == 1);
    CHECK(report.sessions_unreadable == 1);

    // 上限:三场录制件,max_recordings=2 只扫两件。
    const fs::path recordings_root = temp.Get() / "recordings";
    for (int i = 0; i < 3; ++i) {
        lubancode::skills::RecordingStartInfo info;
        info.name = "r" + std::to_string(i);
        info.goal = "g";
        info.acceptance = "a";
        info.cwd = "D:/nowhere";
        auto recorder = lubancode::skills::WorkflowRecorder::Start(recordings_root, info);
        REQUIRE(recorder.has_value());
        REQUIRE(recorder->Stop("ok").has_value());
    }
    sources.recordings_root = recordings_root;
    sources.max_recordings = 2;
    const auto capped = lubancode::evolution::CollectObservations(sources, &report);
    CHECK(report.recordings_scanned == 2);
    CHECK(capped.size() == 2);
}
