// GoalService 测试(轨迹 v3 §4.67 G2:验收相位与判词采用):
//   - BeginEvaluation:phase running->evaluating、checkpointRef 落快照、
//     evidenceRefs 追加、stale 翻旧(证据有效期)、CAS、停态/终态/相位拒;
//   - CompleteIterationWithEvaluation:continue 与下一轮意图同一快照提交
//     (账上恰一笔 applied)、achieved 终态、blocked/needs_user 带键、
//     evaluator_failed 暂停收口、usage 只增、终态后迟到判词拒;
//   - EvidenceRefFromTrace:v1 采证 -> v3 引用(tool_use_id 回指、facts
//     hash 兜底、fresh/truncated 照搬)。
// G0/G1 面在 test_goal_service.cpp / test_goal_service_g1.cpp 不动。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/goal_service.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace goalns = lubancode::runtime::goal;
using goalns::GoalEvidenceRef;
using goalns::GoalLifecycle;
using goalns::GoalPendingIntent;
using goalns::GoalPhase;
using goalns::GoalService;
using goalns::GoalStateSnapshot;
using goalns::GoalUsage;
using goalns::GoalVerdictKind;
using lubancode::trajectory::v3::Durability;
using lubancode::trajectory::v3::V3Writer;

namespace {

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

std::filesystem::path FreshDir(const char* tag) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      (std::string("lubancode-goal-g2-") + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::int64_t FixedClock() { return 1700000000000; }

struct ServiceHarness {
    std::filesystem::path dir;
    std::optional<V3Writer> writer;
    std::optional<GoalService> service;

    explicit ServiceHarness(const char* tag) : dir(FreshDir(tag)) {
        auto started = V3Writer::Start(dir / "s1.jsonl", "20260911-130000-GGGGGG",
                                       "run-000001", "system prompt");
        REQUIRE(started.has_value());
        writer = std::move(*started);
        GoalService::Options options;
        options.session_dir = dir;
        options.clock = &FixedClock;
        service.emplace(&*writer, std::move(options));
    }

    // 建目标 + 认领 + 开轮(直进执行轮;合同带一枚 required criterion)。
    void RunToRunning(const char* objective = "修好 auth 模块") {
        GoalStateSnapshot draft;
        draft.objective = objective;
        draft.contract.criteria.push_back({"c-1", "ctest -R auth 全过", true});
        draft.pending_intent = GoalPendingIntent{"wi-1", 1, "", 1}.ToJson();
        auto created = service->CreateGoal(std::move(draft), nlohmann::json{});
        REQUIRE(created.ok);
        auto claimed = service->ClaimPendingIntent("run-000001", created.payload.at("stateRevision"),
                                                   nlohmann::json{});
        REQUIRE(claimed.ok);
        auto began = service->BeginIteration(claimed.payload.at("stateRevision"),
                                             nlohmann::json{});
        REQUIRE(began.ok);
        REQUIRE(began.payload.contains("iterationId"));
    }

    GoalEvidenceRef MakeEvidence(const std::string& id, bool fresh = true) {
        GoalEvidenceRef ref;
        ref.id = id;
        ref.kind = "command_exit";
        ref.source = goalns::GoalEvidenceSource::ToolAction;
        ref.source_ref = "action-000001";
        ref.session_id = "20260911-130000-GGGGGG";
        ref.run_id = "run-000001";
        ref.content_sha256 = std::string(64, 'c');
        ref.observed_at_ms = 1700000000000;
        ref.fresh = fresh;
        return ref;
    }

    const GoalStateSnapshot* Now() const { return service->current(); }
};

goalns::EvaluationVerdict ContinueVerdict(const std::string& predecessor) {
    goalns::EvaluationVerdict verdict;
    verdict.evaluation_id = "eval-goal-1/iter-1";
    verdict.kind = GoalVerdictKind::Continue;
    GoalPendingIntent intent;
    intent.work_item_id = "goal-1/wi-1";
    intent.contract_revision = 1;
    intent.predecessor_iteration_id = predecessor;
    intent.continuation_ordinal = 1;
    verdict.next_intent = intent;
    GoalUsage usage;
    usage.input_tokens = 12;
    usage.request_count = 1;
    verdict.usage_addition = usage;
    return verdict;
}

}  // namespace

TEST_CASE("BeginEvaluation:相位、checkpointRef、证据追加、stale 翻旧") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    ServiceHarness harness("begin-eval");

    SUBCASE("先落一枚证据再评估,旧验证证据翻 stale") {
        harness.RunToRunning();
        // 第一轮:落证据 ev-1(command_exit)并收口。
        auto began1 = harness.service->BeginEvaluation(
            harness.Now()->state_revision, "evt-ckpt-1", {harness.MakeEvidence("ev-1")},
            {}, nlohmann::json{});
        REQUIRE(began1.ok);
        auto closed1 = harness.service->CompleteIterationWithEvaluation(
            began1.payload.at("stateRevision"), ContinueVerdict("goal-1/iter-1"),
            nlohmann::json{});
        REQUIRE(closed1.ok);
        // 第二轮:认领新意图、开轮、写盘级证据落地后评估——ev-1 须翻 stale。
        auto claimed = harness.service->ClaimPendingIntent(
            "run-000001", harness.Now()->state_revision, nlohmann::json{});
        REQUIRE(claimed.ok);
        auto began_iter = harness.service->BeginIteration(
            claimed.payload.at("stateRevision"), nlohmann::json{});
        REQUIRE(began_iter.ok);
        auto began2 = harness.service->BeginEvaluation(
            harness.Now()->state_revision, "evt-ckpt-2", {harness.MakeEvidence("ev-2")},
            {"ev-1"}, nlohmann::json{});
        REQUIRE(began2.ok);
        const GoalStateSnapshot* snapshot = harness.Now();
        REQUIRE(snapshot->phase == GoalPhase::Evaluating);
        REQUIRE(snapshot->checkpoint_ref.has_value());
        CHECK(*snapshot->checkpoint_ref == "evt-ckpt-2");
        REQUIRE(snapshot->evidence_refs.size() == 2);
        CHECK(snapshot->evidence_refs[0].id == "ev-1");
        CHECK_FALSE(snapshot->evidence_refs[0].fresh);  // 写盘后旧验证翻旧
        CHECK(snapshot->evidence_refs[1].id == "ev-2");
        CHECK(snapshot->evidence_refs[1].fresh);
        // evaluationId 发号(iterationId 派生,稳定)。
        CHECK(began2.payload.at("evaluationId") == "eval-goal-1/iter-2");
    }

    SUBCASE("不在执行轮拒") {
        EnvGuard inner("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
        ServiceHarness idle("begin-eval-idle");
        idle.RunToRunning();
        auto ended = idle.service->EndIteration(idle.Now()->state_revision,
                                                nlohmann::json{});
        REQUIRE(ended.ok);
        auto began = idle.service->BeginEvaluation(idle.Now()->state_revision, {},
                                                   std::vector<GoalEvidenceRef>{},
                                                   std::vector<std::string>{},
                                                   nlohmann::json{});
        REQUIRE_FALSE(began.ok);
        CHECK(began.error_code == goalns::kErrGoalCandidateInvalid);
    }

    SUBCASE("CAS 冲突拒") {
        harness.RunToRunning();
        auto began = harness.service->BeginEvaluation(
            harness.Now()->state_revision + 999, {}, std::vector<GoalEvidenceRef>{},
            std::vector<std::string>{}, nlohmann::json{});
        REQUIRE_FALSE(began.ok);
        CHECK(began.error_code == goalns::kErrGoalRevisionConflict);
    }
}

TEST_CASE("CompleteIterationWithEvaluation:continue 与下一轮意图同一笔 applied") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    ServiceHarness harness("adopt-continue");
    harness.RunToRunning();
    auto began = harness.service->BeginEvaluation(
        harness.Now()->state_revision, "evt-ckpt-1", {harness.MakeEvidence("ev-1")},
        std::vector<std::string>{}, nlohmann::json{});
    REQUIRE(began.ok);
    const std::uint64_t revision_before = harness.Now()->state_revision;

    auto closed = harness.service->CompleteIterationWithEvaluation(
        began.payload.at("stateRevision"), ContinueVerdict("goal-1/iter-1"),
        nlohmann::json{});
    REQUIRE(closed.ok);
    CHECK(closed.payload.at("verdictKind") == "continue");
    CHECK(closed.payload.at("nextWorkItemId") == "goal-1/wi-1");

    const GoalStateSnapshot* snapshot = harness.Now();
    CHECK(snapshot->state_revision == revision_before + 1);  // 恰好一笔
    CHECK(snapshot->phase == GoalPhase::Idle);
    CHECK(snapshot->lifecycle == GoalLifecycle::Active);
    REQUIRE(snapshot->applied_evaluation_id.has_value());
    CHECK(*snapshot->applied_evaluation_id == "eval-goal-1/iter-1");
    CHECK(snapshot->usage.input_tokens == 12);  // usage 只增入账
    // 续排意图同笔落位:恢复按原 workItemId 补队列(EvaluateGoalWork 认它)。
    const auto view = goalns::EvaluateGoalWork(*snapshot, "run-000001");
    CHECK(view.has_intent);
    CHECK(view.intent.work_item_id == "goal-1/wi-1");

    // 账面:从评估到采用之间恰两笔 applied(BeginEvaluation + 采用)。
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(harness.dir / "s1.jsonl");
    REQUIRE(ledger.has_value());
    int applied_count = 0;
    for (const auto& event : ledger->events) {
        if (event.kind == lubancode::trajectory::v3::EventKindV3::StateGoalApplied) {
            ++applied_count;
        }
    }
    // CreateGoal + Claim + BeginIteration + BeginEvaluation + 采用 = 5 笔。
    CHECK(applied_count == 5);
}

TEST_CASE("CompleteIterationWithEvaluation:分路终态与字段门槛") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");

    SUBCASE("achieved:终态,迟到判词拒") {
        ServiceHarness harness("adopt-achieved");
        harness.RunToRunning();
        auto began = harness.service->BeginEvaluation(
            harness.Now()->state_revision, {}, std::vector<GoalEvidenceRef>{},
            std::vector<std::string>{}, nlohmann::json{});
        REQUIRE(began.ok);
        goalns::EvaluationVerdict verdict;
        verdict.evaluation_id = "eval-goal-1/iter-1";
        verdict.kind = GoalVerdictKind::Achieved;
        auto closed = harness.service->CompleteIterationWithEvaluation(
            began.payload.at("stateRevision"), verdict, nlohmann::json{});
        REQUIRE(closed.ok);
        CHECK(harness.Now()->lifecycle == GoalLifecycle::Achieved);
        CHECK(goalns::IsLifecycleTerminal(harness.Now()->lifecycle));
        // 终态后再来的迟到判词:只留审计,不改账(§4.67.6)。
        goalns::EvaluationVerdict late = ContinueVerdict("goal-1/iter-1");
        auto refused = harness.service->CompleteIterationWithEvaluation(
            harness.Now()->state_revision, late, nlohmann::json{});
        REQUIRE_FALSE(refused.ok);
        CHECK(refused.error_code == goalns::kErrGoalCandidateInvalid);
    }

    SUBCASE("blocked 缺 blockerKey 拒,带键成") {
        ServiceHarness harness("adopt-blocked");
        harness.RunToRunning();
        auto began = harness.service->BeginEvaluation(
            harness.Now()->state_revision, {}, std::vector<GoalEvidenceRef>{},
            std::vector<std::string>{}, nlohmann::json{});
        REQUIRE(began.ok);
        goalns::EvaluationVerdict no_key;
        no_key.evaluation_id = "eval-goal-1/iter-1";
        no_key.kind = GoalVerdictKind::Blocked;
        no_key.stop_reason = "blocked";
        auto refused = harness.service->CompleteIterationWithEvaluation(
            began.payload.at("stateRevision"), no_key, nlohmann::json{});
        REQUIRE_FALSE(refused.ok);
        CHECK(refused.error_code == goalns::kErrGoalCandidateInvalid);

        goalns::EvaluationVerdict with_key = no_key;
        with_key.blocker_key = "missing_credential:DEPLOY_TOKEN";
        auto closed = harness.service->CompleteIterationWithEvaluation(
            began.payload.at("stateRevision"), with_key, nlohmann::json{});
        REQUIRE(closed.ok);
        CHECK(harness.Now()->lifecycle == GoalLifecycle::Blocked);
        CHECK(harness.Now()->blocker_key == "missing_credential:DEPLOY_TOKEN");
    }

    SUBCASE("needs_user 落 awaiting_user,带 pendingQuestion") {
        ServiceHarness harness("adopt-needs-user");
        harness.RunToRunning();
        auto began = harness.service->BeginEvaluation(
            harness.Now()->state_revision, {}, std::vector<GoalEvidenceRef>{},
            std::vector<std::string>{}, nlohmann::json{});
        REQUIRE(began.ok);
        goalns::EvaluationVerdict verdict;
        verdict.evaluation_id = "eval-goal-1/iter-1";
        verdict.kind = GoalVerdictKind::NeedsUser;
        verdict.pending_question = "删库还是归档?";
        verdict.stop_reason = "needs_user";
        auto closed = harness.service->CompleteIterationWithEvaluation(
            began.payload.at("stateRevision"), verdict, nlohmann::json{});
        REQUIRE(closed.ok);
        CHECK(harness.Now()->lifecycle == GoalLifecycle::AwaitingUser);
        CHECK(harness.Now()->pending_question == "删库还是归档?");
    }

    SUBCASE("evaluator_failed:暂停收口,保留停因") {
        ServiceHarness harness("adopt-eval-failed");
        harness.RunToRunning();
        auto began = harness.service->BeginEvaluation(
            harness.Now()->state_revision, {}, std::vector<GoalEvidenceRef>{},
            std::vector<std::string>{}, nlohmann::json{});
        REQUIRE(began.ok);
        goalns::EvaluationVerdict verdict;
        verdict.evaluation_id = "eval-goal-1/iter-1";
        verdict.kind = GoalVerdictKind::EvaluatorFailed;
        verdict.stop_reason = "evaluator_failed: 判词两坏";
        auto closed = harness.service->CompleteIterationWithEvaluation(
            began.payload.at("stateRevision"), verdict, nlohmann::json{});
        REQUIRE(closed.ok);
        CHECK(harness.Now()->lifecycle == GoalLifecycle::Paused);
        CHECK(harness.Now()->stop_reason.find("evaluator_failed") != std::string::npos);
        CHECK(harness.Now()->phase == GoalPhase::Idle);
        // 暂停后:意图不排,恢复核验归显式 resume。
        const auto view = goalns::EvaluateGoalWork(*harness.Now(), "run-000001");
        CHECK_FALSE(view.claimable);
    }

    SUBCASE("continue 缺意图拒(同一快照提交的硬约束)") {
        ServiceHarness harness("adopt-no-intent");
        harness.RunToRunning();
        auto began = harness.service->BeginEvaluation(
            harness.Now()->state_revision, {}, std::vector<GoalEvidenceRef>{},
            std::vector<std::string>{}, nlohmann::json{});
        REQUIRE(began.ok);
        goalns::EvaluationVerdict verdict;
        verdict.evaluation_id = "eval-goal-1/iter-1";
        verdict.kind = GoalVerdictKind::Continue;
        auto refused = harness.service->CompleteIterationWithEvaluation(
            began.payload.at("stateRevision"), verdict, nlohmann::json{});
        REQUIRE_FALSE(refused.ok);
        CHECK(refused.error_code == goalns::kErrGoalCandidateInvalid);
    }
}

TEST_CASE("EvidenceRefFromTrace:tool_use_id 回指、facts hash 兜底、位照搬") {
    goalns::GoalEvidence trace;
    trace.id = "ev-7";
    trace.goal_id = "goal-1";
    trace.iteration_id = "goal-1/iter-2";
    trace.tool_use_id = "action-000009";
    trace.producer = "run_command";
    trace.facts["command"] = "ctest -R auth";
    trace.fresh = false;      // 已翻旧的旧验证证据
    trace.truncated = true;
    const GoalEvidenceRef ref = goalns::EvidenceRefFromTrace(trace, "sess-1", "run-1", "base-1");
    CHECK(ref.id == "ev-7");
    CHECK(ref.source_ref == "action-000009");
    CHECK(ref.session_id == "sess-1");
    CHECK(ref.run_id == "run-1");
    CHECK(ref.workspace_baseline == "base-1");
    CHECK(ref.fresh == false);
    CHECK(ref.truncated == true);
    // content_sha256 缺失:facts canonical 字节的 hash 顶上,不为凑非空造空串。
    CHECK_FALSE(ref.content_sha256.empty());
    CHECK(goalns::ValidateEvidenceRef(ref.ToJson()).empty());

    // tool_use_id 空的(宿主合成):producer 兜底,合同仍过。
    goalns::GoalEvidence synthetic = trace;
    synthetic.tool_use_id.clear();
    const GoalEvidenceRef fallback = goalns::EvidenceRefFromTrace(synthetic, "s", "r", "");
    CHECK(fallback.source_ref == "host:run_command");
    CHECK(goalns::ValidateEvidenceRef(fallback.ToJson()).empty());
}
