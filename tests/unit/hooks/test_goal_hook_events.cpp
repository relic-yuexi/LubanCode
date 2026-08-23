// goal 单:goal 生命周期六枚 hook 事件的合同钉子(枚举/规范名/往返/
// matcher 字段/输出能力)。dispatcher 的真执行归 hooks 框架,这里只钉
// 事件面不歪。单子边界:Hook 不可直接写 Achieved——输出能力面定死没有
// permission_decision、can_block 恒 false。

#include <doctest/doctest.h>

#include <string>

#include "hooks/events.hpp"

using lubancode::hooks::EventHasMatcherField;
using lubancode::hooks::EventOutputCapabilities;
using lubancode::hooks::HookEvent;
using lubancode::hooks::OutputCapabilities;
using lubancode::hooks::ParseHookEvent;
using lubancode::hooks::ToString;

TEST_CASE("六枚 goal 事件:枚举名往返") {
    for (const std::string name :
         {"GoalCreated", "GoalIterationStart", "GoalIterationEnd", "GoalEvaluated", "GoalPaused",
          "GoalCompleted"}) {
        HookEvent event{};
        REQUIRE(ParseHookEvent(name, event));
        CHECK(ToString(event) == name);
    }
    // 认不得的键照旧拒(规范名是 PascalCase)。
    HookEvent dummy{};
    CHECK_FALSE(ParseHookEvent("GoalCreate", dummy));
    CHECK_FALSE(ParseHookEvent("goal_created", dummy));
    CHECK_FALSE(ParseHookEvent("GoalEvaluate", dummy));
}

TEST_CASE("matcher 字段:Evaluated 匹配 decision,Paused 匹配 reason,iteration 起止无") {
    CHECK(EventHasMatcherField(HookEvent::GoalCreated));
    CHECK(EventHasMatcherField(HookEvent::GoalEvaluated));
    CHECK(EventHasMatcherField(HookEvent::GoalPaused));
    CHECK(EventHasMatcherField(HookEvent::GoalCompleted));
    CHECK_FALSE(EventHasMatcherField(HookEvent::GoalIterationStart));
    CHECK_FALSE(EventHasMatcherField(HookEvent::GoalIterationEnd));
}

TEST_CASE("输出能力:全部只给 additionalContext,不可拦不可裁权") {
    for (const HookEvent e :
         {HookEvent::GoalCreated, HookEvent::GoalIterationStart, HookEvent::GoalIterationEnd,
          HookEvent::GoalEvaluated, HookEvent::GoalPaused, HookEvent::GoalCompleted}) {
        const auto caps = OutputCapabilities(e);
        CHECK_FALSE(caps.can_block);  // 事件描述的是已落账的状态变更,拦不回
        CHECK_FALSE(caps.permission_decision);  // Hook 不可直接写 Achieved
        CHECK(caps.additional_context);         // 补上下文/审计可以
    }
}
