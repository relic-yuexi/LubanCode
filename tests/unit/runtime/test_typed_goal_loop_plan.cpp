// goal/loop/plan 的 typed 命令兑现(CommandService::HandleGoalCommand/
// HandleLoopCommand/HandlePlanCommand)。前端只发 ClientCommand,不拼
// slash 字符串——这里钉:goal 六命令的状态机真值折回执、loop 七命令的
// scheduler 翻译、plan.review 的三对匹配与 continued 留痕、空装配的
// 稳定禁用码。

#include <doctest/doctest.h>

#include <string>

#include "runtime/command.hpp"
#include "runtime/command_service.hpp"
#include "runtime/goal_coordinator.hpp"
#include "runtime/goal_types.hpp"
#include "runtime/loop_scheduler.hpp"
#include "runtime/session_runtime.hpp"

using lubancode::runtime::ClientCommand;
using lubancode::runtime::ClientCommandKind;
using lubancode::runtime::CommandService;
using lubancode::runtime::SessionRuntime;
using lubancode::runtime::goal::GoalCoordinator;
using lubancode::runtime::loop::LoopScheduler;

namespace {

GoalCoordinator::Options GoalOn() {
    GoalCoordinator::Options o;
    o.goals_enabled = true;
    return o;
}

ClientCommand Cmd(ClientCommandKind kind) {
    ClientCommand command;
    command.kind = kind;
    return command;
}

// 走到 Active(goal 命令测试的公共底)。
void MakeGoalActive(GoalCoordinator& g) {
    lubancode::runtime::goal::GoalContract c;
    c.objective = "迁移认证层并保持契约测试通过";
    c.criteria.push_back({"c-1", "契约测试全绿", true});
    REQUIRE(g.Create("迁移认证层", "/r", "r", 1000).ok);
    REQUIRE(g.SubmitContract(c, 1100).ok);
}

}  // namespace

TEST_CASE("goal typed:Create/Get/Edit/Pause/Resume/Clear 全路") {
    CommandService service({});  // goal/loop 不吃 Options,空配即可
    GoalCoordinator g(GoalOn());
    MakeGoalActive(g);

    // Create(已有 active goal 的场子先清掉,另立一只验 Create 路)。
    REQUIRE(g.Clear(1150).ok);
    ClientCommand create = Cmd(ClientCommandKind::CreateGoal);
    create.text = "把 CI 修绿并保持十分钟";
    const auto created = service.HandleGoalCommand(create, &g, "/r", 1200);
    CHECK(created.accepted);
    CHECK(created.payload.value("goal_id", std::string()) == "goal-2");

    // Get:结构化全账,不发模型。
    const auto status = service.HandleGoalCommand(Cmd(ClientCommandKind::GetGoal), &g, "/r", 1300);
    CHECK(status.accepted);
    CHECK(status.payload.value("has_goal", false));
    CHECK(status.payload.at("goal").value("id", std::string()) == "goal-2");

    // Edit:expected_revision CAS。
    ClientCommand edit = Cmd(ClientCommandKind::EditGoal);
    edit.text = "把 CI 修绿并加一轮冒烟";
    edit.payload["expected_revision"] = 1;
    const auto edited = service.HandleGoalCommand(edit, &g, "/r", 1400);
    CHECK(edited.accepted);
    edit.payload["expected_revision"] = 1;  // 旧 revision 再来:冲突
    const auto conflict = service.HandleGoalCommand(edit, &g, "/r", 1450);
    CHECK_FALSE(conflict.accepted);
    CHECK(conflict.error_code == lubancode::runtime::goal::kErrGoalRevisionConflict);

    // Pause 幂等,Resume 只收可续态。
    CHECK(service.HandleGoalCommand(Cmd(ClientCommandKind::PauseGoal), &g, "/r", 1500).accepted);
    CHECK(service.HandleGoalCommand(Cmd(ClientCommandKind::PauseGoal), &g, "/r", 1550).accepted);
    CHECK(service.HandleGoalCommand(Cmd(ClientCommandKind::ResumeGoal), &g, "/r", 1600).accepted);

    // Clear:没带 confirm 一律拒,不动账。
    const auto refused = service.HandleGoalCommand(Cmd(ClientCommandKind::ClearGoal), &g, "/r", 1700);
    CHECK_FALSE(refused.accepted);
    CHECK(refused.error_code == "confirmation_required");
    CHECK(g.HasActiveGoal());  // 账没动
    ClientCommand clear = Cmd(ClientCommandKind::ClearGoal);
    clear.payload["confirm"] = true;
    const auto cleared = service.HandleGoalCommand(clear, &g, "/r", 1800);
    CHECK(cleared.accepted);
    CHECK_FALSE(g.HasActiveGoal());
}

TEST_CASE("goal typed:空装配回稳定禁用码,不冒充成功") {
    CommandService service({});
    const auto r = service.HandleGoalCommand(Cmd(ClientCommandKind::GetGoal), nullptr, "/r", 1000);
    CHECK_FALSE(r.accepted);
    CHECK(r.error_code == "goal.disabled");
    // 非 goal 命令误投:invalid_request,不吞。
    GoalCoordinator live(GoalOn());
    const auto wrong = service.HandleGoalCommand(Cmd(ClientCommandKind::StartTurn), &live, "/r", 1000);
    CHECK_FALSE(wrong.accepted);
    CHECK(wrong.error_code == "invalid_request");
}

TEST_CASE("loop typed:七命令翻 scheduler,回执带结构化账") {
    CommandService service({});
    LoopScheduler::Options options;
    options.enabled = true;
    LoopScheduler s(options);

    ClientCommand create = Cmd(ClientCommandKind::CreateLoopTask);
    create.text = "盯 CI,红就报";
    create.payload["interval_ms"] = 300000;
    const auto created = service.HandleLoopCommand(create, &s, "/r", "sess-1", 1000);
    CHECK(created.accepted);
    const std::string task_id = created.payload.value("task_id", std::string());
    CHECK(!task_id.empty());

    const auto list = service.HandleLoopCommand(Cmd(ClientCommandKind::ListLoopTasks), &s, "/r", "sess-1", 1100);
    CHECK(list.accepted);
    REQUIRE(list.payload.at("tasks").is_array());
    REQUIRE(list.payload.at("tasks").size() == 1);
    CHECK(list.payload.at("tasks")[0].value("task_id", std::string()) == task_id);
    CHECK(list.payload.at("tasks")[0].value("interval_ms", 0) == 300000);

    ClientCommand read = Cmd(ClientCommandKind::ReadLoopTask);
    read.value = task_id;
    const auto one = service.HandleLoopCommand(read, &s, "/r", "sess-1", 1200);
    CHECK(one.accepted);
    CHECK(one.payload.at("task").value("task_id", std::string()) == task_id);

    // pause/resume/cancel 带 id 走真路。
    ClientCommand pause = Cmd(ClientCommandKind::PauseLoopTask);
    pause.value = task_id;
    CHECK(service.HandleLoopCommand(pause, &s, "/r", "sess-1", 1300).accepted);
    ClientCommand resume = Cmd(ClientCommandKind::ResumeLoopTask);
    resume.value = task_id;
    CHECK(service.HandleLoopCommand(resume, &s, "/r", "sess-1", 1400).accepted);
    ClientCommand run_now = Cmd(ClientCommandKind::RunLoopTaskNow);
    run_now.value = task_id;
    CHECK(service.HandleLoopCommand(run_now, &s, "/r", "sess-1", 1500).accepted);
    // run 不收 all。
    ClientCommand run_all = Cmd(ClientCommandKind::RunLoopTaskNow);
    run_all.value = "all";
    const auto refused = service.HandleLoopCommand(run_all, &s, "/r", "sess-1", 1600);
    CHECK_FALSE(refused.accepted);
    CHECK(refused.error_code == "loop.invalid_request");
    ClientCommand cancel = Cmd(ClientCommandKind::CancelLoopTask);
    cancel.value = task_id;
    CHECK(service.HandleLoopCommand(cancel, &s, "/r", "sess-1", 1700).accepted);
}

TEST_CASE("loop typed:空装配回稳定禁用码") {
    CommandService service({});
    const auto r = service.HandleLoopCommand(Cmd(ClientCommandKind::ListLoopTasks), nullptr, "/r", "sess", 1000);
    CHECK_FALSE(r.accepted);
    CHECK(r.error_code == "loop.disabled");
}

TEST_CASE("plan typed:review 三对匹配才落账,continued 只留痕") {
    CommandService service({});
    SessionRuntime runtime({});  // 纯内存:sessions_dir 空
    // 造一份计划:RecordPlanDocument 落内存真值。
    lubancode::runtime::PlanDocument plan;
    plan.plan_id = "plan-1";
    plan.revision = 1;
    plan.markdown = "# 计划\n第一步先摸现状。";
    plan.state = lubancode::runtime::PlanReviewState::Presented;
    plan.content_sha256 = lubancode::runtime::ScanProposedPlan("").found
                               ? std::string()
                               : std::string("deadbeef");  // 任意稳定 hash 即可
    runtime.RecordPlanDocument(plan);

    // 三对不匹配:stale_request_id,不落账。
    ClientCommand stale = Cmd(ClientCommandKind::ReviewPlan);
    stale.payload["plan_id"] = "plan-1";
    stale.payload["plan_revision"] = 2;  // 旧 revision
    stale.payload["sha256"] = plan.content_sha256;
    stale.payload["decision"] = "approved_confirm";
    const auto stale_r = service.HandlePlanCommand(stale, &runtime);
    CHECK_FALSE(stale_r.accepted);
    CHECK(stale_r.error_code == "stale_request_id");
    CHECK(runtime.latest_plan()->state == lubancode::runtime::PlanReviewState::Presented);

    // continued:不动账,只回执。
    ClientCommand cont = Cmd(ClientCommandKind::ReviewPlan);
    cont.payload["plan_id"] = plan.plan_id;
    cont.payload["plan_revision"] = plan.revision;
    cont.payload["sha256"] = plan.content_sha256;
    cont.payload["decision"] = "continued";
    const auto cont_r = service.HandlePlanCommand(cont, &runtime);
    CHECK(cont_r.accepted);
    CHECK(runtime.latest_plan()->state == lubancode::runtime::PlanReviewState::Presented);

    // 匹配的批准:落账,回执带执行档。
    ClientCommand approve = Cmd(ClientCommandKind::ReviewPlan);
    approve.payload["plan_id"] = plan.plan_id;
    approve.payload["plan_revision"] = plan.revision;
    approve.payload["sha256"] = plan.content_sha256;
    approve.payload["decision"] = "approved_auto";
    const auto approved = service.HandlePlanCommand(approve, &runtime);
    CHECK(approved.accepted);
    CHECK(approved.payload.value("decision", std::string()) == "approved");
    CHECK(approved.payload.value("permission_mode", std::string()) == "auto");
    CHECK(runtime.latest_plan()->state == lubancode::runtime::PlanReviewState::Approved);

    // ReopenPlanReview:有稿报回执。
    const auto reopen = service.HandlePlanCommand(Cmd(ClientCommandKind::ReopenPlanReview), &runtime);
    CHECK(reopen.accepted);
    CHECK(reopen.payload.value("plan_id", std::string()) == "plan-1");
}

TEST_CASE("plan typed:SetCollaborationMode 切档带 revision,同档重复切非错误") {
    CommandService service({});
    SessionRuntime runtime({});
    ClientCommand to_plan = Cmd(ClientCommandKind::SetCollaborationMode);
    to_plan.value = "plan";
    to_plan.payload["permission_before_plan"] = "confirm";
    const auto first = service.HandlePlanCommand(to_plan, &runtime);
    CHECK(first.accepted);
    CHECK(first.payload.value("switched", false));
    const auto again = service.HandlePlanCommand(to_plan, &runtime);
    CHECK(again.accepted);
    CHECK_FALSE(again.payload.value("switched", true));  // 重复切 false,不是错误
    ClientCommand bad = Cmd(ClientCommandKind::SetCollaborationMode);
    bad.value = "yolo";
    const auto refused = service.HandlePlanCommand(bad, &runtime);
    CHECK_FALSE(refused.accepted);
    CHECK(refused.error_code == "plan.invalid_mode");
}
