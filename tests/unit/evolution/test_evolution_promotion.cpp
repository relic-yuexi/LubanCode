// 自进化闭环阶段 4:批准、安装与回滚单测。钉十件事——
//   1. approve:propose -> test -> approve 全链。批准页材料(§十清单)、
//      approval.json 记 approved+decision、状态 awaiting_approval -> staged、
//      store 里版本目录/channels 账/install-log 落齐;
//   2. 批准绑定哈希:未评测拒批;评测后改一字拒批(旧批准作废,指路);
//   3. code-bearing(带 Plugin)首版明拒,指路 Package trust;状态不动;
//   4. staging 原子性:staging 写一半失败,正式 store 不变;恢复后重批可装;
//      同版本不同内容不原地改;
//   5. use:点名 canary——状态 staged -> canary,snapshot 折出 canary 版本,
//      BuildPackageMount 挂上 store 层(scope=Store),技能 canonical 名在册;
//   6. promote:canary -> active,canary 指针清空,snapshot 换 active;
//   7. rollback:无父撤下(版本与账一枚不删)、有父切回父版;指定版本切换;
//   8. 快照钉死:promote/rollback 之后,已构造的 mount 照指旧版照旧跑;
//   9. store 手改:装配快照哈希对不上即拒挂并指路;挂载候选被剔;
//   10. 状态机各处拒:use 从非 staged、promote 从非 canary、终态不再迁移;
//       store 层与 user/dev 层同 id 的遮蔽次序(dev > project > store > user)。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "evolution/coordinator.hpp"
#include "evolution/observation_store.hpp"
#include "evolution/promoter.hpp"
#include "package/inventory.hpp"
#include "package/mounting.hpp"
#include "platform/paths.hpp"
#include "skills/workflow_recorder.hpp"

namespace {

namespace fs = std::filesystem;
using namespace lubancode::evolution;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_evolution_promotion_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

struct MadeRecording {
    lubancode::skills::RecordingStatus status;
    std::vector<lubancode::skills::RecordEvent> events;
};

// 同名(同 slug -> 同包 id)、异 goal(异指纹,躲开拒绝/去重门)。
MadeRecording MakeRecording(const fs::path& recordings_root, const std::string& goal) {
    lubancode::skills::RecordingStartInfo info;
    info.name = "provider 绑定排查";  // 两场同名 -> 同一只包
    info.goal = goal;
    info.acceptance = "产物可解析";
    info.cwd = "D:/nowhere";
    auto recorder = lubancode::skills::WorkflowRecorder::Start(recordings_root, info);
    REQUIRE(recorder.has_value());
    if (!recorder.has_value()) {
        return {};
    }
    recorder->RecordToolCall("read_file", nlohmann::json{{"path", "cfg/provider.yaml"}}, "item-1",
                             "toolu-1");
    recorder->RecordToolResult("read_file", false, "读到绑定段", "", "", "item-1");
    recorder->RecordToolCall("run_command", nlohmann::json{{"command", "probe legacy"}}, "item-2",
                             "toolu-2");
    recorder->RecordToolResult("run_command", true, "协议读不了", "error", "net.unsupported",
                               "item-2");  // 稳定失败路进排错节
    CHECK(recorder->Stop("yaml 可解析").has_value());
    MadeRecording made;
    made.status.id = recorder->id();
    made.status.name = info.name;
    made.status.dir = recorder->dir();
    made.status.finished = true;
    made.events = lubancode::skills::ReadRecordingEvents(recorder->dir());
    return made;
}

// 一只候选全链走到 evaluated(评测五道门跑完)。
struct MadeCandidate {
    std::string candidate_id;
    std::string package_id;
    std::string content_hash;
    fs::path dir;
};
MadeCandidate RunToEvaluated(EvolutionCoordinator& coordinator, const fs::path& recordings_root,
                             const std::string& goal) {
    const MadeRecording made = MakeRecording(recordings_root, goal);
    const auto proposed = coordinator.ProposeRecording(made.status, made.events);
    REQUIRE(proposed.has_value());
    const auto tested = coordinator.Test(proposed->candidate_id);
    REQUIRE(tested.has_value());
    REQUIRE(tested->state_after == "evaluated");
    MadeCandidate out;
    out.candidate_id = proposed->candidate_id;
    out.package_id = proposed->package_id;
    out.content_hash = proposed->content_hash;
    out.dir = proposed->candidate_dir;
    return out;
}

// 候选 package.yaml 的版本段改写(父子版本用例:第二只候选瞄准 0.2.0)。
bool RewriteManifestVersion(const fs::path& package_dir, const std::string& from,
                            const std::string& to) {
    const auto text = ReadText(package_dir / "package.yaml");
    if (!text.has_value()) {
        return false;
    }
    std::string out = *text;
    const std::size_t at = out.find("version: " + from);
    if (at == std::string::npos) {
        return false;
    }
    out.replace(at, ("version: " + from).size(), "version: " + to);
    return WriteText(package_dir / "package.yaml", out);
}

// 演化账改写:补父版(父子版本用例)。内容哈希只盖 package/,改账不改哈希。
bool InjectParent(const fs::path& candidate_dir, const std::string& parent_version,
                  const std::string& parent_hash) {
    const auto text = ReadText(candidate_dir / "evolution.json");
    if (!text.has_value()) {
        return false;
    }
    auto record = ParseEvolutionRecord(*text);
    if (!record.has_value()) {
        return false;
    }
    EvolutionRecordParent parent;
    parent.version = parent_version;
    parent.content_hash = parent_hash;
    record->parent = parent;
    return WriteText(candidate_dir / "evolution.json", SerializeEvolutionRecord(*record));
}

// 哈希对账账本改写:approval.json 与 eval-plan.json 绑新哈希(模拟"按新哈希
// 重写计划再 test"的正路;内容变过,旧账作废)。
bool RebindHashLedgers(const fs::path& candidate_dir, const std::string& new_hash) {
    if (const auto text = ReadText(candidate_dir / "approval.json"); text.has_value()) {
        auto approval = ParseApprovalRecord(*text);
        if (!approval.has_value()) {
            return false;
        }
        approval->content_hash = new_hash;
        if (!WriteText(candidate_dir / "approval.json", SerializeApprovalRecord(*approval))) {
            return false;
        }
    }
    if (const auto text = ReadText(candidate_dir / "eval-plan.json"); text.has_value()) {
        auto plan = nlohmann::json::parse(*text);
        plan["content_hash"] = new_hash;
        if (!WriteText(candidate_dir / "eval-plan.json", plan.dump(2) + "\n")) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. approve 全链
// ---------------------------------------------------------------------------

TEST_CASE("晋升.approve:批准页材料齐全,原子落 store,状态 staged") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const MadeCandidate made = RunToEvaluated(coordinator, home / "recordings", "排查 provider 绑定误判");

    const auto approved = coordinator.Approve(made.candidate_id);
    REQUIRE(approved.has_value());
    CHECK(approved->installed_version == "0.1.0");
    CHECK(fs::exists(approved->version_dir / "package.yaml"));

    // ---- 批准页材料(§十清单逐项) ----
    const auto& brief = approved->brief;
    CHECK(brief.candidate_id == made.candidate_id);
    CHECK(brief.package_id == made.package_id);
    CHECK(brief.candidate_version == "0.1.0-candidate.1");
    CHECK(brief.content_hash == made.content_hash);
    CHECK(brief.parent_line.find("无父版") != std::string::npos);
    REQUIRE(brief.source_lines.size() == 1);
    CHECK(brief.source_lines[0].rfind("recording ", 0) == 0);
    REQUIRE(brief.components_added.size() == 1);
    CHECK(brief.components_added[0].rfind("skills/", 0) == 0);
    CHECK(brief.components_changed.empty());
    CHECK(brief.components_removed.empty());
    CHECK(brief.permissions_added.empty());  // content-only:权限差异无
    CHECK(brief.tools_added.empty());
    CHECK(brief.tier == "content-only");
    REQUIRE(brief.eval_summary.has_value());
    CHECK(brief.eval_summary->line_count > 0);
    CHECK(brief.rollback_target_line.find("撤下") != std::string::npos);

    // ---- store 落位:版本目录 + channels 账 + install-log ----
    const fs::path store_root = home / "package-store";
    CHECK(fs::exists(store_root / lubancode::platform::Utf8ToPath(made.package_id) / "0.1.0" /
                     "package.yaml"));
    const auto channels_text =
        ReadText(store_root / lubancode::platform::Utf8ToPath(made.package_id) / "channels.json");
    REQUIRE(channels_text.has_value());
    const auto channels = ParseStoreChannels(*channels_text);
    REQUIRE(channels.has_value());
    REQUIRE(channels->versions.count("0.1.0") == 1);
    CHECK(channels->versions.at("0.1.0").content_hash == made.content_hash);
    CHECK(channels->versions.at("0.1.0").candidate_id == made.candidate_id);
    CHECK_FALSE(channels->active.has_value());   // 只装架,未选用
    CHECK_FALSE(channels->canary.has_value());
    const std::vector<StoreLogEvent> log = LoadStoreLog(
        store_root / lubancode::platform::Utf8ToPath(made.package_id) / "install-log.jsonl");
    REQUIRE(log.size() == 1);
    CHECK(log[0].event == "install");
    CHECK(log[0].version == "0.1.0");

    // ---- 批准账与状态 ----
    const auto approval_text = ReadText(made.dir / "approval.json");
    REQUIRE(approval_text.has_value());
    const auto approval = ParseApprovalRecord(*approval_text);
    REQUIRE(approval.has_value());
    CHECK(approval->status == "approved");
    REQUIRE(approval->decision.has_value());
    CHECK(approval->decision->decided_by == "user");
    CHECK(approval->decision->decision == "approved");
    CHECK(approval->content_hash == made.content_hash);  // 只认当前哈希
    CHECK(coordinator.store().Find(made.candidate_id)->state == CandidateState::Staged);

    // ---- 状态账:evaluated -> awaiting_approval -> staged 两笔都在 ----
    const auto state_text = ReadText(made.dir / "state.jsonl").value_or("");
    CHECK(state_text.find("\"to\":\"awaiting_approval\"") != std::string::npos);
    CHECK(state_text.find("\"to\":\"staged\"") != std::string::npos);

    // ---- staged 未选用:快照折不出挂载项 ----
    const VersionStore::Snapshot snapshot = coordinator.version_store().BuildSnapshot();
    CHECK(snapshot.empty());

    // ---- 再批一回:状态机不让(staged 只往前走);store 侧 Install 幂等 ----
    const auto again = coordinator.Approve(made.candidate_id);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().find("staged") != std::string::npos);
    CHECK(coordinator.store().Find(made.candidate_id)->state == CandidateState::Staged);
    VersionStore store(store_root);
    const auto reinstalled = store.Install(made.dir / "package", made.candidate_id, made.content_hash);
    REQUIRE(reinstalled.has_value());
    CHECK(reinstalled->already_present);  // 同版本同哈希:不重装(channels 账不翻倍)
    const std::vector<StoreLogEvent> log_after = LoadStoreLog(
        store_root / lubancode::platform::Utf8ToPath(made.package_id) / "install-log.jsonl");
    CHECK(log_after.size() == 1);  // 幂等不追加事件
}

// ---------------------------------------------------------------------------
// 2. 批准绑定哈希
// ---------------------------------------------------------------------------

TEST_CASE("晋升.approve:未评测拒批;评测后改一字拒批(旧批准作废)") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);

    SUBCASE("未评测:drafted 直接批,拒") {
        const MadeRecording made = MakeRecording(home / "recordings", "排查 provider 绑定误判");
        const auto proposed = coordinator.ProposeRecording(made.status, made.events);
        REQUIRE(proposed.has_value());
        const auto approved = coordinator.Approve(proposed->candidate_id);
        REQUIRE_FALSE(approved.has_value());
        CHECK(approved.error().find("/evolve test") != std::string::npos);
        CHECK(coordinator.store().Find(proposed->candidate_id)->state == CandidateState::Drafted);
    }

    SUBCASE("评测后改一字:哈希对不上,拒批指路") {
        const MadeCandidate made = RunToEvaluated(coordinator, home / "recordings", "排查 provider 绑定误判");
        // 改候选一字:包里那份 SKILL 正文添一行(内容哈希即变;路径不猜,
        // 盘上现找)。
        fs::path found_skill;
        for (auto it = fs::recursive_directory_iterator(made.dir / "package");
             it != fs::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file() && it->path().filename().generic_string() == "SKILL.md") {
                found_skill = it->path();
                break;
            }
        }
        REQUIRE(found_skill.has_filename());
        const auto text = ReadText(found_skill);
        REQUIRE(text.has_value());
        REQUIRE(WriteText(found_skill, *text + "\n后来添的一行,候选内容变了。\n"));

        const auto approved = coordinator.Approve(made.candidate_id);
        REQUIRE_FALSE(approved.has_value());
        CHECK(approved.error().find("内容变过") != std::string::npos);
        CHECK(approved.error().find("作废") != std::string::npos);
        CHECK(coordinator.store().Find(made.candidate_id)->state == CandidateState::Evaluated);
        // store 一枚没落。
        CHECK_FALSE(fs::exists(home / "package-store"));
    }
}

// ---------------------------------------------------------------------------
// 3. code-bearing 拒批
// ---------------------------------------------------------------------------

TEST_CASE("晋升.approve:code-bearing 首版明拒,指路 Package trust") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);

    // 起草后先塞一件 process Plugin 草稿(评测前就位,评测账全绑新哈希),
    // 再评测、再批——档位门上明拒。
    const MadeRecording material = MakeRecording(home / "recordings", "排查 provider 绑定误判");
    const auto proposed = coordinator.ProposeRecording(material.status, material.events);
    REQUIRE(proposed.has_value());
    REQUIRE(WriteText(proposed->candidate_dir / "package" / "plugins" / "probe" / "plugin.lua",
                      "-- demo process plugin draft\nreturn {}\n"));
    const std::string new_hash = ComputeCandidateContentHash(proposed->candidate_dir / "package");
    REQUIRE_FALSE(new_hash.empty());
    REQUIRE(new_hash != proposed->content_hash);
    REQUIRE(RebindHashLedgers(proposed->candidate_dir, new_hash));
    const auto tested = coordinator.Test(proposed->candidate_id);
    REQUIRE(tested.has_value());

    const auto approved = coordinator.Approve(proposed->candidate_id);
    REQUIRE_FALSE(approved.has_value());
    CHECK(approved.error().find("code-bearing") != std::string::npos);
    CHECK(approved.error().find("trust") != std::string::npos);
    CHECK(coordinator.store().Find(proposed->candidate_id)->state == CandidateState::Evaluated);
    CHECK_FALSE(fs::exists(home / "package-store"));
}

// ---------------------------------------------------------------------------
// 4. staging 原子性
// ---------------------------------------------------------------------------

TEST_CASE("晋升.staging:写一半失败正式 store 不变;恢复重批可装;同版本不原地改") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const MadeCandidate made = RunToEvaluated(coordinator, home / "recordings", "排查 provider 绑定误判");

    const fs::path pkg_dir = home / "package-store" / lubancode::platform::Utf8ToPath(made.package_id);

    SUBCASE("staging 造不出来:正式版本目录一枚不落,清障后重批可装") {
        // .staging 处先占一个普通文件:create_directories 必败(写一半即停)。
        REQUIRE(WriteText(pkg_dir / ".staging", "占位文件,staging 造不出目录"));
        const auto approved = coordinator.Approve(made.candidate_id);
        REQUIRE_FALSE(approved.has_value());
        CHECK_FALSE(fs::exists(pkg_dir / "0.1.0"));           // 正式 store 不变
        CHECK(coordinator.store().Find(made.candidate_id)->state ==
              CandidateState::AwaitingApproval);  // 停在批准页,可重批

        fs::remove(pkg_dir / ".staging");
        const auto retried = coordinator.Approve(made.candidate_id);
        REQUIRE(retried.has_value());
        CHECK(fs::exists(pkg_dir / "0.1.0" / "package.yaml"));
        CHECK(coordinator.store().Find(made.candidate_id)->state == CandidateState::Staged);
    }

    SUBCASE("同版本不同内容:明拒,盘上版本纹丝不动") {
        const auto approved = coordinator.Approve(made.candidate_id);
        REQUIRE(approved.has_value());
        const std::string before =
            ComputeCandidateContentHash(pkg_dir / "0.1.0");

        // 造一份同版本号、内容不同的包,直接喂 Install(撞版本的路)。
        const fs::path other = temp.Get() / "other-package";
        REQUIRE(WriteText(other / "package.yaml",
                          "schema: 1\nid: " + made.package_id +
                              "\nversion: 0.1.0\nname: demo\ndescription: 另一份内容。\n"));
        REQUIRE(WriteText(other / "skills" / "other" / "SKILL.md",
                          "---\nname: other\ndescription: 另一份内容。\n---\n\n# 另一份\n"));
        VersionStore store(home / "package-store");
        const auto collision = store.Install(other, "cand-00000000-999", "sha256:" + std::string(64, '1'));
        REQUIRE_FALSE(collision.has_value());
        CHECK(collision.error().find("不原地改") != std::string::npos);
        CHECK(ComputeCandidateContentHash(pkg_dir / "0.1.0") == before);  // 盘上没动
    }
}

// ---------------------------------------------------------------------------
// 5/6/7. use -> promote -> rollback 全链(无父撤下)
// ---------------------------------------------------------------------------

TEST_CASE("晋升.use-promote-rollback:点名 canary、晋升、撤下;账一枚不删") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const MadeCandidate made = RunToEvaluated(coordinator, home / "recordings", "排查 provider 绑定误判");
    REQUIRE(coordinator.Approve(made.candidate_id).has_value());
    const fs::path pkg_dir = home / "package-store" / lubancode::platform::Utf8ToPath(made.package_id);

    // ---- use:点名 canary ----
    const auto used = coordinator.Use(made.candidate_id);
    REQUIRE(used.has_value());
    CHECK(used->version == "0.1.0");
    CHECK(coordinator.store().Find(made.candidate_id)->state == CandidateState::Canary);
    {
        const auto channels = coordinator.version_store().LoadChannels(made.package_id);
        REQUIRE(channels.has_value());
        REQUIRE(channels->canary.has_value());
        CHECK(channels->canary->version == "0.1.0");
        CHECK(channels->canary->via == "canary");
        CHECK_FALSE(channels->active.has_value());
    }

    // ---- 快照:canary 折出来,路径+哈希齐全 ----
    VersionStore::Snapshot snapshot = coordinator.version_store().BuildSnapshot();
    REQUIRE(snapshot.entries.size() == 1);
    CHECK(snapshot.entries[0].channel == "canary");
    CHECK(snapshot.entries[0].version == "0.1.0");
    CHECK(snapshot.entries[0].intact);
    CHECK(snapshot.entries[0].actual_hash == made.content_hash);

    // ---- 挂载:store 选中版本进会话钉快照(scope=Store),技能 canonical 在册 ----
    lubancode::package::PackageMountInput input;
    input.store_candidates = coordinator.version_store().ScanSelectedCandidates();
    const lubancode::package::PackageMount mount = lubancode::package::BuildPackageMount(input);
    const auto* entry = mount.Find(made.package_id);
    REQUIRE(entry != nullptr);
    CHECK(entry->scope == lubancode::package::PackageScope::Store);
    CHECK(entry->version_text == "0.1.0");
    CHECK(entry->content_hash == made.content_hash.substr(7));
    REQUIRE(entry->mounted_canonical_ids.size() == 1);  // 一份 Skill
    CHECK(entry->mounted_canonical_ids[0].rfind(made.package_id + ":", 0) == 0);
    const std::vector<lubancode::tools::PackagedSkillRoot> roots =
        lubancode::package::MountSkillRoots(mount);
    REQUIRE(roots.size() == 1);
    CHECK(roots[0].skills_dir == pkg_dir / "0.1.0" / "skills");

    // ---- promote:canary -> active ----
    const auto promoted = coordinator.Promote(made.candidate_id);
    REQUIRE(promoted.has_value());
    CHECK(coordinator.store().Find(made.candidate_id)->state == CandidateState::Active);
    {
        const auto channels = coordinator.version_store().LoadChannels(made.package_id);
        REQUIRE(channels.has_value());
        REQUIRE(channels->active.has_value());
        CHECK(channels->active->version == "0.1.0");
        CHECK(channels->active->via == "promote");
        CHECK_FALSE(channels->canary.has_value());  // 灰度收走
    }
    snapshot = coordinator.version_store().BuildSnapshot();
    REQUIRE(snapshot.entries.size() == 1);
    CHECK(snapshot.entries[0].channel == "active");

    // ---- 旧 mount 钉死:promote 之后,已构造的快照照指旧版(此处同版本,
    //      钉死的证据在父子版本用例里展开) ----

    // ---- rollback:无父撤下;版本与账一枚不删 ----
    const auto rolled = coordinator.Rollback(made.package_id);
    REQUIRE(rolled.has_value());
    CHECK(rolled->from_version == "0.1.0");
    CHECK_FALSE(rolled->to_version.has_value());  // 无父 = 撤下
    REQUIRE(rolled->rolled_back_candidates.size() == 1);
    CHECK(rolled->rolled_back_candidates[0] == made.candidate_id);
    CHECK(coordinator.store().Find(made.candidate_id)->state == CandidateState::RolledBack);
    {
        const auto channels = coordinator.version_store().LoadChannels(made.package_id);
        REQUIRE(channels.has_value());
        CHECK_FALSE(channels->active.has_value());
        CHECK_FALSE(channels->canary.has_value());
        CHECK(channels->versions.count("0.1.0") == 1);  // 版本账还在
    }
    // 版本目录、评测账、批准账、迁移账全在;install-log 四笔齐全。
    CHECK(fs::exists(pkg_dir / "0.1.0" / "package.yaml"));
    CHECK_FALSE(LoadEvalResults(made.dir / "eval-results.jsonl").empty());
    const auto approval = ParseApprovalRecord(ReadText(made.dir / "approval.json").value_or(""));
    REQUIRE(approval.has_value());
    CHECK(approval->status == "approved");  // 回滚不抹批准账
    const std::vector<StoreLogEvent> log = LoadStoreLog(pkg_dir / "install-log.jsonl");
    REQUIRE(log.size() == 4);
    CHECK(log[0].event == "install");
    CHECK(log[1].event == "canary");
    CHECK(log[2].event == "promote");
    CHECK(log[3].event == "rollback");
    // 撤下之后:快照空(不再挂载)。
    CHECK(coordinator.version_store().BuildSnapshot().empty());

    // ---- 终态不再迁移:批不了、用不了;rollback 二回没有可回的版本 ----
    CHECK_FALSE(coordinator.Approve(made.candidate_id).has_value());
    CHECK_FALSE(coordinator.Use(made.candidate_id).has_value());
    const auto again = coordinator.Rollback(made.package_id);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().find("没有可回") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 7. 父子版本:回滚切回父版;指定版本切换;快照钉死
// ---------------------------------------------------------------------------

TEST_CASE("晋升.父子版本:0.2.0 晋升后一条命令切回 0.1.0;在跑快照照旧") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);

    // ---- 第一只:0.1.0 走到 active ----
    const MadeCandidate first = RunToEvaluated(coordinator, home / "recordings", "排查 provider 绑定误判");
    REQUIRE(coordinator.Approve(first.candidate_id).has_value());
    REQUIRE(coordinator.Use(first.candidate_id).has_value());
    REQUIRE(coordinator.Promote(first.candidate_id).has_value());

    // ---- 第二只:同包 0.2.0,演化账记父版 0.1.0 ----
    const MadeRecording second_material = MakeRecording(home / "recordings", "换一套更稳的绑定探法");
    const auto second = coordinator.ProposeRecording(second_material.status, second_material.events);
    REQUIRE(second.has_value());
    REQUIRE(second->package_id == first.package_id);  // 同名录制 -> 同一只包
    REQUIRE(RewriteManifestVersion(second->candidate_dir / "package", "0.1.0", "0.2.0"));
    const std::string second_hash = ComputeCandidateContentHash(second->candidate_dir / "package");
    REQUIRE_FALSE(second_hash.empty());
    REQUIRE(InjectParent(second->candidate_dir, "0.1.0", first.content_hash));
    REQUIRE(RebindHashLedgers(second->candidate_dir, second_hash));
    REQUIRE(coordinator.Test(second->candidate_id).has_value());
    const auto approved = coordinator.Approve(second->candidate_id);
    REQUIRE(approved.has_value());
    CHECK(approved->installed_version == "0.2.0");
    CHECK(approved->brief.parent_line.find("0.1.0") != std::string::npos);
    CHECK(approved->brief.rollback_target_line.find("0.1.0") != std::string::npos);
    REQUIRE(coordinator.Use(second->candidate_id).has_value());

    // ---- 在跑会话的快照(canary 0.2.0)先钉住 ----
    lubancode::package::PackageMountInput input;
    input.store_candidates = coordinator.version_store().ScanSelectedCandidates();
    const lubancode::package::PackageMount running_mount = lubancode::package::BuildPackageMount(input);
    const auto* running = running_mount.Find(first.package_id);
    REQUIRE(running != nullptr);
    CHECK(running->version_text == "0.2.0");
    CHECK(running->content_hash == second_hash.substr(7));

    // ---- promote 0.2.0,再 rollback:一条命令切回父版 0.1.0 ----
    REQUIRE(coordinator.Promote(second->candidate_id).has_value());
    const auto rolled = coordinator.Rollback(first.package_id);
    REQUIRE(rolled.has_value());
    CHECK(rolled->from_version == "0.2.0");
    REQUIRE(rolled->to_version.has_value());
    CHECK(*rolled->to_version == "0.1.0");
    CHECK(rolled->rolled_back_candidates.size() == 2);  // 两只候选都落 rolled_back

    // ---- 钉死证据:旧 mount 仍是 0.2.0(目录还在,技能照读);新装配拿 0.1.0 ----
    CHECK(running->version_text == "0.2.0");
    CHECK(running->content_hash == second_hash.substr(7));
    CHECK(fs::exists(running->package_root / "package.yaml"));
    const std::vector<lubancode::tools::PackagedSkillRoot> running_roots =
        lubancode::package::MountSkillRoots(running_mount);
    REQUIRE(running_roots.size() == 1);
    CHECK(running_roots[0].skills_dir.filename() == "skills");
    CHECK(running_roots[0].skills_dir.parent_path().filename() == "0.2.0");

    lubancode::package::PackageMountInput fresh_input;
    fresh_input.store_candidates = coordinator.version_store().ScanSelectedCandidates();
    const lubancode::package::PackageMount fresh_mount = lubancode::package::BuildPackageMount(fresh_input);
    const auto* fresh = fresh_mount.Find(first.package_id);
    REQUIRE(fresh != nullptr);
    CHECK(fresh->version_text == "0.1.0");
    CHECK(fresh->content_hash == first.content_hash.substr(7));

    // ---- 指定版本切换:再点名切回 0.2.0(store 账里有,切得动) ----
    const auto forward = coordinator.Rollback(first.package_id, "0.2.0");
    REQUIRE(forward.has_value());
    REQUIRE(forward->to_version.has_value());
    CHECK(*forward->to_version == "0.2.0");
    // 版本一枚不删:两枚都在盘上。
    const fs::path pkg_dir = home / "package-store" / lubancode::platform::Utf8ToPath(first.package_id);
    CHECK(fs::exists(pkg_dir / "0.1.0" / "package.yaml"));
    CHECK(fs::exists(pkg_dir / "0.2.0" / "package.yaml"));
    // 查无此版的指名:拒。
    const auto nowhere = coordinator.Rollback(first.package_id, "9.9.9");
    REQUIRE_FALSE(nowhere.has_value());
    CHECK(nowhere.error().find("不在") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 8/9. store 手改:拒挂并指路
// ---------------------------------------------------------------------------

TEST_CASE("晋升.store 手改:装配哈希对不上即拒挂并指路;挂载候选被剔") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const MadeCandidate made = RunToEvaluated(coordinator, home / "recordings", "排查 provider 绑定误判");
    REQUIRE(coordinator.Approve(made.candidate_id).has_value());
    REQUIRE(coordinator.Use(made.candidate_id).has_value());
    REQUIRE(coordinator.Promote(made.candidate_id).has_value());

    // ---- 手改 store 版本目录里的一份文件 ----
    const fs::path pkg_dir = home / "package-store" / lubancode::platform::Utf8ToPath(made.package_id);
    fs::path skill_file;
    for (auto it = fs::recursive_directory_iterator(pkg_dir / "0.1.0");
         it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file() && it->path().filename().generic_string() == "SKILL.md") {
            skill_file = it->path();
            break;
        }
    }
    REQUIRE(skill_file.has_filename());
    const auto text = ReadText(skill_file);
    REQUIRE(text.has_value());
    REQUIRE(WriteText(skill_file, *text + "\n手改的一行。\n"));

    // ---- 快照:intact=false,note 指路(拒挂 + 修法) ----
    const VersionStore::Snapshot snapshot = coordinator.version_store().BuildSnapshot();
    CHECK(snapshot.entries.empty());
    REQUIRE(snapshot.rejected.size() == 1);
    CHECK_FALSE(snapshot.rejected[0].intact);
    CHECK(snapshot.rejected[0].note.find("拒挂") != std::string::npos);
    CHECK(snapshot.rejected[0].note.find("rollback") != std::string::npos);

    // ---- 挂载侧照 AddEvolutionStoreSelections 的规矩剔掉,发现账还在 ----
    std::vector<lubancode::package::PackageCandidate> candidates =
        coordinator.version_store().ScanSelectedCandidates();
    REQUIRE(candidates.size() == 1);  // /package list 仍看得见(发现不等于挂载)
    for (const auto& rejected : snapshot.rejected) {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [&](const auto& candidate) {
                                            return candidate.dir_name == rejected.package_id;
                                        }),
                         candidates.end());
    }
    lubancode::package::PackageMountInput input;
    input.store_candidates = candidates;
    const lubancode::package::PackageMount mount = lubancode::package::BuildPackageMount(input);
    CHECK(mount.Find(made.package_id) == nullptr);  // 拒挂

    // ---- 指针再动它也被哈希门拦下:rollback 指名切到手改过的版本,拒 ----
    VersionStore store(home / "package-store");
    const auto refuse = store.RollbackTo(made.package_id, "0.1.0", "想切回手改过的版本");
    REQUIRE_FALSE(refuse.has_value());
    CHECK(refuse.error().find("改过") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 10. 状态机各处拒 + 层间遮蔽
// ---------------------------------------------------------------------------

TEST_CASE("晋升.状态机:use 从非 staged 拒;promote 从非 canary 拒") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const MadeCandidate made = RunToEvaluated(coordinator, home / "recordings", "排查 provider 绑定误判");

    const auto early_use = coordinator.Use(made.candidate_id);
    REQUIRE_FALSE(early_use.has_value());
    CHECK(early_use.error().find("staged") != std::string::npos);

    REQUIRE(coordinator.Approve(made.candidate_id).has_value());
    const auto early_promote = coordinator.Promote(made.candidate_id);
    REQUIRE_FALSE(early_promote.has_value());
    CHECK(early_promote.error().find("canary") != std::string::npos);

    // use 之后重复 use:报已 canary(不迁移)。
    REQUIRE(coordinator.Use(made.candidate_id).has_value());
    const auto repeat_use = coordinator.Use(made.candidate_id);
    REQUIRE_FALSE(repeat_use.has_value());
    CHECK(repeat_use.error().find("canary") != std::string::npos);
}

TEST_CASE("晋升.遮蔽:store 压 user,dev 压 store(dev > project > store > user)") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const MadeCandidate made = RunToEvaluated(coordinator, home / "recordings", "排查 provider 绑定误判");
    REQUIRE(coordinator.Approve(made.candidate_id).has_value());
    REQUIRE(coordinator.Use(made.candidate_id).has_value());

    // user 层放一份同 id 的手装拷贝:store 胜。
    const fs::path user_layer = temp.Get() / "user-layer";
    REQUIRE(WriteText(user_layer / lubancode::platform::Utf8ToPath(made.package_id) / "package.yaml",
                      "schema: 1\nid: " + made.package_id +
                          "\nversion: 0.0.1\nname: demo\ndescription: 手装拷贝。\n"));
    lubancode::package::PackageMountInput input;
    input.scan.user_root = user_layer;
    input.store_candidates = coordinator.version_store().ScanSelectedCandidates();
    auto mount = lubancode::package::BuildPackageMount(input);
    const auto* entry = mount.Find(made.package_id);
    REQUIRE(entry != nullptr);
    CHECK(entry->scope == lubancode::package::PackageScope::Store);
    CHECK(entry->version_text == "0.1.0");

    // dev 层再放一份:dev 胜(显式调试层压一切)。
    const fs::path dev_layer = temp.Get() / "dev-layer";
    REQUIRE(WriteText(dev_layer / lubancode::platform::Utf8ToPath(made.package_id) / "package.yaml",
                      "schema: 1\nid: " + made.package_id +
                          "\nversion: 0.9.0\nname: demo\ndescription: 调试拷贝。\n"));
    input.scan.dev_roots.push_back(dev_layer);
    mount = lubancode::package::BuildPackageMount(input);
    entry = mount.Find(made.package_id);
    REQUIRE(entry != nullptr);
    CHECK(entry->scope == lubancode::package::PackageScope::Dev);
    CHECK(entry->version_text == "0.9.0");
}

// ---------------------------------------------------------------------------
// channels.json 序列化回读
// ---------------------------------------------------------------------------

TEST_CASE("晋升.channels 账:序列化回读,坏文本给 nullopt") {
    StoreChannels channels;
    channels.schema = 1;
    channels.package_id = "evolve.demo";
    StoreVersionInfo info;
    info.version = "0.1.0";
    info.content_hash = "sha256:" + std::string(64, 'a');
    info.candidate_id = "cand-20260829-001";
    info.installed_at = "2026-08-29T00:00:00Z";
    channels.versions["0.1.0"] = info;
    StoreChannelPointer active;
    active.version = "0.1.0";
    active.content_hash = info.content_hash;
    active.candidate_id = info.candidate_id;
    active.set_at = "2026-08-29T01:00:00Z";
    active.via = "promote";
    channels.active = active;

    const std::string text = SerializeStoreChannels(channels);
    const auto back = ParseStoreChannels(text);
    REQUIRE(back.has_value());
    CHECK(back->package_id == "evolve.demo");
    REQUIRE(back->versions.count("0.1.0") == 1);
    CHECK(back->versions.at("0.1.0").content_hash == info.content_hash);
    REQUIRE(back->active.has_value());
    CHECK(back->active->via == "promote");
    CHECK_FALSE(back->canary.has_value());

    CHECK_FALSE(ParseStoreChannels("{}").has_value());
    CHECK_FALSE(ParseStoreChannels("not json").has_value());
    CHECK_FALSE(ParseStoreChannels("{\"schema\":2,\"package_id\":\"x\"}").has_value());
}
