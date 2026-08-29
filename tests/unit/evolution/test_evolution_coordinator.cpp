// 自进化闭环阶段 2:EvolutionCoordinator 单测。钉六件事——
//   1. propose:一场录制变成完整候选(目录五件 + package/ 两份文本),
//      evolution.json 各字段(parent null、来源、生成器、组件差异)、
//      approval awaiting_approval、状态账 observed->drafted;
//   2. 落盘的 package.yaml 过 manifest 严格解析,SKILL.md 过安装校验;
//   3. 整包哈希与 package 阶段 1 盘点算法同值;
//   4. reject:状态落 rejected、approval 记 decision 与指纹、观察账
//      IsRejected;再 propose 同类(同 fingerprint)被拒之门外;
//   5. 终态不可再迁移;
//   6. 防偷装:候选只落 package-candidates,四层扫描(ScanPackages)
//      扫不到它——/package list 全程看不见。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "evolution/coordinator.hpp"
#include "evolution/observation_store.hpp"
#include "package/inventory.hpp"
#include "package/manifest.hpp"
#include "platform/paths.hpp"
#include "skills/skill_drafter.hpp"
#include "skills/workflow_recorder.hpp"

namespace {

namespace fs = std::filesystem;
using namespace lubancode::evolution;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_evolution_coordinator_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

std::optional<std::string> ReadText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// 造一场含稳定失败路的完整录制件。帮手函数非 void,断言用 CHECK。
struct MadeRecording {
    lubancode::skills::RecordingStatus status;
    std::vector<lubancode::skills::RecordEvent> events;
};

MadeRecording MakeRecording(const fs::path& recordings_root, const std::string& name,
                            const std::string& goal) {
    lubancode::skills::RecordingStartInfo info;
    info.name = name;
    info.goal = goal;
    info.acceptance = "产物可解析";
    info.cwd = "D:/nowhere";
    auto recorder = lubancode::skills::WorkflowRecorder::Start(recordings_root, info);
    CHECK(recorder.has_value());
    if (!recorder.has_value()) {
        return {};
    }
    recorder->RecordToolCall("read_file", nlohmann::json{{"path", "cfg/provider.yaml"}}, "item-1",
                             "toolu-1");
    recorder->RecordToolResult("read_file", false, "读到绑定段", "", "", "item-1");  // is_error=false
    recorder->RecordToolCall("run_command", nlohmann::json{{"command", "probe legacy"}}, "item-2",
                             "toolu-2");
    recorder->RecordToolResult("run_command", true, "协议读不了", "error", "net.unsupported",
                               "item-2");  // is_error=true:稳定失败路
    CHECK(recorder->Stop("yaml 可解析").has_value());
    MadeRecording made;
    made.status.id = recorder->id();
    made.status.name = name;
    made.status.dir = recorder->dir();
    made.status.finished = true;
    made.events = lubancode::skills::ReadRecordingEvents(recorder->dir());
    return made;
}

}  // namespace

TEST_CASE("协调器.propose:一场录制变成完整候选,账目齐全") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const MadeRecording made =
        MakeRecording(home / "recordings", "provider 绑定排查", "排查 provider 绑定误判");
    ObservationStore observations(home / "evolution" / "observations");

    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const auto result = coordinator.ProposeRecording(made.status, made.events);
    REQUIRE(result.has_value());
    CHECK(result->package_id.rfind("evolve.", 0) == 0);
    CHECK(result->candidate_id.rfind("cand-", 0) == 0);
    CHECK(result->candidate_version == "0.1.0-candidate.1");
    CHECK(result->content_hash.rfind("sha256:", 0) == 0);
    CHECK(result->content_hash.size() == 7 + 64);

    // ---- 目录形状:五件套 + package/ 两份文本 ----
    const fs::path dir = result->candidate_dir;
    CHECK(fs::exists(dir / "evolution.json"));
    CHECK(fs::exists(dir / "eval-plan.json"));
    CHECK(fs::exists(dir / "eval-results.jsonl"));
    CHECK(fs::exists(dir / "approval.json"));
    CHECK(fs::exists(dir / "state.jsonl"));
    CHECK(fs::exists(dir / "package" / "package.yaml"));
    CHECK(fs::exists(dir / "package" / lubancode::platform::Utf8ToPath(result->skill_rel_path)));

    // ---- package.yaml:严格解析 ----
    const auto yaml_text = ReadText(dir / "package" / "package.yaml");
    REQUIRE(yaml_text.has_value());
    const auto manifest = lubancode::package::ParsePackageManifest(*yaml_text);
    REQUIRE(manifest.has_value());
    CHECK(manifest->id == result->package_id);
    CHECK(manifest->version.text == "0.1.0");

    // ---- SKILL.md:过安装校验 ----
    const auto skill_text = ReadText(dir / "package" /
                                          lubancode::platform::Utf8ToPath(result->skill_rel_path));
    REQUIRE(skill_text.has_value());
    CHECK(lubancode::skills::ValidateSkillMarkdownForInstall(*skill_text).has_value());

    // ---- evolution.json:各字段 ----
    const auto record_text = ReadText(dir / "evolution.json");
    REQUIRE(record_text.has_value());
    const auto record = ParseEvolutionRecord(*record_text);
    REQUIRE(record.has_value());
    CHECK(record->candidate_id == result->candidate_id);
    CHECK(record->package_id == result->package_id);
    CHECK_FALSE(record->parent.has_value());  // 无父明写 null,不假装升级
    CHECK(record->sources.recording_ids == std::vector<std::string>{made.status.id});
    CHECK(record->generator.provider == "builtin");
    CHECK(record->generator.model == "skill-drafter");
    CHECK_FALSE(record->generator.prompt_revision.empty());
    CHECK(record->changes.components_added.size() == 1);  // 只添一份 Skill,最小包
    CHECK(record->changes.permissions_added.empty());     // content-only:无权限差异
    CHECK(record->changes.tools_added.empty());           // 不添工具
    CHECK(record->objective.find("provider") != std::string::npos);

    // ---- approval.json:awaiting_approval、未决、hash 绑定 ----
    const auto approval_text = ReadText(dir / "approval.json");
    REQUIRE(approval_text.has_value());
    const auto approval = ParseApprovalRecord(*approval_text);
    REQUIRE(approval.has_value());
    CHECK(approval->status == "awaiting_approval");
    CHECK(approval->tier == "content-only");
    CHECK_FALSE(approval->decision.has_value());
    CHECK(approval->content_hash == result->content_hash);

    // ---- 状态账:observed -> drafted 一行 ----
    const auto state_text = ReadText(dir / "state.jsonl").value_or("");
    CHECK(state_text.find("\"from\":\"observed\"") != std::string::npos);
    CHECK(state_text.find("\"to\":\"drafted\"") != std::string::npos);
    CHECK(coordinator.store().Find(result->candidate_id)->state == CandidateState::Drafted);

    // ---- 整包哈希与 package 阶段 1 盘点算法同值 ----
    CHECK(ComputeCandidateContentHash(dir / "package") == result->content_hash);
    {
        lubancode::package::PackageCandidate direct;
        direct.scope = lubancode::package::PackageScope::Dev;
        direct.package_root = dir / "package";
        direct.dir_name = "package";
        CHECK("sha256:" + lubancode::package::BuildPackageInventory(direct).content_hash ==
              result->content_hash);
    }

    // ---- 观察账有了这条来源(可回指) ----
    CHECK(observations.Find(MakeObservationId(ObservationSource::Recording, made.status.id))
              .has_value());
}

TEST_CASE("协调器.reject:落 rejected、指纹进账、同类不再被劝") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const MadeRecording made =
        MakeRecording(home / "recordings", "provider 绑定排查", "排查 provider 绑定误判");
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const auto proposed = coordinator.ProposeRecording(made.status, made.events);
    REQUIRE(proposed.has_value());

    // 观察指纹先拿到(等会儿对账)。
    const std::string observation_id =
        MakeObservationId(ObservationSource::Recording, made.status.id);
    const auto observation = observations.Find(observation_id);
    REQUIRE(observation.has_value());
    const std::string fingerprint = observation->fingerprint;

    // ---- reject ----
    const auto rejected = coordinator.Reject(proposed->candidate_id, "演示用,不要");
    REQUIRE(rejected.has_value());
    CHECK(rejected->fingerprint == fingerprint);
    CHECK(coordinator.store().Find(proposed->candidate_id)->state == CandidateState::Rejected);

    // approval.json:rejected + decision 齐(用户、时刻、理由、指纹)。
    const auto approval_text = ReadText(rejected->candidate_dir / "approval.json");
    REQUIRE(approval_text.has_value());
    const auto approval = ParseApprovalRecord(*approval_text);
    REQUIRE(approval.has_value());
    CHECK(approval->status == "rejected");
    REQUIRE(approval->decision.has_value());
    CHECK(approval->decision->decided_by == "user");
    CHECK(approval->decision->decision == "rejected");
    CHECK(approval->decision->reason == "演示用,不要");
    CHECK(approval->decision->fingerprint == fingerprint);
    CHECK(approval->content_hash == proposed->content_hash);

    // 观察账:指纹已拒。
    CHECK(observations.IsRejected(fingerprint));
    CHECK(observations.LoadRejected().size() == 1);

    // ---- 再 propose 同类(同 fingerprint):被拒之门外,不死缠 ----
    const auto again = coordinator.ProposeRecording(made.status, made.events);
    REQUIRE(!again.has_value());
    CHECK(again.error().find("不再起草") != std::string::npos);
    CHECK(coordinator.store().LoadAll().size() == 1);  // 候选没翻倍

    // ---- 终态不可再迁移 ----
    const auto re_reject = coordinator.Reject(proposed->candidate_id, "再拒一回");
    REQUIRE(!re_reject.has_value());
    CHECK(re_reject.error().find("终态") != std::string::npos);
}

TEST_CASE("协调器.diff:无父与空对照,列新增文件与 SKILL 摘要") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const MadeRecording made =
        MakeRecording(home / "recordings", "provider 绑定排查", "排查 provider 绑定误判");
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const auto proposed = coordinator.ProposeRecording(made.status, made.events);
    REQUIRE(proposed.has_value());

    const auto diff = coordinator.Diff(proposed->candidate_id);
    REQUIRE(diff.has_value());
    CHECK(diff->baseline.find("无父版") != std::string::npos);
    CHECK(diff->baseline.find("空对照") != std::string::npos);
    REQUIRE(diff->added.size() == 2);  // package.yaml + skills/<id>/SKILL.md
    CHECK(diff->added[0].rel == "package.yaml");
    CHECK(diff->added[1].rel == proposed->skill_rel_path);
    bool saw_skill = false;
    for (const auto& file : diff->added) {
        if (file.is_skill) {
            saw_skill = true;
            CHECK(file.hash.rfind("sha256:", 0) == 0);
        }
    }
    CHECK(saw_skill);
    CHECK_FALSE(diff->skill_summary.empty());   // 正文摘要非空
    CHECK(diff->skill_summary.find("排错") !=
          std::string::npos);  // 稳定失败路进了排错节
    CHECK(diff->skill_summary.find("验收") != std::string::npos);

    // 查无此候:报错不炸。
    const auto missing = coordinator.Diff("cand-20990101-999");
    CHECK_FALSE(missing.has_value());
}

TEST_CASE("防偷装:候选不进四层扫描,/package list 看不见") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const MadeRecording made =
        MakeRecording(home / "recordings", "provider 绑定排查", "排查 provider 绑定误判");
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const auto proposed = coordinator.ProposeRecording(made.status, made.events);
    REQUIRE(proposed.has_value());

    // 四层扫描全指向正经目录(user 层还是 home 下的 packages/,与候选仓
    // package-candidates/ 平级但不同名):一只都扫不到。
    lubancode::package::ScanOptions options;
    options.user_root = home / "packages";
    options.project_root = temp.Get() / "project" / ".lubancode" / "packages";
    const auto scanned = lubancode::package::ScanPackages(options);
    CHECK(scanned.empty());

    // 反证:把候选目录直接摆进 user 层才扫得着——证明上面扫不到是"候选仓
    // 不在扫描层里",不是包本身不合法。
    const fs::path smuggle = home / "packages" / "smuggled";
    fs::create_directories(smuggle.parent_path());
    fs::copy(proposed->candidate_dir / "package", smuggle, fs::copy_options::recursive);
    const auto scanned2 = lubancode::package::ScanPackages(options);
    REQUIRE(scanned2.size() == 1);
    CHECK(scanned2[0].manifest.has_value());
    CHECK(scanned2[0].manifest->id == proposed->package_id);

    // 候选仓自己也只认 evolution.json 齐的候选,状态是 drafted(不是 active)。
    const auto all = coordinator.store().LoadAll();
    REQUIRE(all.size() == 1);
    CHECK(all[0].state == CandidateState::Drafted);
    CHECK(all[0].package_id == proposed->package_id);
}
