// 验收收口编排测试(轨迹 v3 §4.67 G2):真 GoalService + 真 V3Writer +
// 脚本 backend 走全链:
//   - 采证/checkpoint 事实行落链,证据引用入快照;
//   - continue:判词采用与下一轮意图同一笔 applied,恢复可按原
//     workItemId 补队列;
//   - achieved 缺 required 证据(执行模型自称完成、todo 全勾):程序门槛
//     改判 continue,续排补证据;
//   - evaluator 两坏:rejected + evaluator_failed 暂停,不默认 achieved;
//   - 判词不合合同(缺 criterion):一次修复机会,repair 后仍错暂停;
//   - 写盘级证据落地后旧验证证据翻 stale(证据有效期)。
#include <doctest/doctest.h>

#include <atomic>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/assembler.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/goal_evaluation_flow.hpp"
#include "runtime/goal_service.hpp"
#include "runtime/goal_types.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace goalns = lubancode::runtime::goal;
using goalns::GoalCloseoutMaterial;
using goalns::GoalEvaluationFlowOptions;
using goalns::GoalEvaluationInput;
using goalns::GoalEvaluation;
using goalns::GoalEvidence;
using goalns::GoalLifecycle;
using goalns::GoalPhase;
using goalns::GoalService;
using goalns::GoalStateSnapshot;
using goalns::CloseGoalIterationWithEvaluation;
using lubancode::trajectory::v3::EventKindV3;
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

class ScriptBackend : public lubancode::api::Backend {
public:
    std::vector<std::string> replies;
    std::size_t call = 0;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        const std::string text = call < replies.size() ? replies[call++] : "{}";
        on_event(lubancode::api::MessageStart{});
        on_event(lubancode::api::TextDelta{text});
        lubancode::api::MessageDone done;
        done.usage = lubancode::api::Usage{10, 4, 0, 0, 0};
        done.usage_reported = true;
        on_event(done);
        return {};
    }
};

std::filesystem::path FreshDir(const char* tag) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      (std::string("lubancode-goal-flow-") + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::int64_t FixedClock() { return 1700000000000; }

struct FlowHarness {
    std::filesystem::path dir;
    std::optional<V3Writer> writer;
    std::optional<GoalService> service;
    ScriptBackend backend;

    explicit FlowHarness(const char* tag) : dir(FreshDir(tag)) {
        auto started = V3Writer::Start(dir / "s1.jsonl", "20260911-140000-HHHHHH",
                                       "run-000001", "system prompt");
        REQUIRE(started.has_value());
        writer = std::move(*started);
        GoalService::Options options;
        options.session_dir = dir;
        options.clock = &FixedClock;
        service.emplace(&*writer, std::move(options));
    }

    // 建目标(合同带 required criterion c-1 + 产物)并开进执行轮。
    void RunToRunning() {
        GoalStateSnapshot draft;
        draft.objective = "修好 auth 模块;ctest -R auth 全过";
        draft.contract.criteria.push_back({"c-1", "ctest -R auth 全过", true});
        draft.contract.required_artifacts.push_back("auth-report.txt");
        draft.pending_intent = goalns::GoalPendingIntent{"wi-1", 1, "", 1}.ToJson();
        auto created = service->CreateGoal(std::move(draft), nlohmann::json{{"source", "test"}});
        REQUIRE(created.ok);
        auto claimed = service->ClaimPendingIntent(
            "run-000001", created.payload.at("stateRevision"), nlohmann::json{{"source", "test"}});
        REQUIRE(claimed.ok);
        auto began = service->BeginIteration(claimed.payload.at("stateRevision"),
                                             nlohmann::json{{"source", "test"}});
        REQUIRE(began.ok);
    }

    GoalEvidence MakeEvidence(const std::string& id, const std::string& artifact_key_value) {
        GoalEvidence ev;
        ev.id = id;
        ev.goal_id = "goal-1";
        ev.iteration_id = "goal-1/iter-1";
        ev.producer = "run_command";
        ev.facts["command"] = "ctest -R auth";
        ev.facts["exit_code"] = 0;
        if (!artifact_key_value.empty()) ev.facts["artifact"] = artifact_key_value;
        ev.content_sha256 = std::string(64, 'd');
        ev.observed_at_ms = FixedClock();
        ev.fresh = true;
        return ev;
    }

    GoalCloseoutMaterial Material(const std::vector<GoalEvidence>& fresh,
                                  const std::vector<GoalEvidence>& material) {
        GoalCloseoutMaterial m;
        m.parent_turn_id = "turn-000001";
        m.now_ms = FixedClock();
        m.checkpoint.summary = "跑了一轮";
        m.fresh_evidence = fresh;
        m.material_evidence = material;
        return m;
    }

    GoalEvaluationFlowOptions Options() {
        GoalEvaluationFlowOptions options;
        options.model = "eval-model";
        options.provider = "test-provider";
        options.wire = "anthropic";
        options.timeout_secs = 5;
        return options;
    }

    const GoalStateSnapshot* Now() const { return service->current(); }
};

const char* kContinueVerdict = R"({
  "decision": "continue", "summary": "还差产物", "progress": true,
  "criteria": [{"id": "c-1", "status": "fail", "evidence_ids": ["ev-1"], "reason": "报告没写"}],
  "next_action": "补写 auth-report.txt"
})";

// evaluator 判 achieved 但材料缺 required 产物证据——程序门槛改判 continue。
const char* kAchievedVerdict = R"({
  "decision": "achieved", "summary": "全过了", "progress": true,
  "criteria": [{"id": "c-1", "status": "pass", "evidence_ids": ["ev-1"], "reason": "ctest 绿"}],
  "next_action": "无"
})";

const char* kAchievedWithArtifact = R"({
  "decision": "achieved", "summary": "全过了", "progress": true,
  "criteria": [{"id": "c-1", "status": "pass", "evidence_ids": ["ev-2"], "reason": "ctest 绿"}],
  "next_action": "无"
})";

}  // namespace

TEST_CASE("全链 continue:事实行落链、判词与续排意图同一笔 applied") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    FlowHarness harness("continue");
    harness.RunToRunning();
    const GoalEvidence ev = harness.MakeEvidence("ev-1", "auth-report.txt");
    harness.backend.replies = {kContinueVerdict};
    const auto result = CloseGoalIterationWithEvaluation(
        *harness.service, *harness.writer, harness.backend, harness.Options(),
        harness.Material({ev}, {ev}));
    REQUIRE(result.ok);
    CHECK(result.decision == "continue");
    CHECK(result.next_work_item_id == "goal-1/wi-1");
    CHECK(result.usage.input_tokens == 10);  // 评估费用入 goal 账

    const GoalStateSnapshot* snapshot = harness.Now();
    CHECK(snapshot->phase == GoalPhase::Idle);
    CHECK(snapshot->lifecycle == GoalLifecycle::Active);
    REQUIRE(snapshot->applied_evaluation_id.has_value());
    CHECK(*snapshot->applied_evaluation_id == "eval-goal-1/iter-1");
    // 续排意图可认领(下一轮按原 workItemId 补队列)。
    const auto view = goalns::EvaluateGoalWork(*snapshot, "run-000001");
    CHECK(view.claimable);

    // 账面:证据/checkpoint 事实行 + 验收三段 + 验收消息全在链上。
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(harness.dir / "s1.jsonl");
    REQUIRE(ledger.has_value());
    bool saw_evidence = false, saw_checkpoint = false, saw_requested = false,
         saw_completed = false;
    for (const auto& event : ledger->events) {
        if (event.kind == EventKindV3::GoalEvidenceRecorded) {
            saw_evidence = true;
            CHECK(event.payload.at("evidenceId") == "ev-1");
            CHECK(event.payload.at("facts").at("command") == "ctest -R auth");
        }
        if (event.kind == EventKindV3::GoalCheckpointRecorded) saw_checkpoint = true;
        if (event.kind == EventKindV3::GoalEvaluationRequested) saw_requested = true;
        if (event.kind == EventKindV3::GoalEvaluationCompleted) {
            saw_completed = true;
            CHECK(event.payload.at("decision") == "continue");
        }
    }
    CHECK(saw_evidence);
    CHECK(saw_checkpoint);
    CHECK(saw_requested);
    CHECK(saw_completed);
    // 验卷收口。
    const auto report = lubancode::trajectory::v3::VerifyV3File(harness.dir / "s1.jsonl");
    CHECK(report.ok);
}

TEST_CASE("执行模型自称完成、缺 required 证据:achieved 改判 continue") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    FlowHarness harness("overridden");
    harness.RunToRunning();
    // 证据只有 ev-1(facts 不含 auth-report.txt):required 产物缺证据。
    const GoalEvidence ev = harness.MakeEvidence("ev-1", "");
    harness.backend.replies = {kAchievedVerdict};
    const auto result = CloseGoalIterationWithEvaluation(
        *harness.service, *harness.writer, harness.backend, harness.Options(),
        harness.Material({ev}, {ev}));
    REQUIRE(result.ok);
    CHECK(result.decision == "continue");  // 不 achieved
    CHECK(result.overridden_achieved);
    CHECK(result.override_reason.find("auth-report.txt") != std::string::npos);
    // 目标继续活着,续排意图已排(下一步补证据)。
    CHECK(harness.Now()->lifecycle == GoalLifecycle::Active);
    CHECK(result.next_work_item_id == "goal-1/wi-1");
}

TEST_CASE("材料全齐的 achieved:终态封账") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    FlowHarness harness("achieved");
    harness.RunToRunning();
    const GoalEvidence ev = harness.MakeEvidence("ev-2", "auth-report.txt");
    harness.backend.replies = {kAchievedWithArtifact};
    const auto result = CloseGoalIterationWithEvaluation(
        *harness.service, *harness.writer, harness.backend, harness.Options(),
        harness.Material({ev}, {ev}));
    REQUIRE(result.ok);
    CHECK(result.decision == "achieved");
    CHECK(harness.Now()->lifecycle == GoalLifecycle::Achieved);
    CHECK_FALSE(result.overridden_achieved);
}

TEST_CASE("evaluator 两坏:rejected 落链,目标暂停不默认 achieved") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    FlowHarness harness("two-bad");
    harness.RunToRunning();
    const GoalEvidence ev = harness.MakeEvidence("ev-1", "auth-report.txt");
    harness.backend.replies = {"坏的一次", "坏的两次"};
    const auto result = CloseGoalIterationWithEvaluation(
        *harness.service, *harness.writer, harness.backend, harness.Options(),
        harness.Material({ev}, {ev}));
    CHECK(result.decision == "evaluator_failed");
    CHECK(harness.Now()->lifecycle == GoalLifecycle::Paused);
    CHECK(harness.Now()->stop_reason.find("evaluator_failed") != std::string::npos);
    // 无效判词不采用:appliedEvaluationId 不绑定(审计在 rejected 行)。
    CHECK_FALSE(harness.Now()->applied_evaluation_id.has_value());
    // 两坏也花了钱:失败路的逐次累计 usage 随 evaluator_failed 收口入 goal
    // 账(§4.67.10"每次 usage 各记";丢掉就是漏记)。两轮各 input 10。
    CHECK(result.usage.input_tokens == 20);
    CHECK(result.usage.request_count == 2);
    CHECK(harness.Now()->usage.input_tokens == 20);
    CHECK(harness.Now()->usage.request_count == 2);

    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(harness.dir / "s1.jsonl");
    REQUIRE(ledger.has_value());
    bool saw_rejected = false, saw_completed = false;
    for (const auto& event : ledger->events) {
        if (event.kind == EventKindV3::GoalEvaluationRejected) saw_rejected = true;
        if (event.kind == EventKindV3::GoalEvaluationCompleted) saw_completed = true;
    }
    CHECK(saw_rejected);
    CHECK_FALSE(saw_completed);
}

TEST_CASE("验收前预算闸:token 帽已尽时验收请求也不发,落 budget_exhausted 可恢复") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    // 带帽干净场:帽 50,开轮后先花 55(实报)——EvaluateBudget 判
    // exhausted,连验收请求也不能豁免(§4.67.7)。
    FlowHarness gated("budget-gate");
    {
        GoalStateSnapshot draft;
        draft.objective = "预算受限的目标";
        draft.contract.criteria.push_back({"c-1", "ctest 全过", true});
        draft.budget.max_total_tokens = 50;
        draft.pending_intent = goalns::GoalPendingIntent{"wi-1", 1, "", 1}.ToJson();
        auto created = gated.service->CreateGoal(std::move(draft), nlohmann::json{{"source", "test"}});
        REQUIRE(created.ok);
        REQUIRE(gated.service
                    ->ClaimPendingIntent("run-000001", created.payload.at("stateRevision"),
                                         nlohmann::json{{"source", "test"}})
                    .ok);
        REQUIRE(gated.service
                    ->BeginIteration(gated.service->current()->state_revision,
                                     nlohmann::json{{"source", "test"}})
                    .ok);
    }
    goalns::GoalUsage overspent;
    overspent.input_tokens = 55;
    overspent.request_count = 1;
    overspent.usage_reported = true;
    REQUIRE(gated.service
                ->RecordGoalUsage("subagent-9", "subagent", overspent,
                                  gated.service->current()->state_revision,
                                  nlohmann::json{{"source", "test"}})
                .ok);
    gated.backend.replies = {kContinueVerdict};
    const auto result = CloseGoalIterationWithEvaluation(
        *gated.service, *gated.writer, gated.backend, gated.Options(),
        gated.Material({}, {}));
    CHECK(result.ok);
    CHECK(result.decision == "budget_exhausted");
    CHECK(gated.backend.call == 0);  // 验收请求一个都没发
    CHECK(gated.Now()->lifecycle == GoalLifecycle::BudgetExhausted);
    CHECK(gated.Now()->phase == GoalPhase::Idle);
    CHECK(gated.Now()->pending_intent.empty());  // 工作项销账,不滞留认领
    CHECK(gated.Now()->stop_reason.find("budget_exhausted") == 0);
    CHECK(gated.Now()->usage.input_tokens == 55);  // 旧费用保留
    // 恢复路:显式加预算后转回 active,工作面可再排(命令面 resume)。
    goalns::GoalBudgetAddition addition;
    addition.total_tokens = 500;
    REQUIRE(gated.service
                ->AddBudget(addition, gated.service->current()->state_revision,
                            nlohmann::json{{"source", "test"}})
                .ok);
    goalns::GoalTransitionCandidate back;
    back.goal_id = gated.Now()->goal_id;
    back.expected_state_revision = gated.Now()->state_revision;
    back.to_lifecycle = GoalLifecycle::Active;
    back.to_phase = GoalPhase::Idle;
    REQUIRE(gated.service->ApplyTransition(back).ok);
    const auto view = goalns::EvaluateGoalWork(*gated.Now(), "run-000001");
    CHECK_FALSE(view.claimable);  // 意图已销账:resume 命令面的补排段接管
}

TEST_CASE("判词缺 criterion:一次修复后仍错,暂停") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    FlowHarness harness("missing-criterion");
    harness.RunToRunning();
    const GoalEvidence ev = harness.MakeEvidence("ev-1", "auth-report.txt");
    // 两轮都缺 criterion c-1 的判词:严格校验两坏 → evaluator_failed。
    const char* missing = R"({"decision":"continue","summary":"s","progress":true,
        "criteria":[],"next_action":"补"})";
    harness.backend.replies = {missing, missing};
    const auto result = CloseGoalIterationWithEvaluation(
        *harness.service, *harness.writer, harness.backend, harness.Options(),
        harness.Material({ev}, {ev}));
    CHECK(result.decision == "evaluator_failed");
    CHECK(harness.Now()->lifecycle == GoalLifecycle::Paused);
}

TEST_CASE("修复一次后成:判词被采用,费用两轮各记") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    FlowHarness harness("repair-ok");
    harness.RunToRunning();
    const GoalEvidence ev = harness.MakeEvidence("ev-1", "auth-report.txt");
    const char* missing = R"({"decision":"continue","summary":"s","progress":true,
        "criteria":[],"next_action":"补"})";
    harness.backend.replies = {missing, kContinueVerdict};
    const auto result = CloseGoalIterationWithEvaluation(
        *harness.service, *harness.writer, harness.backend, harness.Options(),
        harness.Material({ev}, {ev}));
    REQUIRE(result.ok);
    CHECK(result.decision == "continue");
    CHECK(result.usage.request_count == 2);
    CHECK(result.usage.input_tokens == 20);  // 两轮各 10,累计
    REQUIRE(harness.Now()->applied_evaluation_id.has_value());
}

TEST_CASE("不在执行轮:编排拒,不动状态") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    FlowHarness harness("not-running");
    // 没开轮就收口:phase=idle,编排拒绝。
    GoalStateSnapshot draft;
    draft.objective = "目标";
    draft.contract.criteria.push_back({"c-1", "验", true});
    auto created = harness.service->CreateGoal(std::move(draft), nlohmann::json{{"source", "test"}});
    REQUIRE(created.ok);
    const auto result = CloseGoalIterationWithEvaluation(
        *harness.service, *harness.writer, harness.backend, harness.Options(),
        harness.Material({}, {}));
    CHECK_FALSE(result.ok);
    CHECK(result.error_code == goalns::kErrGoalCandidateInvalid);
    CHECK(harness.Now()->phase == GoalPhase::Idle);
}
