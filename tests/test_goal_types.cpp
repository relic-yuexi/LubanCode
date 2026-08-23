// /goal 单第 0/1 期:领域类型与状态机转换表。
// 纯函数钉死:枚举往返、objective 计数(4000 characters 的准确口径)、
// 每条合法转换、每条非法转换、pause 分路、terminal 判定、序列化 roundtrip。

#include <doctest/doctest.h>

#include <string>

#include "runtime/goal_types.hpp"

using lubancode::runtime::goal::CheckpointStatus;
using lubancode::runtime::goal::CountGoalObjectiveChars;
using lubancode::runtime::goal::GoalBudget;
using lubancode::runtime::goal::GoalCheckpoint;
using lubancode::runtime::goal::GoalContract;
using lubancode::runtime::goal::GoalDecision;
using lubancode::runtime::goal::GoalEvaluation;
using lubancode::runtime::goal::GoalEvidence;
using lubancode::runtime::goal::GoalIteration;
using lubancode::runtime::goal::GoalIterationPhase;
using lubancode::runtime::goal::GoalState;
using lubancode::runtime::goal::GoalTask;
using lubancode::runtime::goal::GoalUsage;
using lubancode::runtime::goal::IsGoalResumable;
using lubancode::runtime::goal::IsGoalTerminal;
using lubancode::runtime::goal::PauseTransition;
using lubancode::runtime::goal::ParseGoalState;
using lubancode::runtime::goal::ToString;
using lubancode::runtime::goal::ValidateGoalObjective;
using lubancode::runtime::goal::kGoalObjectiveMaxChars;

TEST_CASE("GoalState:枚举往返,线上字符串稳定") {
    const GoalState all[] = {
        GoalState::Preparing,     GoalState::Active,          GoalState::Running,
        GoalState::Evaluating,    GoalState::Pausing,         GoalState::Paused,
        GoalState::AwaitingApproval, GoalState::AwaitingUser, GoalState::Blocked,
        GoalState::Achieved,      GoalState::BudgetExhausted, GoalState::SuspendedByPolicy,
        GoalState::Failed,        GoalState::Cleared,
    };
    for (const GoalState s : all) {
        const std::string text = ToString(s);
        GoalState back = GoalState::Preparing;
        CHECK(ParseGoalState(text, back));
        CHECK(back == s);
    }
    GoalState dummy = GoalState::Active;
    CHECK_FALSE(ParseGoalState("nonsense", dummy));
    CHECK(ToString(GoalState::AwaitingApproval) == "awaiting_approval");
    CHECK(ToString(GoalState::SuspendedByPolicy) == "suspended_by_policy");
}

TEST_CASE("terminal 判定:后五态终态,resume 只收三态") {
    CHECK(IsGoalTerminal(GoalState::Achieved));
    CHECK(IsGoalTerminal(GoalState::BudgetExhausted));
    CHECK(IsGoalTerminal(GoalState::SuspendedByPolicy));
    CHECK(IsGoalTerminal(GoalState::Failed));
    CHECK(IsGoalTerminal(GoalState::Cleared));
    CHECK_FALSE(IsGoalTerminal(GoalState::Paused));
    CHECK_FALSE(IsGoalTerminal(GoalState::Running));

    CHECK(IsGoalResumable(GoalState::Paused));
    CHECK(IsGoalResumable(GoalState::AwaitingUser));
    CHECK(IsGoalResumable(GoalState::Blocked));
    CHECK_FALSE(IsGoalResumable(GoalState::Active));
    CHECK_FALSE(IsGoalResumable(GoalState::Achieved));  // Achieved 要新建,不复活
    CHECK_FALSE(IsGoalResumable(GoalState::Cleared));
}

TEST_CASE("objective 计数:按码点,不拿 bytes 冒充") {
    // ASCII:1 字节 1 码点。
    CHECK(CountGoalObjectiveChars("abc") == 3);
    // 中文:3 字节 1 码点。
    CHECK(CountGoalObjectiveChars("迁移") == 2);
    // emoji(4 字节)算 1 个 character。
    CHECK(CountGoalObjectiveChars("🔥") == 1);
    // 组合字符按码点各自计数(Unicode scalar value 口径)。
    CHECK(CountGoalObjectiveChars("é") == 2);  // e + U+0301
    // 换行收:各算各。
    CHECK(CountGoalObjectiveChars("a\nb") == 3);
    // 非法首字节按 1 前进,不死循环。
    CHECK(CountGoalObjectiveChars(std::string("\xff\xfe")) == 2);
}

TEST_CASE("objective 校验:空/全空白/恰 4000/4001/CRLF") {
    CHECK(ValidateGoalObjective("") == lubancode::runtime::goal::kErrGoalObjectiveEmpty);
    CHECK(ValidateGoalObjective("   \n\t ") == lubancode::runtime::goal::kErrGoalObjectiveEmpty);
    CHECK(ValidateGoalObjective("迁移认证层").empty());
    CHECK(ValidateGoalObjective("  迁移认证层  ").empty());  // trim 后非空即收

    // 恰 4000(码点)过线,4001 拒。
    std::string exactly(kGoalObjectiveMaxChars, 'x');
    CHECK(CountGoalObjectiveChars(exactly) == kGoalObjectiveMaxChars);
    CHECK(ValidateGoalObjective(exactly).empty());
    exactly += "x";
    CHECK(ValidateGoalObjective(exactly) == lubancode::runtime::goal::kErrGoalObjectiveTooLong);

    // 中文 4000 码点 = 12000 字节,仍算 4000 characters(不拿 bytes 冒充)。
    std::string zh;
    zh.reserve(kGoalObjectiveMaxChars * 3);
    for (std::size_t i = 0; i < kGoalObjectiveMaxChars; ++i) zh += "迁";
    CHECK(CountGoalObjectiveChars(zh) == kGoalObjectiveMaxChars);
    CHECK(ValidateGoalObjective(zh).empty());
    zh += "迁";
    CHECK(ValidateGoalObjective(zh) == lubancode::runtime::goal::kErrGoalObjectiveTooLong);

    // CRLF 归一:正文里的 \r\n 不截断计数。
    CHECK(ValidateGoalObjective("行一\r\n行二").empty());
}

TEST_CASE("转换表:主链每条走到,非法转换拒") {
    CHECK(lubancode::runtime::goal::IsValidTransition(GoalState::Preparing, GoalState::Active));
    CHECK(lubancode::runtime::goal::IsValidTransition(GoalState::Active, GoalState::Running));
    CHECK(lubancode::runtime::goal::IsValidTransition(GoalState::Running, GoalState::Evaluating));
    CHECK(lubancode::runtime::goal::IsValidTransition(GoalState::Evaluating, GoalState::Active));
    CHECK(lubancode::runtime::goal::IsValidTransition(GoalState::Evaluating, GoalState::Achieved));
    CHECK(lubancode::runtime::goal::IsValidTransition(GoalState::Evaluating, GoalState::Blocked));
    CHECK(lubancode::runtime::goal::IsValidTransition(GoalState::Evaluating, GoalState::BudgetExhausted));
    CHECK(lubancode::runtime::goal::IsValidTransition(GoalState::Evaluating, GoalState::AwaitingUser));
    CHECK(lubancode::runtime::goal::IsValidTransition(GoalState::Paused, GoalState::Active));
    CHECK(lubancode::runtime::goal::IsValidTransition(GoalState::Running, GoalState::Pausing));

    // 非法:跳链与越权。
    CHECK_FALSE(lubancode::runtime::goal::IsValidTransition(GoalState::Preparing, GoalState::Running));
    CHECK_FALSE(lubancode::runtime::goal::IsValidTransition(GoalState::Active, GoalState::Achieved));
    CHECK_FALSE(lubancode::runtime::goal::IsValidTransition(GoalState::Paused, GoalState::Running));
    CHECK_FALSE(lubancode::runtime::goal::IsValidTransition(GoalState::Active, GoalState::Evaluating));
    // terminal 一律不可再动。
    for (const GoalState terminal : {GoalState::Achieved, GoalState::Failed, GoalState::Cleared,
                                     GoalState::BudgetExhausted, GoalState::SuspendedByPolicy}) {
        CHECK(lubancode::runtime::goal::AllowedTransitions(terminal).empty());
    }
}

TEST_CASE("pause 分路:立刻态直落,跑动态等边界,terminal 拒") {
    const auto immediate = PauseTransition(GoalState::Active);
    CHECK(immediate.allowed);
    CHECK(immediate.immediate);
    CHECK(immediate.target == GoalState::Paused);

    const auto deferred = PauseTransition(GoalState::Running);
    CHECK(deferred.allowed);
    CHECK_FALSE(deferred.immediate);  // 记 pause_requested,下一安全边界收口
    CHECK(deferred.target == GoalState::Pausing);

    const auto evaluating = PauseTransition(GoalState::Evaluating);
    CHECK(evaluating.allowed);
    CHECK_FALSE(evaluating.immediate);  // 首版选"等回但不续"

    const auto idle_again = PauseTransition(GoalState::Paused);
    CHECK(idle_again.allowed);  // 幂等
    CHECK(idle_again.target == GoalState::Paused);

    const auto terminal = PauseTransition(GoalState::Achieved);
    CHECK_FALSE(terminal.allowed);
}

TEST_CASE("checkpoint/decision/iteration 枚举往返") {
    for (const CheckpointStatus s : {CheckpointStatus::Progress, CheckpointStatus::ReadyForEvaluation,
                                     CheckpointStatus::Blocked, CheckpointStatus::NeedsUser}) {
        CheckpointStatus back = CheckpointStatus::Progress;
        CHECK(lubancode::runtime::goal::ParseCheckpointStatus(ToString(s), back));
        CHECK(back == s);
    }
    for (const GoalDecision d : {GoalDecision::Continue, GoalDecision::Achieved,
                                 GoalDecision::Blocked, GoalDecision::NeedsUser}) {
        GoalDecision back = GoalDecision::Continue;
        CHECK(lubancode::runtime::goal::ParseGoalDecision(ToString(d), back));
        CHECK(back == d);
    }
    for (const GoalIterationPhase p : {GoalIterationPhase::Scheduled, GoalIterationPhase::Running,
                                       GoalIterationPhase::Checkpointed, GoalIterationPhase::Evaluating,
                                       GoalIterationPhase::Finished, GoalIterationPhase::Interrupted}) {
        GoalIterationPhase back = GoalIterationPhase::Scheduled;
        CHECK(lubancode::runtime::goal::ParseGoalIterationPhase(ToString(p), back));
        CHECK(back == p);
    }
}

TEST_CASE("序列化 roundtrip:GoalTask 全家桶") {
    GoalTask t;
    t.id = "goal-3";
    t.revision = 2;
    t.objective = "迁移认证层并保持契约测试通过";
    t.objective_sha256 = "abc";
    t.state = GoalState::Running;
    t.contract.objective = t.objective;
    t.contract.in_scope = {"auth/", "tests/auth"};
    t.contract.out_of_scope = {"billing"};
    t.contract.criteria.push_back({"c-1", "契约测试全绿", true});
    t.contract.criteria.push_back({"c-2", "旧路径回滚可行", false});
    t.contract.validation_commands = {"ctest --test-dir build"};
    t.contract_frozen = true;
    t.contract_sha256 = "deadbeef";
    t.budget.max_iterations = 40;
    t.counters.iterations_started = 8;
    t.counters.no_progress_streak = 1;
    t.checkpoint.summary = "修复 token refresh 的 Windows 分支";
    t.checkpoint.completed = {"契约测试跑通"};
    t.checkpoint.remaining = {"重跑 e2e"};
    GoalEvaluation ev;
    ev.id = "eval-5";
    ev.decision = GoalDecision::Continue;
    ev.criteria.push_back({"c-1", "pass", {"ev-1"}, "测试绿"});
    t.last_evaluation = ev;
    t.usage.input_tokens = 100;
    t.usage.usage_reported = true;

    const nlohmann::json j = t.to_json();
    const GoalTask back = GoalTask::from_json(j);
    CHECK(back.id == t.id);
    CHECK(back.revision == 2);
    CHECK(back.state == GoalState::Running);
    CHECK(back.contract_frozen);
    CHECK(back.contract.criteria.size() == 2);
    CHECK(back.contract.criteria[0].id == "c-1");
    CHECK(back.contract.criteria[0].required);
    CHECK(back.contract.criteria[1].required == false);
    CHECK(back.budget.max_iterations.value() == 40);
    CHECK(back.counters.iterations_started == 8);
    CHECK(back.checkpoint.summary == t.checkpoint.summary);
    CHECK(back.last_evaluation.has_value());
    CHECK(back.last_evaluation->decision == GoalDecision::Continue);
    CHECK(back.last_evaluation->criteria[0].evidence_ids.size() == 1);
    CHECK(back.usage.usage_reported);
    CHECK(back.usage.input_tokens == 100);
}

TEST_CASE("序列化:缺字段的旧档按默认收,不崩") {
    const GoalTask back = GoalTask::from_json(nlohmann::json::object());
    CHECK(back.id.empty());
    CHECK(back.state == GoalState::Preparing);
    CHECK_FALSE(back.contract_frozen);
    CHECK(back.budget.max_iterations.has_value() == false);
}

TEST_CASE("GoalUsage::Add 分角色累加,usage_reported 只沾不减") {
    GoalUsage a;
    a.input_tokens = 10;
    a.usage_reported = true;
    GoalUsage b;
    b.output_tokens = 5;
    b.request_count = 2;
    a.Add(b);
    CHECK(a.input_tokens == 10);
    CHECK(a.output_tokens == 5);
    CHECK(a.request_count == 2);
    CHECK(a.usage_reported);  // 一处报过就算报过

    GoalUsage unreported;
    unreported.input_tokens = 0;
    unreported.usage_reported = false;
    a.Add(unreported);
    CHECK(a.usage_reported);
}
