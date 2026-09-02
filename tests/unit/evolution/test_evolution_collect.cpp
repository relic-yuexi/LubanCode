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

// (P0-6:旧会话档的 WriteSession 造档函数已删——采集器不再扫旧档,
// goal/trace 观察的输入接 trajectory 新账属自进化单后续波次。)

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

    // (P0-6:会话档路已删——goal/trace 的观察输入接 trajectory 新账属
    // 自进化单后续波次。)
    const std::string session_file;

    // 一条已接受 memory。
    lubancode::evolution::CollectSources sources;
    sources.recordings_root = recordings_root;
    sources.workflow_runs_root = runs_root;
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

    // 四路各出多少:recording 1 + run 1 + memory 1(P0-6 起旧会话档路删)。
    REQUIRE(observations.size() == 3);
    CHECK(report.recordings_scanned == 1);
    CHECK(report.runs_scanned == 1);
    CHECK(report.memory_entries == 1);
    CHECK(report.observations == 3);

    std::string all;
    for (const auto& observation : observations) {
        all += lubancode::evolution::SerializeObservation(observation);
    }

    // ---- 密钥与思考原文:全文查无(验收线)----
    CHECK(all.find(kFakeToken) == std::string::npos);
    CHECK(all.find(kFakeThinking) == std::string::npos);
    CHECK(all.find("INNER-THINKING") == std::string::npos);
    (void)0;

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
    CHECK_FALSE(saw_goal);   // P0-6:旧会话档路删,goal 观察暂缺
    CHECK_FALSE(saw_trace);  // 同上,tooltrace 观察暂缺
    CHECK(saw_recording);
    CHECK(saw_run);
    CHECK(saw_memory);

    // ---- 落账与重采幂等 ----
    lubancode::evolution::ObservationStore store(home / "evolution" / "observations");
    for (const auto& observation : observations) {
        REQUIRE(store.Append(observation).has_value());
    }
    CHECK(store.Load().size() == 3);
    // 再采一回:id 全部命中 DuplicateId,账不翻倍。
    const auto again = lubancode::evolution::CollectObservations(sources, nullptr);
    CHECK(again.size() == 3);
    for (const auto& observation : again) {
        const auto status = store.Append(observation);
        REQUIRE(status.has_value());
        CHECK(*status == lubancode::evolution::ObservationStore::AppendStatus::DuplicateId);
    }
    CHECK(store.Load().size() == 3);
}

TEST_CASE("采集:空根不扫、坏档跳过、上限生效") {
    TempDir temp;
    lubancode::evolution::CollectSources sources;  // 全空:不崩,零观察。
    lubancode::evolution::CollectReport report;
    CHECK(lubancode::evolution::CollectObservations(sources, &report).empty());
    CHECK(report.observations == 0);

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
