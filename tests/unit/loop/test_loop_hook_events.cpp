// /loop 单:loop 生命周期四枚 hook 事件的合同钉子(枚举/规范名/往返/
// matcher 字段/输出能力)。dispatcher 的真执行归 hooks 框架,这里只钉
// 事件面不歪。

#include <doctest/doctest.h>

#include <string>

#include "hooks/events.hpp"

using lubancode::hooks::EventHasMatcherField;
using lubancode::hooks::EventOutputCapabilities;
using lubancode::hooks::HookEvent;
using lubancode::hooks::OutputCapabilities;
using lubancode::hooks::ParseHookEvent;
using lubancode::hooks::ToString;

TEST_CASE("四枚 loop 事件:枚举名往返") {
    for (const std::string name : {"LoopTaskCreate", "LoopTickStart", "LoopTickEnd", "LoopTaskStop"}) {
        HookEvent event{};
        REQUIRE(ParseHookEvent(name, event));
        CHECK(ToString(event) == name);
    }
    // 认不得的键照旧拒。
    HookEvent dummy{};
    CHECK_FALSE(ParseHookEvent("LoopTick", dummy));
    CHECK_FALSE(ParseHookEvent("loop_task_create", dummy));  // 规范名是 PascalCase
}

TEST_CASE("matcher 字段:Create 匹配 source,TickEnd 匹配 outcome,其余无") {
    CHECK(EventHasMatcherField(HookEvent::LoopTaskCreate));
    CHECK(EventHasMatcherField(HookEvent::LoopTickEnd));
    CHECK_FALSE(EventHasMatcherField(HookEvent::LoopTickStart));
    CHECK_FALSE(EventHasMatcherField(HookEvent::LoopTaskStop));
}

TEST_CASE("输出能力:Create 可拦(收紧不扩权),起止/停只给反馈") {
    const auto create = OutputCapabilities(HookEvent::LoopTaskCreate);
    CHECK(create.can_block);              // deny 创建/收紧 interval
    CHECK_FALSE(create.permission_decision);  // 不能扩大权限
    CHECK(create.additional_context);

    for (const HookEvent e : {HookEvent::LoopTickStart, HookEvent::LoopTickEnd,
                              HookEvent::LoopTaskStop}) {
        const auto caps = OutputCapabilities(e);
        CHECK_FALSE(caps.can_block);          // 事实已发生,不倒回去
        CHECK_FALSE(caps.permission_decision);
        CHECK(caps.additional_context);       // 反馈/追加上下文可以
    }
}
