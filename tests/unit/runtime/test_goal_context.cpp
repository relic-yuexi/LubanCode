// /goal 单:GoalContext 注入与 continuation 消息(纯函数)。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "runtime/goal_context.hpp"

using lubancode::runtime::goal::BuildGoalContext;
using lubancode::runtime::goal::BuildGoalContinuationMessage;
using lubancode::runtime::goal::GoalCheckpoint;
using lubancode::runtime::goal::GoalContract;
using lubancode::runtime::goal::GoalEvaluation;
using lubancode::runtime::goal::GoalEvidence;
using lubancode::runtime::goal::GoalState;
using lubancode::runtime::goal::GoalTask;
using lubancode::runtime::goal::GoalDecision;

namespace {

GoalTask SampleTask() {
    GoalTask t;
    t.id = "goal-3";
    t.revision = 2;
    t.objective = "迁移认证层并保持契约测试通过";
    t.contract.in_scope = {"auth/", "tests/auth"};
    t.contract.out_of_scope = {"billing/"};
    t.contract.criteria.push_back({"c-1", "契约测试全绿", true});
    t.contract.criteria.push_back({"c-2", "旧路径可回滚", false});
    t.contract.constraints = {"不改 billing"};
    t.state = GoalState::Running;
    t.checkpoint.summary = "修复 token refresh 的 Windows 分支";
    t.checkpoint.completed = {"契约测试跑通"};
    t.checkpoint.remaining = {"重跑 e2e"};
    t.checkpoint.validations = {"ctest 退出码 0"};
    t.checkpoint.next_action = "重跑 e2e,再核旧路径回滚";
    t.budget.max_iterations = 40;
    t.counters.iterations_started = 8;
    t.usage.input_tokens = 150000;
    t.usage.output_tokens = 34000;
    t.usage.usage_reported = true;
    GoalEvaluation ev;
    ev.decision = GoalDecision::Continue;
    ev.summary = "4/6 criteria pass;e2e stale";
    t.last_evaluation = ev;
    return t;
}

GoalEvidence SampleEvidence(const std::string& id, bool fresh) {
    GoalEvidence e;
    e.id = id;
    e.goal_id = "goal-3";
    e.producer = "run_command";
    e.facts["exit_code"] = 0;
    e.fresh = fresh;
    return e;
}

}  // namespace

TEST_CASE("GoalContext:合同原样注入,不带八轮旧正文") {
    const GoalTask task = SampleTask();
    const std::string ctx = BuildGoalContext(task, {SampleEvidence("ev-1", true)});
    CHECK(ctx.find("goal-3") != std::string::npos);
    CHECK(ctx.find("revision 2") != std::string::npos);
    CHECK(ctx.find("迁移认证层并保持契约测试通过") != std::string::npos);
    CHECK(ctx.find("+auth/") != std::string::npos);
    CHECK(ctx.find("-billing/") != std::string::npos);
    CHECK(ctx.find("c-1") != std::string::npos);
    CHECK(ctx.find("契约测试全绿") != std::string::npos);
    CHECK(ctx.find("goal_checkpoint") != std::string::npos);  // 工具合同明说
    CHECK(ctx.find("无权宣布目标达成") != std::string::npos);  // 执行模型无权切 state
    const bool injection_warned =
        ctx.find("prompt injection") != std::string::npos || ctx.find("夹带指令") != std::string::npos;
    CHECK(injection_warned);
}

TEST_CASE("GoalContext:checkpoint、evidence 摘要与预算") {
    const GoalTask task = SampleTask();
    const std::string ctx = BuildGoalContext(task, {SampleEvidence("ev-1", true), SampleEvidence("ev-2", false)});
    CHECK(ctx.find("修复 token refresh") != std::string::npos);
    CHECK(ctx.find("todo: 重跑 e2e") != std::string::npos);
    CHECK(ctx.find("verified: ctest 退出码 0") != std::string::npos);
    CHECK(ctx.find("ev-1") != std::string::npos);
    CHECK(ctx.find("stale") != std::string::npos);  // stale 标注
    CHECK(ctx.find("iterations: 8/40") != std::string::npos);
    CHECK(ctx.find("continue") != std::string::npos);  // 上一轮判词
    CHECK(ctx.find("重跑 e2e,再核旧路径回滚") != std::string::npos);  // next_action 建议
}

TEST_CASE("GoalContext:token 未报告写明,不画 0") {
    GoalTask task = SampleTask();
    task.usage = {};  // provider 不报 usage
    const std::string ctx = BuildGoalContext(task, {});
    CHECK(ctx.find("未报告") != std::string::npos);
    CHECK(ctx.find("时间与轮数闸照常收口") != std::string::npos);
}

TEST_CASE("GoalContext:首轮无 checkpoint、evidence 上限") {
    GoalTask task = SampleTask();
    task.checkpoint = GoalCheckpoint{};  // 首轮
    std::vector<GoalEvidence> many;
    for (int i = 0; i < 20; ++i) many.push_back(SampleEvidence("ev-" + std::to_string(i), true));
    const std::string ctx = BuildGoalContext(task, many);
    CHECK(ctx.find("还没有 checkpoint") != std::string::npos);
    CHECK(ctx.find("ev-7") != std::string::npos);   // 前 8 枚带
    CHECK(ctx.find("ev-15") == std::string::npos);  // 上限外的没带
}

TEST_CASE("continuation 消息:带 goal id/revision/iteration 与工具指引") {
    const GoalTask task = SampleTask();
    const std::string msg = BuildGoalContinuationMessage(task, 9);
    CHECK(msg.find("goal-3") != std::string::npos);
    CHECK(msg.find("r2") != std::string::npos);
    CHECK(msg.find("iteration 9") != std::string::npos);
    CHECK(msg.find("goal_checkpoint") != std::string::npos);
    CHECK(msg.find("ready_for_evaluation") != std::string::npos);
}
