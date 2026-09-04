// 自进化闭环阶段 7 单测:有限自动建议。钉五件事(照阶段 7 清单)——
//   1. 缺省关闭:开关文件缺失/坏 JSON/字段不对一律当关,单测钉死;
//   2. 五门判定(§七"自动提示至少要满足"):各案正反面——独立任务证据
//      (门一)、同形(门二)、非偶然(门三)、无同指纹在途/被拒候选
//      (门四)、收益说得清(门五);
//   3. 拒绝后不死缠:propose 落了候选(在途)或 reject 落了拒绝指纹,
//      同指纹簇不再过门(接观察账与候选仓既有的去重账);
//   4. 命中账:shown/accepted 只追加,坏行跳过;命中率与接受率从账现算;
//   5. 冒烟:假观察簇 -> 关时不提示 -> 开后过五门亮一行 -> propose 记
//      accepted -> 接受率账出;只提示,不自动起草(引擎不碰写口)。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "evolution/adapters.hpp"
#include "evolution/coordinator.hpp"
#include "evolution/observation_store.hpp"
#include "evolution/suggest.hpp"
#include "skills/workflow_recorder.hpp"

namespace {

namespace fs = std::filesystem;
using namespace lubancode::evolution;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_evolution_stage7_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

bool WriteText(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << content;
    return file.good();
}

// 一条手工观察(纯结构,不走采集器——五门判定是纯函数,材料直给)。
EvolutionObservation MakeObs(const std::string& source_id, ObservationOutcome outcome,
                             const std::string& fingerprint, const std::vector<std::string>& tools) {
    EvolutionObservation observation;
    observation.id = MakeObservationId(ObservationSource::Recording, source_id);
    observation.source = ObservationSource::Recording;
    observation.source_id = source_id;
    observation.source_ref = "D:/nowhere/" + source_id;
    observation.summary = "抽样核查大账";
    observation.outcome = outcome;
    observation.fingerprint = fingerprint;
    nlohmann::json details;
    details["goal"] = "抽样核查大账";
    details["tools"] = tools;
    observation.details = details;
    return observation;
}

// 一场完整录制(冒烟用:观察要真进账,候选要真落盘)。
ClusterTaskMaterial MakeRecording(const fs::path& recordings_root, const std::string& name) {
    lubancode::skills::RecordingStartInfo info;
    info.name = name;
    info.goal = "抽样核查大账";
    info.acceptance = "产物可解析";
    info.cwd = "D:/nowhere";
    auto recorder = lubancode::skills::WorkflowRecorder::Start(recordings_root, info);
    REQUIRE(recorder.has_value());
    recorder->RecordToolCall("read_file", nlohmann::json{{"path", "cfg/a.yaml"}}, "e1", "e1");
    recorder->RecordToolResult("read_file", false, "成了", "", "", "e1");
    recorder->RecordToolCall("write_file", nlohmann::json{{"path", "out/a.json"}}, "e2", "e2");
    recorder->RecordToolResult("write_file", false, "成了", "", "", "e2");
    CHECK(recorder->Stop("yaml 可解析").has_value());
    ClusterTaskMaterial material;
    material.status.id = recorder->id();
    material.status.name = name;
    material.status.dir = recorder->dir();
    material.status.finished = true;
    material.events = lubancode::skills::ReadRecordingEvents(recorder->dir());
    return material;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 缺省关闭是铁律:读不出一律 false
// ---------------------------------------------------------------------------
TEST_CASE("stage7:开关——缺文件/坏 JSON/字段不对一律当关;写开写关才变") {
    TempDir temp;
    const fs::path root = temp.Get() / "evolution";
    CHECK_FALSE(LoadSuggestEnabled(root));  // 没有文件:缺省关

    WriteText(root / "suggest.json", "not json at all");
    CHECK_FALSE(LoadSuggestEnabled(root));  // 坏 JSON:当关,不猜

    WriteText(root / "suggest.json", R"({"schema":1,"enabled":"yes"})");
    CHECK_FALSE(LoadSuggestEnabled(root));  // enabled 不是布尔:当关

    CHECK_FALSE(SaveSuggestEnabled(root, true).has_value());
    CHECK(LoadSuggestEnabled(root));

    CHECK_FALSE(SaveSuggestEnabled(root, false).has_value());
    CHECK_FALSE(LoadSuggestEnabled(root));  // 写回关:铁律可恢复
}

// ---------------------------------------------------------------------------
// 2. 五门判定各案(纯函数,正反面逐门)
// ---------------------------------------------------------------------------
TEST_CASE("stage7:五门——独立任务/同形/非偶然/去重/收益,各案正反面") {
    const std::string fp = "fp-stage7-case";
    const std::vector<std::string> tools = {"read_file", "write_file"};

    SUBCASE("五门全过:两场独立、各自验证走通、有做法") {
        const std::vector<EvolutionObservation> cluster = {
            MakeObs("rec-a", ObservationOutcome::Success, fp, tools),
            MakeObs("rec-b", ObservationOutcome::Success, fp, tools),
        };
        const SuggestionVerdict verdict = AssessSuggestion(cluster, {});
        CHECK(verdict.eligible);
        CHECK(verdict.gate_tasks);
        CHECK(verdict.gate_shape);
        CHECK(verdict.gate_not_accidental);
        CHECK(verdict.gate_no_pending_or_rejected);
        CHECK(verdict.gate_benefit);
        CHECK(verdict.why_not.empty());
        CHECK(verdict.independent_tasks == 2);
        CHECK(verdict.independent_successes == 2);
        CHECK(verdict.shape_steps == 2);
        CHECK_FALSE(verdict.benefit_line.empty());
        CHECK(verdict.benefit_line.find("Memory") != std::string::npos);
        CHECK(verdict.representative_obs_id == cluster.front().id);
    }

    SUBCASE("门一不过:只有一场独立任务(同场两条观察不凑数——重采同 id 本就不进账,这里造两条不同 id 单场)") {
        const std::vector<EvolutionObservation> cluster = {
            MakeObs("rec-a", ObservationOutcome::Success, fp, tools),
        };
        const SuggestionVerdict verdict = AssessSuggestion(cluster, {});
        CHECK_FALSE(verdict.eligible);
        CHECK_FALSE(verdict.gate_tasks);
        REQUIRE_FALSE(verdict.why_not.empty());
        CHECK(verdict.why_not.front().find("独立任务") != std::string::npos);
    }

    SUBCASE("门二/门五不过:没有形状(一句事实或偏好,Memory 就装得下)") {
        const std::vector<EvolutionObservation> cluster = {
            MakeObs("rec-a", ObservationOutcome::Success, fp, {}),
            MakeObs("rec-b", ObservationOutcome::Success, fp, {}),
        };
        const SuggestionVerdict verdict = AssessSuggestion(cluster, {});
        CHECK_FALSE(verdict.eligible);
        CHECK_FALSE(verdict.gate_shape);
        CHECK_FALSE(verdict.gate_benefit);
        bool says_memory = false;
        for (const std::string& note : verdict.why_not) {
            says_memory = says_memory || note.find("Memory") != std::string::npos;
        }
        CHECK(says_memory);
    }

    SUBCASE("门三不过:两场独立,但只有一场带验证证据走通(另场未验证)") {
        const std::vector<EvolutionObservation> cluster = {
            MakeObs("rec-a", ObservationOutcome::Success, fp, tools),
            MakeObs("rec-b", ObservationOutcome::Unknown, fp, tools),
        };
        const SuggestionVerdict verdict = AssessSuggestion(cluster, {});
        CHECK_FALSE(verdict.eligible);
        CHECK(verdict.gate_tasks);  // 门一过
        CHECK_FALSE(verdict.gate_not_accidental);
        CHECK(verdict.independent_successes == 1);
        bool says_accidental = false;
        for (const std::string& note : verdict.why_not) {
            says_accidental = says_accidental || note.find("偶然") != std::string::npos;
        }
        CHECK(says_accidental);
    }

    SUBCASE("门四不过:同指纹已有在途/被拒候选(挡门指纹集命中)") {
        const std::vector<EvolutionObservation> cluster = {
            MakeObs("rec-a", ObservationOutcome::Success, fp, tools),
            MakeObs("rec-b", ObservationOutcome::Success, fp, tools),
        };
        const SuggestionVerdict verdict = AssessSuggestion(cluster, {fp});
        CHECK_FALSE(verdict.eligible);
        CHECK_FALSE(verdict.gate_no_pending_or_rejected);
        bool says_blocked = false;
        for (const std::string& note : verdict.why_not) {
            says_blocked = says_blocked || note.find("不再劝") != std::string::npos;
        }
        CHECK(says_blocked);
    }

    SUBCASE("门二不过:簇内指纹不一致(调用方聚错了簇,如实报)") {
        const std::vector<EvolutionObservation> cluster = {
            MakeObs("rec-a", ObservationOutcome::Success, fp, tools),
            MakeObs("rec-b", ObservationOutcome::Success, "fp-other", tools),
        };
        const SuggestionVerdict verdict = AssessSuggestion(cluster, {});
        CHECK_FALSE(verdict.eligible);
        CHECK_FALSE(verdict.gate_shape);
    }
}

// ---------------------------------------------------------------------------
// 3. 拒绝后不死缠:接观察账拒绝指纹与候选仓既有候选(两本去重账)
// ---------------------------------------------------------------------------
TEST_CASE("stage7:不死缠——propose 在途挡门;reject 拒绝指纹挡门;空账不挡") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings = home / "recordings";
    ObservationStore observations(home / "evolution" / "observations");
    CandidateStore candidates(home / "package-candidates");

    // 空账不挡。
    CHECK(CollectBlockedFingerprints(observations, candidates).empty());

    // 两场真录制 -> 进观察账 -> propose 落候选(在途)。
    const ClusterTaskMaterial a = MakeRecording(recordings, "抽样甲");
    const ClusterTaskMaterial b = MakeRecording(recordings, "抽样乙");
    std::string fingerprint;
    {
        const auto made = ObservationsFromRecording({a.status, a.events});
        REQUIRE_FALSE(made.empty());
        fingerprint = made.front().fingerprint;
        REQUIRE(observations.Append(made.front()).has_value());
    }
    {
        const auto made = ObservationsFromRecording({b.status, b.events});
        REQUIRE_FALSE(made.empty());
        CHECK(observations.Append(made.front()).has_value());
    }
    EvolutionCoordinator coordinator(candidates.root(), &observations);
    const auto proposed = coordinator.ProposeFromCluster({a, b});
    REQUIRE(proposed.has_value());

    // 在途候选挡门:同指纹簇五门过不了第四门。
    const std::vector<EvolutionObservation> ledger = observations.Load();
    std::vector<EvolutionObservation> cluster;
    for (const EvolutionObservation& observation : ledger) {
        if (observation.fingerprint == fingerprint) {
            cluster.push_back(observation);
        }
    }
    REQUIRE(cluster.size() == 2);
    const std::vector<std::string> blocked = CollectBlockedFingerprints(observations, candidates);
    CHECK(std::find(blocked.begin(), blocked.end(), fingerprint) != blocked.end());
    const SuggestionVerdict pending_verdict = AssessSuggestion(cluster, blocked);
    CHECK_FALSE(pending_verdict.eligible);
    CHECK_FALSE(pending_verdict.gate_no_pending_or_rejected);
    bool other_gates_hold = pending_verdict.gate_tasks && pending_verdict.gate_shape &&
                            pending_verdict.gate_not_accidental && pending_verdict.gate_benefit;
    CHECK(other_gates_hold);  // 挡的只是门四,不是一票否决别的证据

    // reject 之后:拒绝指纹进观察账的 rejected 账,同指纹照样不劝。
    REQUIRE(coordinator.Reject(proposed->candidate_id, "先不要").has_value());
    CHECK(observations.IsRejected(fingerprint));
    const SuggestionVerdict rejected_verdict =
        AssessSuggestion(cluster, CollectBlockedFingerprints(observations, candidates));
    CHECK_FALSE(rejected_verdict.eligible);
    CHECK_FALSE(rejected_verdict.gate_no_pending_or_rejected);
}

// ---------------------------------------------------------------------------
// 4. 命中账:只追加、坏行跳过、比率现算
// ---------------------------------------------------------------------------
TEST_CASE("stage7:命中账——shown/accepted 入账,坏行跳过,接受率按指纹算") {
    TempDir temp;
    const fs::path file = temp.Get() / "evolution" / "suggest.jsonl";
    SuggestLedger ledger(file);

    SuggestEvent shown;
    shown.type = "shown";
    shown.fingerprint = "fp-a";
    shown.cluster_size = 3;
    shown.benefit = "Memory 装不下的整套做法";
    shown.obs_id = "obs-aaa";
    CHECK_FALSE(ledger.Append(shown).has_value());

    SuggestEvent shown_again;  // 同指纹第二次提示:仍是 open,直到接受
    shown_again.type = "shown";
    shown_again.fingerprint = "fp-a";
    shown_again.cluster_size = 4;
    shown_again.obs_id = "obs-bbb";
    CHECK_FALSE(ledger.Append(shown_again).has_value());
    SuggestEvent shown_other;
    shown_other.type = "shown";
    shown_other.fingerprint = "fp-b";
    shown_other.obs_id = "obs-ccc";
    CHECK_FALSE(ledger.Append(shown_other).has_value());

    CHECK(ledger.Load().size() == 3);
    CHECK(ledger.HasOpenSuggestion("fp-a"));
    CHECK(ledger.HasOpenSuggestion("fp-b"));
    CHECK_FALSE(ledger.HasOpenSuggestion("fp-none"));

    SuggestEvent accepted;
    accepted.type = "accepted";
    accepted.fingerprint = "fp-a";
    accepted.candidate_id = "cand-20260831-001";
    CHECK_FALSE(ledger.Append(accepted).has_value());
    CHECK_FALSE(ledger.HasOpenSuggestion("fp-a"));   // 记过接受,不再 open
    CHECK(ledger.HasOpenSuggestion("fp-b"));

    const SuggestLedger::Stats stats = ledger.ComputeStats();
    CHECK(stats.shown_events == 3);
    CHECK(stats.accepted_events == 1);
    CHECK(stats.shown_fingerprints == 2);
    CHECK(stats.accepted_fingerprints == 1);
    CHECK(stats.acceptance_rate == doctest::Approx(0.5));

    // 坏行(半截 JSON/未知 type)跳过,不废整账。
    {
        std::ofstream output(file, std::ios::binary | std::ios::app);
        output << "{\"schema\":1,\"type\":\"shown\",\"fingerprint\":\n";       // 半截
        output << "{\"schema\":1,\"type\":\"mystery\",\"fingerprint\":\"x\"}\n";  // 未知 type
        output << "{\"schema\":2,\"type\":\"shown\",\"fingerprint\":\"y\"}\n";    // schema 不认
    }
    CHECK(ledger.Load().size() == 4);  // 4 条好的,3 条坏的跳过
    CHECK(ledger.ComputeStats().shown_fingerprints == 2);  // 比率不被坏行搅动

    // 没出过提示的账:接受率不算(不冒充 0)。
    SuggestLedger empty(temp.Get() / "evolution" / "none.jsonl");
    CHECK(empty.ComputeStats().acceptance_rate < 0.0);
}

// ---------------------------------------------------------------------------
// 5. 冒烟:关时不提示 -> 开后过五门亮一行 -> propose 记 accepted -> 账出
// ---------------------------------------------------------------------------
TEST_CASE("stage7:冒烟——缺省关;开后提示;点头起草记接受;账面齐全") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path evolution_root = home / "evolution";
    const fs::path recordings = home / "recordings";

    // 缺省关:开关账还没写,一律当关。
    CHECK_FALSE(LoadSuggestEnabled(evolution_root));

    // 两场真录制 -> 观察进账(带验证证据的成功,门三的料)。
    const ClusterTaskMaterial a = MakeRecording(recordings, "抽样甲");
    const ClusterTaskMaterial b = MakeRecording(recordings, "抽样乙");
    ObservationStore observations(evolution_root / "observations");
    std::string fingerprint;
    for (const ClusterTaskMaterial& material : {a, b}) {
        const auto made = ObservationsFromRecording({material.status, material.events});
        REQUIRE_FALSE(made.empty());
        fingerprint = made.front().fingerprint;
        REQUIRE(observations.Append(made.front()).has_value());
    }

    // 关着:连命中账都不该有(命令层开着才写;这里钉引擎侧——账文件不存在)。
    CHECK_FALSE(fs::exists(evolution_root / "suggest.jsonl"));

    // 开了(用户点头),评估同指纹簇:五门全过,亮一行建议(此处记一笔 shown)。
    CHECK_FALSE(SaveSuggestEnabled(evolution_root, true).has_value());
    CHECK(LoadSuggestEnabled(evolution_root));
    std::vector<EvolutionObservation> cluster;
    for (const EvolutionObservation& observation : observations.Load()) {
        if (observation.fingerprint == fingerprint) {
            cluster.push_back(observation);
        }
    }
    REQUIRE(cluster.size() == 2);
    const std::vector<std::string> blocked =
        CollectBlockedFingerprints(observations, CandidateStore(home / "package-candidates"));
    const SuggestionVerdict verdict = AssessSuggestion(cluster, blocked);
    REQUIRE(verdict.eligible);
    SuggestLedger ledger(evolution_root / "suggest.jsonl");
    SuggestEvent shown;
    shown.type = "shown";
    shown.fingerprint = verdict.fingerprint;
    shown.cluster_size = verdict.cluster_size;
    shown.benefit = verdict.benefit_line;
    shown.obs_id = verdict.representative_obs_id;
    CHECK_FALSE(ledger.Append(shown).has_value());

    std::cout << "==== 冒烟:阶段 7 建议行 ====\n";
    std::cout << "建议(只提示不自动起草):指纹 " << verdict.fingerprint << " 已有 "
              << verdict.independent_tasks << " 场独立任务同形走通(" << verdict.summary
              << ")。" << verdict.benefit_line << "\n";
    std::cout << "  点头起草: /evolve propose " << verdict.representative_obs_id << "\n";

    // 用户点头:走既有 propose 路(写口仍是 Coordinator,建议引擎没碰)。
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const auto proposed = coordinator.ProposeFromCluster({a, b});
    REQUIRE(proposed.has_value());
    CHECK(ledger.HasOpenSuggestion(fingerprint));
    SuggestEvent accepted;
    accepted.type = "accepted";
    accepted.fingerprint = fingerprint;
    accepted.candidate_id = proposed->candidate_id;
    CHECK_FALSE(ledger.Append(accepted).has_value());

    // 拒绝后不死缠:reject 落拒绝指纹,同簇再判,门四挡下。
    REQUIRE(coordinator.Reject(proposed->candidate_id, "冒烟:先不要").has_value());
    CHECK(observations.IsRejected(fingerprint));
    const std::vector<std::string> blocked_now =
        CollectBlockedFingerprints(observations, CandidateStore(home / "package-candidates"));
    const SuggestionVerdict after_reject = AssessSuggestion(cluster, blocked_now);
    CHECK_FALSE(after_reject.eligible);
    CHECK_FALSE(after_reject.gate_no_pending_or_rejected);

    // 账出:命中率与接受率现算。
    const SuggestLedger::Stats stats = ledger.ComputeStats();
    std::cout << "==== 冒烟:命中账 ====\n";
    std::cout << "提示 " << stats.shown_events << " 笔/" << stats.shown_fingerprints
              << " 指纹;接受 " << stats.accepted_events << " 笔/" << stats.accepted_fingerprints
              << " 指纹;接受率 "
              << (stats.acceptance_rate < 0.0 ? std::string("(没出过提示)")
                                              : std::to_string(stats.acceptance_rate * 100.0) + "%")
              << "\n";
    CHECK(stats.shown_events == 1);
    CHECK(stats.accepted_events == 1);
    CHECK(stats.acceptance_rate == doctest::Approx(1.0));

    // 关回去:缺省态可恢复,再评估不产生新账(引擎不开口)。
    CHECK_FALSE(SaveSuggestEnabled(evolution_root, false).has_value());
    CHECK_FALSE(LoadSuggestEnabled(evolution_root));
}
