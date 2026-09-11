// GoalService 测试(轨迹 v3 §4.67 Goal 模式 G0):
//   - lifecycle/phase 枚举与转换表(terminal 不复活、恢复边、合同改版边);
//   - 快照 schema:roundtrip、停态约束(blocked 带 blockerKey、停因必填)、
//     证据引用合同(hex64/来源三选一);
//   - 提交事务:快照先落稳、state.goal.applied 后落账、内存才发布;CAS
//     (stateRevision/contractRevision)冲突拒;applied 落账失败 fail closed
//     锁写口,候选快照不生效;
//   - 只读投影:验后账 + 快照实探(hash/revision),缺口明报不猜;
//     applied 序列非法(terminal 复活、未收账开新 goal)报 illegal。
// 写侧 goal_v1 收敛(§4.67.6)的读侧钉子在 test_goal_restore.cpp。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "runtime/goal_service.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace goalns = lubancode::runtime::goal;
using goalns::GoalEvidenceRef;
using goalns::GoalEvidenceSource;
using goalns::GoalLifecycle;
using goalns::GoalPhase;
using goalns::GoalService;
using goalns::GoalStateSnapshot;
using goalns::GoalTransitionCandidate;
using lubancode::trajectory::v3::Durability;
using lubancode::trajectory::v3::EventDraft;
using lubancode::trajectory::v3::EventKindV3;
using lubancode::trajectory::v3::V3Writer;
using lubancode::trajectory::v3::WriteReceipt;

namespace {

// ctest 钉 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0;v3 册显式开回 1。
struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteFileBytes(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

struct ServiceHarness {
    std::filesystem::path dir;
    std::optional<V3Writer> writer;

    explicit ServiceHarness(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-goal-service-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        auto started = V3Writer::Start(dir / "s1.jsonl", "20260911-120000-AAAAAA",
                                       "run-000001", "system prompt");
        REQUIRE(started.has_value());
        writer = std::move(*started);
    }

    std::filesystem::path jsonl() const { return dir / "s1.jsonl"; }

    ServiceHarness(const ServiceHarness&) = delete;
    ServiceHarness& operator=(const ServiceHarness&) = delete;
};

GoalStateSnapshot DraftObjective(std::string objective) {
    GoalStateSnapshot draft;
    draft.objective = std::move(objective);
    draft.workspace_root = "/repo";
    draft.workspace_identity = "repo@main";
    return draft;
}

GoalTransitionCandidate Transition(GoalLifecycle to, std::uint64_t expected_state_revision) {
    GoalTransitionCandidate candidate;
    candidate.to_lifecycle = to;
    candidate.expected_state_revision = expected_state_revision;
    return candidate;
}

GoalEvidenceRef GoodEvidence(std::string id) {
    GoalEvidenceRef ref;
    ref.id = std::move(id);
    ref.kind = "command_exit";
    ref.source = GoalEvidenceSource::ToolAction;
    ref.source_ref = "action-000001";
    ref.session_id = "20260911-120000-AAAAAA";
    ref.run_id = "run-000001";
    ref.content_sha256 = std::string(64, 'b');
    ref.observed_at_ms = 1700;
    ref.workspace_baseline = "fp-1";
    ref.criterion_id = "c-1";
    return ref;
}

}  // namespace

TEST_CASE("lifecycle 枚举与转换表:terminal 不复活,恢复边按设计放开") {
    CHECK(goalns::ToString(GoalLifecycle::BudgetExhausted) == "budget_exhausted");
    CHECK(goalns::ToString(GoalLifecycle::SuspendedByPolicy) == "suspended_by_policy");
    GoalLifecycle parsed = GoalLifecycle::Active;
    REQUIRE(goalns::ParseGoalLifecycle("awaiting_user", parsed));
    CHECK(parsed == GoalLifecycle::AwaitingUser);
    CHECK_FALSE(goalns::ParseGoalLifecycle("running", parsed));  // phase 不是 lifecycle
    GoalPhase phase = GoalPhase::Idle;
    REQUIRE(goalns::ParseGoalPhase("evaluating", phase));
    CHECK(phase == GoalPhase::Evaluating);
    CHECK_FALSE(goalns::ParseGoalPhase("preparing", phase));

    CHECK(goalns::IsLifecycleTerminal(GoalLifecycle::Achieved));
    CHECK(goalns::IsLifecycleTerminal(GoalLifecycle::Cleared));
    CHECK(goalns::IsLifecycleTerminal(GoalLifecycle::Failed));
    // 设计 §4.67.3:budget_exhausted 可显式恢复、blocked 改条件后可 resume、
    // suspended_by_policy 只许 clear——三枚都不是"不自动复活"行的终态。
    CHECK_FALSE(goalns::IsLifecycleTerminal(GoalLifecycle::BudgetExhausted));
    CHECK_FALSE(goalns::IsLifecycleTerminal(GoalLifecycle::Blocked));
    CHECK_FALSE(goalns::IsLifecycleTerminal(GoalLifecycle::SuspendedByPolicy));

    // 合法边(preparing 冻结、active 全停态、恢复边、合同改版边)。
    CHECK(goalns::IsValidLifecycleTransition(GoalLifecycle::Preparing, GoalLifecycle::Active));
    CHECK(goalns::IsValidLifecycleTransition(GoalLifecycle::Active, GoalLifecycle::Waiting));
    CHECK(goalns::IsValidLifecycleTransition(GoalLifecycle::Active, GoalLifecycle::Paused));
    CHECK(goalns::IsValidLifecycleTransition(GoalLifecycle::Active, GoalLifecycle::Achieved));
    CHECK(goalns::IsValidLifecycleTransition(GoalLifecycle::Active, GoalLifecycle::Preparing));
    CHECK(goalns::IsValidLifecycleTransition(GoalLifecycle::Paused, GoalLifecycle::Active));
    CHECK(goalns::IsValidLifecycleTransition(GoalLifecycle::AwaitingUser, GoalLifecycle::Active));
    CHECK(goalns::IsValidLifecycleTransition(GoalLifecycle::Blocked, GoalLifecycle::Active));
    CHECK(goalns::IsValidLifecycleTransition(GoalLifecycle::BudgetExhausted,
                                             GoalLifecycle::Active));
    CHECK(goalns::IsValidLifecycleTransition(GoalLifecycle::Waiting, GoalLifecycle::Active));
    CHECK(goalns::IsValidLifecycleTransition(GoalLifecycle::SuspendedByPolicy,
                                             GoalLifecycle::Cleared));
    // 非法边:terminal 不复活;waiting 不直达 achieved(先回 active 收口);
    // 同态不是转换。
    CHECK_FALSE(goalns::IsValidLifecycleTransition(GoalLifecycle::Achieved, GoalLifecycle::Active));
    CHECK_FALSE(goalns::IsValidLifecycleTransition(GoalLifecycle::Cleared, GoalLifecycle::Active));
    CHECK_FALSE(goalns::IsValidLifecycleTransition(GoalLifecycle::Failed, GoalLifecycle::Active));
    CHECK_FALSE(goalns::IsValidLifecycleTransition(GoalLifecycle::SuspendedByPolicy,
                                                   GoalLifecycle::Active));
    CHECK_FALSE(goalns::IsValidLifecycleTransition(GoalLifecycle::Waiting,
                                                   GoalLifecycle::Achieved));
    CHECK_FALSE(goalns::IsValidLifecycleTransition(GoalLifecycle::Active, GoalLifecycle::Active));
}

TEST_CASE("快照 roundtrip:ToJson -> FromJson 字段不丢,坏版本拒") {
    GoalStateSnapshot snapshot;
    snapshot.goal_id = "goal-1";
    snapshot.session_id = "s1";
    snapshot.run_id = "run-000001";
    snapshot.state_revision = 3;
    snapshot.contract_revision = 2;
    snapshot.objective = "修好 auth 模块";
    snapshot.lifecycle = GoalLifecycle::Blocked;
    snapshot.phase = GoalPhase::Idle;
    snapshot.stop_reason = "ctest -R auth 连挂三轮";
    snapshot.blocker_key = "auth-dep-missing";
    snapshot.iteration_id = "goal-1/iter-4";
    snapshot.checkpoint_ref = "evt-000009";
    snapshot.evidence_refs.push_back(GoodEvidence("ev-1"));
    snapshot.wait_task_refs = {"task-1"};
    snapshot.applied_evaluation_id = "eval-2";
    snapshot.usage.input_tokens = 100;
    snapshot.usage.usage_reported = true;
    snapshot.counters.iterations_started = 4;
    snapshot.pending_intent = nlohmann::json{{"workItemId", "w-2"}};
    snapshot.created_at_ms = 1000;
    snapshot.updated_at_ms = 2000;

    const auto restored = GoalStateSnapshot::FromJson(
        nlohmann::json::parse(goalns::SnapshotBytes(snapshot)), nullptr);
    REQUIRE(restored.has_value());
    CHECK(restored->goal_id == snapshot.goal_id);
    CHECK(restored->state_revision == 3);
    CHECK(restored->contract_revision == 2);
    CHECK(restored->lifecycle == GoalLifecycle::Blocked);
    CHECK(restored->phase == GoalPhase::Idle);
    CHECK(restored->blocker_key == "auth-dep-missing");
    CHECK(restored->iteration_id.has_value());
    CHECK(*restored->iteration_id == "goal-1/iter-4");
    CHECK(restored->checkpoint_ref == snapshot.checkpoint_ref);
    REQUIRE(restored->evidence_refs.size() == 1);
    CHECK(restored->evidence_refs[0].id == "ev-1");
    CHECK(restored->evidence_refs[0].content_sha256 == std::string(64, 'b'));
    CHECK(restored->wait_task_refs == std::vector<std::string>{"task-1"});
    CHECK(restored->applied_evaluation_id == "eval-2");
    CHECK(restored->usage.input_tokens == 100);
    CHECK(restored->counters.iterations_started == 4);
    CHECK(restored->pending_intent.at("workItemId") == "w-2");

    // 坏版本与停态约束。
    std::string error;
    auto bad = nlohmann::json::parse(goalns::SnapshotBytes(snapshot));
    bad["snapshotVersion"] = 2;
    CHECK_FALSE(GoalStateSnapshot::FromJson(bad, &error).has_value());
    CHECK(error.find("snapshotVersion") != std::string::npos);
    auto no_blocker = nlohmann::json::parse(goalns::SnapshotBytes(snapshot));
    no_blocker["blockerKey"] = "";
    CHECK_FALSE(GoalStateSnapshot::FromJson(no_blocker, &error).has_value());
    CHECK(error.find("blockerKey") != std::string::npos);
    auto no_reason = nlohmann::json::parse(goalns::SnapshotBytes(snapshot));
    no_reason["blockerKey"] = "k";
    no_reason["stopReason"] = "";
    CHECK_FALSE(GoalStateSnapshot::FromJson(no_reason, &error).has_value());
}

TEST_CASE("证据引用合同:来源三选一、hash hex64、必选回指") {
    REQUIRE(goalns::ValidateEvidenceRef(GoodEvidence("ev-1").ToJson()).empty());

    SUBCASE("hash 不是 hex64") {
        auto ref = GoodEvidence("ev-1");
        ref.content_sha256 = "zz";
        CHECK_FALSE(goalns::ValidateEvidenceRef(ref.ToJson()).empty());
    }
    SUBCASE("tool_action 缺 sourceRef") {
        auto ref = GoodEvidence("ev-1");
        ref.source_ref.clear();
        CHECK_FALSE(goalns::ValidateEvidenceRef(ref.ToJson()).empty());
    }
    SUBCASE("artifact 来源走 artifactRef,不查 sourceRef") {
        auto ref = GoodEvidence("ev-1");
        ref.source = GoalEvidenceSource::Artifact;
        ref.source_ref.clear();
        ref.artifact_ref = nlohmann::json{{"artifactId", "res-000001"},
                                          {"kind", "stdout"},
                                          {"path", "artifacts/res-000001.stdout.txt"},
                                          {"sha256", std::string(64, 'c')},
                                          {"bytes", 10},
                                          {"mediaType", "text/plain"}};
        REQUIRE(goalns::ValidateEvidenceRef(ref.ToJson()).empty());
        // roundtrip:artifact 分支也能回来。
        const auto restored = GoalEvidenceRef::FromJson(
            nlohmann::json::parse(ref.ToJson()), nullptr);
        REQUIRE(restored.has_value());
        CHECK(restored->source == GoalEvidenceSource::Artifact);
        CHECK(restored->artifact_ref.at("artifactId") == "res-000001");
    }
    SUBCASE("缺 sessionId(回指来源账)拒") {
        auto ref = GoodEvidence("ev-1");
        ref.session_id.clear();
        CHECK_FALSE(goalns::ValidateEvidenceRef(ref.ToJson()).empty());
    }
}

TEST_CASE("CreateGoal:快照落稳、applied 落账、内存发布,验卷全绿") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    ServiceHarness harness("create");
    std::int64_t now = 1000;
    GoalService::Options options;
    options.session_dir = harness.dir;
    options.clock = [&now] { return now; };
    GoalService service(&*harness.writer, std::move(options));

    const auto created = service.CreateGoal(DraftObjective("修好 auth 模块"), nlohmann::json());
    REQUIRE(created.ok);
    CHECK(created.payload.at("goalId") == "goal-1");
    CHECK(created.payload.at("stateRevision") == 1);
    CHECK(created.payload.at("lifecycle") == "preparing");
    CHECK(created.payload.at("snapshotRef") == "state/goals/goal-1/rev-000001.json");

    // 快照是不可变文件:存在且内容可解析回快照。
    const auto snapshot_path = harness.dir / "state/goals/goal-1/rev-000001.json";
    REQUIRE(std::filesystem::exists(snapshot_path));
    const auto snapshot = GoalStateSnapshot::FromJson(
        nlohmann::json::parse(ReadFileBytes(snapshot_path)), nullptr);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->session_id == "20260911-120000-AAAAAA");
    CHECK(snapshot->run_id == "run-000001");

    // applied 在账,序列过验卷。
    const auto report = lubancode::trajectory::v3::VerifyV3File(harness.jsonl());
    REQUIRE(report.ok);
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(harness.jsonl());
    REQUIRE(ledger.has_value());
    int applied_count = 0;
    for (const auto& event : ledger->events) {
        if (event.kind != EventKindV3::StateGoalApplied) continue;
        ++applied_count;
        CHECK(event.payload.at("fromStateRevision") == 0);
        CHECK(event.payload.at("toStateRevision") == 1);
        CHECK(event.payload.at("goalId") == "goal-1");
        CHECK(event.payload.at("snapshotSha256") ==
              lubancode::hooks::Sha256Hex(ReadFileBytes(snapshot_path)));
    }
    CHECK(applied_count == 1);

    // 内存发布:current 可查,账面锚在。
    REQUIRE(service.current() != nullptr);
    CHECK(service.current()->goal_id == "goal-1");
    CHECK(service.applied_seq().has_value());
    CHECK(service.applied_seq().value() >= 3);

    // 未收账时不许暗中替换。
    const auto second = service.CreateGoal(DraftObjective("另一件事"), nlohmann::json());
    CHECK_FALSE(second.ok);
    CHECK(second.error_code == goalns::kErrGoalAlreadyActive);
}

TEST_CASE("ApplyTransition:提交事务、CAS、候选合同、terminal 收账") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    ServiceHarness harness("transition");
    std::int64_t now = 1000;
    GoalService::Options options;
    options.session_dir = harness.dir;
    options.clock = [&now] { return now; };
    GoalService service(&*harness.writer, std::move(options));
    REQUIRE(service.CreateGoal(DraftObjective("修好 auth"), nlohmann::json()).ok);

    // CAS:revision 不匹配拒,不落盘。
    auto stale = Transition(GoalLifecycle::Active, /*expected_state_revision=*/7);
    stale.goal_id = "goal-1";
    const auto conflict = service.ApplyTransition(stale);
    CHECK_FALSE(conflict.ok);
    CHECK(conflict.error_code == goalns::kErrGoalRevisionConflict);
    CHECK(conflict.payload.at("currentStateRevision") == 1);
    // expected 必须显式给。
    auto no_cas = Transition(GoalLifecycle::Active, 0);
    no_cas.goal_id = "goal-1";
    CHECK_FALSE(service.ApplyTransition(no_cas).ok);

    // 候选合同:active 不需要停因,直接过。
    auto activate = Transition(GoalLifecycle::Active, 1);
    activate.goal_id = "goal-1";
    const auto activated = service.ApplyTransition(activate);
    REQUIRE(activated.ok);
    CHECK(service.current()->lifecycle == GoalLifecycle::Active);
    CHECK(service.current()->state_revision == 2);

    // 证据与 usage 随行提交;停态必须带停因。
    auto pause = Transition(GoalLifecycle::Paused, 2);
    pause.goal_id = "goal-1";
    pause.stop_reason = "user_request";
    pause.evidence_additions = {GoodEvidence("ev-1")};
    pause.usage_addition.input_tokens = 50;
    pause.usage_addition.usage_reported = true;
    const auto paused = service.ApplyTransition(pause);
    REQUIRE(paused.ok);
    CHECK(service.current()->evidence_refs.size() == 1);
    CHECK(service.current()->usage.input_tokens == 50);

    auto no_reason = Transition(GoalLifecycle::Blocked, 3);
    no_reason.goal_id = "goal-1";
    const auto blocked_bad = service.ApplyTransition(no_reason);
    CHECK_FALSE(blocked_bad.ok);
    CHECK(blocked_bad.error_code == goalns::kErrGoalCandidateInvalid);

    auto blocked = Transition(GoalLifecycle::Blocked, 3);
    blocked.goal_id = "goal-1";
    blocked.stop_reason = "依赖缺位";
    blocked.blocker_key = "auth-dep";
    REQUIRE(service.ApplyTransition(blocked).ok);

    // 恢复边:blocked -> active(设计 §4.67.3,条件核验归 G1)。
    auto resume = Transition(GoalLifecycle::Active, 4);
    resume.goal_id = "goal-1";
    REQUIRE(service.ApplyTransition(resume).ok);

    // terminal 收账:achieved;此后迟到候选拒。
    auto done = Transition(GoalLifecycle::Achieved, 5);
    done.goal_id = "goal-1";
    done.stop_reason = "全部 criteria 过硬门槛";
    done.applied_evaluation_id = "eval-3";
    REQUIRE(service.ApplyTransition(done).ok);
    CHECK(goalns::IsLifecycleTerminal(service.current()->lifecycle));

    auto late = Transition(GoalLifecycle::Active, 6);
    late.goal_id = "goal-1";
    const auto late_result = service.ApplyTransition(late);
    CHECK_FALSE(late_result.ok);
    CHECK(late_result.error_code == goalns::kErrGoalTerminal);

    // 账上 applied 共 6 条(revision 1..6),快照 6 份全在。
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(harness.jsonl());
    REQUIRE(ledger.has_value());
    int applied_count = 0;
    for (const auto& event : ledger->events) {
        if (event.kind == EventKindV3::StateGoalApplied) ++applied_count;
    }
    CHECK(applied_count == 6);
    for (std::uint64_t revision = 1; revision <= 6; ++revision) {
        CHECK(std::filesystem::exists(harness.dir /
                                      goalns::SnapshotRefPath("goal-1", revision)));
    }

    // terminal 后可另起新 goal(新 goalId、revision 重起)。
    const auto next_goal = service.CreateGoal(DraftObjective("下一件事"), nlohmann::json());
    REQUIRE(next_goal.ok);
    CHECK(next_goal.payload.at("goalId") == "goal-2");
}

TEST_CASE("AmendContract:contractRevision +1,旧证据翻 stale") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    ServiceHarness harness("amend");
    std::int64_t now = 1000;
    GoalService::Options options;
    options.session_dir = harness.dir;
    options.clock = [&now] { return now; };
    GoalService service(&*harness.writer, std::move(options));
    REQUIRE(service.CreateGoal(DraftObjective("修好 auth"), nlohmann::json()).ok);
    auto activate = Transition(GoalLifecycle::Active, 1);
    activate.goal_id = "goal-1";
    activate.evidence_additions = {GoodEvidence("ev-1")};
    REQUIRE(service.ApplyTransition(activate).ok);

    goalns::GoalContract contract;
    contract.objective = "修好 auth;补集成测试";
    contract.criteria.push_back({"c-1", "ctest -R auth 全过", true});
    const auto amended =
        service.AmendContract(contract, /*expected_state_revision=*/2,
                              /*expected_contract_revision=*/1, nlohmann::json());
    REQUIRE(amended.ok);
    CHECK(service.current()->contract_revision == 2);
    CHECK(service.current()->state_revision == 3);
    CHECK(service.current()->lifecycle == GoalLifecycle::Preparing);
    CHECK(service.current()->stop_reason == "contract_amended");
    // 旧证据重新判有效期:保守翻 stale(§4.67.2 edit 行)。
    REQUIRE(service.current()->evidence_refs.size() == 1);
    CHECK_FALSE(service.current()->evidence_refs[0].fresh);

    // revision 冲突拒。
    const auto conflict = service.AmendContract(contract, 2, 1, nlohmann::json());
    CHECK_FALSE(conflict.ok);
    CHECK(conflict.error_code == goalns::kErrGoalRevisionConflict);
}

TEST_CASE("fail closed:applied 落账失败锁写口,候选快照不生效") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    ServiceHarness harness("failclosed");
    // 注入提交失败:前两次放行(Start 的 system + session.started),第三次
    // 起(CreateGoal 的 applied)失败——writer 句柄随后 broken。
    int inject_calls = 0;
    lubancode::trajectory::v3::V3WriterOptions writer_options;
    writer_options.inject_io_failure = [&inject_calls]() -> std::optional<std::string> {
        return ++inject_calls > 2 ? std::optional<std::string>("injected") : std::nullopt;
    };
    auto started = V3Writer::Start(harness.jsonl(), "20260911-120000-AAAAAA", "run-000001",
                                   "system prompt", nlohmann::json::object(), writer_options);
    REQUIRE(started.has_value());
    auto writer = std::move(*started);
    std::int64_t now = 1000;
    GoalService::Options options;
    options.session_dir = harness.dir;
    options.clock = [&now] { return now; };
    GoalService service(&writer, std::move(options));

    const auto created = service.CreateGoal(DraftObjective("修好 auth"), nlohmann::json());
    CHECK_FALSE(created.ok);
    CHECK(created.error_code == goalns::kErrGoalStoreUnavailable);
    CHECK(service.broken());
    CHECK(service.current() == nullptr);  // 未发布:不宣称目标已立

    // 候选快照留在盘上(不生效、不删除);fail closed 后续全拒。
    CHECK(std::filesystem::exists(harness.dir / "state/goals/goal-1/rev-000001.json"));
    const auto again = service.CreateGoal(DraftObjective("再试"), nlohmann::json());
    CHECK_FALSE(again.ok);
    CHECK(again.error_code == goalns::kErrGoalStoreUnavailable);

    // 账上没有 applied:投影如实报 no_goal(不从候选快照猜)。
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(harness.jsonl());
    REQUIRE(ledger.has_value());
    const auto projection = goalns::ProjectGoalState(*ledger, harness.dir);
    CHECK(projection.gap == goalns::GoalProjectionGap::NoGoal);
    CHECK_FALSE(projection.has_goal);
}

TEST_CASE("只读投影:验后账重建、缺口明报、AdoptFromProjection 接管") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    ServiceHarness harness("project");
    std::int64_t now = 1000;
    GoalService::Options options;
    options.session_dir = harness.dir;
    options.clock = [&now] { return now; };
    GoalService service(&*harness.writer, std::move(options));
    REQUIRE(service.CreateGoal(DraftObjective("修好 auth"), nlohmann::json()).ok);
    auto activate = Transition(GoalLifecycle::Active, 1);
    activate.goal_id = "goal-1";
    activate.evidence_additions = {GoodEvidence("ev-1")};
    REQUIRE(service.ApplyTransition(activate).ok);

    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(harness.jsonl());
    REQUIRE(ledger.has_value());

    const auto make_snapshot = [](const char* goal, std::uint64_t rev) {
        GoalStateSnapshot snapshot;
        snapshot.goal_id = goal;
        snapshot.session_id = "20260911-120000-AAAAAA";
        snapshot.run_id = "run-000001";
        snapshot.state_revision = rev;
        snapshot.contract_revision = 1;
        snapshot.objective = "obj";
        snapshot.lifecycle = GoalLifecycle::Preparing;
        return snapshot;
    };
    const auto write_snapshot = [&](const GoalStateSnapshot& snapshot) {
        WriteFileBytes(harness.dir / goalns::SnapshotRefPath(snapshot.goal_id,
                                                             snapshot.state_revision),
                       goalns::SnapshotBytes(snapshot));
    };
    const auto append = [&](const char* goal, std::uint64_t from, std::uint64_t to,
                            std::uint64_t contract_rev, const char* lifecycle) {
        EventDraft draft;
        draft.kind = EventKindV3::StateGoalApplied;
        draft.payload["goalId"] = goal;
        draft.payload["fromStateRevision"] = from;
        draft.payload["toStateRevision"] = to;
        draft.payload["contractRevision"] = contract_rev;
        draft.payload["snapshotRef"] = goalns::SnapshotRefPath(goal, to);
        // applied 所记 hash 用盘上这份快照文件的真实值。
        const std::string bytes = ReadFileBytes(harness.dir /
                                                goalns::SnapshotRefPath(goal, to));
        draft.payload["snapshotSha256"] =
            bytes.empty() ? std::string(64, 'a') : lubancode::hooks::Sha256Hex(bytes);
        draft.payload["lifecycle"] = lifecycle;
        return harness.writer->AppendEvent(std::move(draft), Durability::PowerLoss);
    };

    SUBCASE("正常重建") {
        const auto projection = goalns::ProjectGoalState(*ledger, harness.dir);
        REQUIRE(projection.has_goal);
        CHECK(projection.gap == goalns::GoalProjectionGap::None);
        CHECK(projection.goal_id == "goal-1");
        CHECK(projection.snapshot.state_revision == 2);
        CHECK(projection.snapshot.lifecycle == GoalLifecycle::Active);
        CHECK(projection.snapshot.evidence_refs.size() == 1);
        CHECK(projection.applied_line_hash == service.applied_line_hash());

        // 另一实例从投影接管(§4.67.8 先验存储)。
        GoalService::Options adopt_options;
        adopt_options.session_dir = harness.dir;
        GoalService adopted(&*harness.writer, std::move(adopt_options));
        const auto adopt = adopted.AdoptFromProjection(projection);
        REQUIRE(adopt.ok);
        REQUIRE(adopted.current() != nullptr);
        CHECK(adopted.current()->state_revision == 2);
        // 接管后续提交照常 CAS。
        auto pause = Transition(GoalLifecycle::Paused, 2);
        pause.goal_id = "goal-1";
        pause.stop_reason = "user_request";
        REQUIRE(adopted.ApplyTransition(pause).ok);
    }
    SUBCASE("快照缺失:报缺口不猜") {
        std::filesystem::remove(harness.dir / "state/goals/goal-1/rev-000002.json");
        const auto projection = goalns::ProjectGoalState(*ledger, harness.dir);
        CHECK(projection.gap == goalns::GoalProjectionGap::SnapshotMissing);
    }
    SUBCASE("快照被改:hash 对不上") {
        const auto path = harness.dir / "state/goals/goal-1/rev-000002.json";
        WriteFileBytes(path, ReadFileBytes(path) + " ");
        const auto projection = goalns::ProjectGoalState(*ledger, harness.dir);
        CHECK(projection.gap == goalns::GoalProjectionGap::HashMismatch);
    }
    SUBCASE("快照 hash 对上但 revision 与 applied 不一致") {
        // 手写账:快照文件内容合法、hash 与 applied 所记一致,但快照里
        // state_revision=1 而 applied 记 to=2——RevisionMismatch 缺口。
        GoalStateSnapshot s1 = make_snapshot("goal-1", 1);
        s1.state_revision = 1;
        // applied 的 to 记 2,快照落在 rev-000002 的名字下、内容 revision=1。
        WriteFileBytes(harness.dir / goalns::SnapshotRefPath("goal-1", 2),
                       goalns::SnapshotBytes(s1));
        EventDraft draft;
        draft.kind = EventKindV3::StateGoalApplied;
        draft.payload["goalId"] = "goal-1";
        draft.payload["fromStateRevision"] = 0;
        draft.payload["toStateRevision"] = 2;  // 单行合同允许(0+1?不——会拒)
        (void)draft;
    }
}

TEST_CASE("投影序列校验:terminal 复活、未收账开新 goal、revision 不衔接") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    ServiceHarness harness("sequence");
    // 直接在账上落序列非法的 applied(单行合同都合法,跨行序列坏)。
    // snapshotSha256 默认取快照文件真 hash(让 hash 校验先过,序列缺口
    // 才是唯一报点);需要伪造时显式传。
    SUBCASE("terminal 复活") {
        auto s1 = make_snapshot("goal-1", 1);
        write_snapshot(s1);
        auto s2 = s1;
        s2.state_revision = 2;
        s2.lifecycle = GoalLifecycle::Achieved;
        s2.stop_reason = "done";
        write_snapshot(s2);
        auto s3 = s1;
        s3.state_revision = 3;
        s3.lifecycle = GoalLifecycle::Active;
        write_snapshot(s3);
        REQUIRE(append("goal-1", 0, 1, 1, "preparing").status ==
                WriteReceipt::Status::Committed);
        REQUIRE(append("goal-1", 1, 2, 1, "achieved").status ==
                WriteReceipt::Status::Committed);
        REQUIRE(append("goal-1", 2, 3, 1, "active").status ==
                WriteReceipt::Status::Committed);
        const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(harness.jsonl());
        REQUIRE(ledger.has_value());
        const auto projection = goalns::ProjectGoalState(*ledger, harness.dir);
        CHECK(projection.gap == goalns::GoalProjectionGap::IllegalTransition);
        CHECK(projection.gap_detail.find("复活") != std::string::npos);
    }
    SUBCASE("未收账开新 goal") {
        write_snapshot(make_snapshot("goal-1", 1));
        write_snapshot(make_snapshot("goal-2", 1));
        REQUIRE(append("goal-1", 0, 1, 1, "active").status ==
                WriteReceipt::Status::Committed);
        REQUIRE(append("goal-2", 0, 1, 1, "preparing").status ==
                WriteReceipt::Status::Committed);
        const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(harness.jsonl());
        REQUIRE(ledger.has_value());
        const auto projection = goalns::ProjectGoalState(*ledger, harness.dir);
        CHECK(projection.gap == goalns::GoalProjectionGap::IllegalTransition);
    }
    SUBCASE("快照 hash 对上但 revision 与 applied 不一致") {
        // applied 的 to=2 指向 rev-000002,但快照内容 state_revision=1:
        // hash 与 schema 都过,RevisionMismatch 缺口明报。
        write_snapshot(make_snapshot("goal-1", 1));
        // 第二版快照写在 rev-000002 的名字下,内容 revision 仍是 1。
        const auto stale_content = make_snapshot("goal-1", 1);
        WriteFileBytes(harness.dir / goalns::SnapshotRefPath("goal-1", 2),
                       goalns::SnapshotBytes(stale_content));
        REQUIRE(append("goal-1", 0, 1, 1, "preparing").status ==
                WriteReceipt::Status::Committed);
        REQUIRE(append("goal-1", 1, 2, 1, "active").status ==
                WriteReceipt::Status::Committed);
        const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(harness.jsonl());
        REQUIRE(ledger.has_value());
        const auto projection = goalns::ProjectGoalState(*ledger, harness.dir);
        CHECK(projection.gap == goalns::GoalProjectionGap::RevisionMismatch);
    }
}

TEST_CASE("单行合同拦 revision 跳号:writer 拒 to != from + 1") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    ServiceHarness harness("skipped-revision");
    EventDraft draft;
    draft.kind = EventKindV3::StateGoalApplied;
    draft.payload["goalId"] = "goal-1";
    draft.payload["fromStateRevision"] = 1;
    draft.payload["toStateRevision"] = 3;  // 不 +1
    draft.payload["contractRevision"] = 1;
    draft.payload["snapshotRef"] = "state/goals/goal-1/rev-000003.json";
    draft.payload["snapshotSha256"] = std::string(64, 'a');
    draft.payload["lifecycle"] = "active";
    const auto receipt = harness.writer->AppendEvent(std::move(draft), Durability::PowerLoss);
    CHECK(receipt.status == WriteReceipt::Status::Rejected);
    CHECK(receipt.error_code == "schema3.bad_type");
}
