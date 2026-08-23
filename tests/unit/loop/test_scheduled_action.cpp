// loop 单遗留(骨架):ScheduledActionResolver 与 schedulable catalog。
// 纯函数钉:catalog 顺序稳定、安全档查表、关键词命中(中英)、模糊正文
// 与空白给 nullopt(保守性:不硬猜,回落发模型)。

#include <doctest/doctest.h>

#include <string>

#include "runtime/scheduled_action.hpp"

using lubancode::runtime::loop::ActionSafety;
using lubancode::runtime::loop::ResolveScheduledAction;
using lubancode::runtime::loop::SafetyOf;
using lubancode::runtime::loop::SchedulableAction;
using lubancode::runtime::loop::SchedulableActionCatalog;

TEST_CASE("catalog:四枚动作,顺序稳定,安全档各就各位") {
    const auto catalog = SchedulableActionCatalog();
    REQUIRE(catalog.size() == 4);
    CHECK(catalog[0].action == SchedulableAction::ReportStatus);
    CHECK(catalog[1].action == SchedulableAction::SummarizeDiff);
    CHECK(catalog[2].action == SchedulableAction::RunTests);
    CHECK(catalog[3].action == SchedulableAction::CheckCi);
    CHECK(SafetyOf(SchedulableAction::ReportStatus) == ActionSafety::LocalOnly);
    CHECK(SafetyOf(SchedulableAction::SummarizeDiff) == ActionSafety::LocalOnly);
    CHECK(SafetyOf(SchedulableAction::RunTests) == ActionSafety::NeedsCommand);
    CHECK(SafetyOf(SchedulableAction::CheckCi) == ActionSafety::NeedsRemote);
    for (const auto& entry : catalog) {
        CHECK(entry.description != nullptr);  // 帮助/诊断用,不许空
    }
}

TEST_CASE("resolver:关键词命中(中英文,大小写不敏感)") {
    CHECK(ResolveScheduledAction("每五分钟跑测试,红就报") == SchedulableAction::RunTests);
    CHECK(ResolveScheduledAction("Run the tests and report") == SchedulableAction::RunTests);
    CHECK(ResolveScheduledAction("定时查 CI 状态") == SchedulableAction::CheckCi);
    CHECK(ResolveScheduledAction("check ci please") == SchedulableAction::CheckCi);
    CHECK(ResolveScheduledAction("每小时汇总 diff 给我") == SchedulableAction::SummarizeDiff);
    CHECK(ResolveScheduledAction("没事就报平安") == SchedulableAction::ReportStatus);
    CHECK(ResolveScheduledAction("status report") == SchedulableAction::ReportStatus);
}

TEST_CASE("resolver:模糊与空白给 nullopt,不硬猜") {
    CHECK(ResolveScheduledAction("") == std::nullopt);
    CHECK(ResolveScheduledAction("   \n\t ") == std::nullopt);
    // 模糊措辞(没明确说做什么):交模型,不冒充理解。
    CHECK(ResolveScheduledAction("继续把手头的活干完") == std::nullopt);
    CHECK(ResolveScheduledAction("帮我看着点") == std::nullopt);
    CHECK(ResolveScheduledAction("随便做点什么") == std::nullopt);
}

TEST_CASE("resolver:多关键词命中按 catalog 序,先到先得") {
    // 同时说了"跑测试"与"报状态":RunTests 在表里先扫(表序即 catalog
    // 意图序——活儿比汇报重要)。
    const auto picked = ResolveScheduledAction("跑测试,完了汇报状态");
    CHECK(picked.has_value());
    CHECK(*picked == SchedulableAction::RunTests);
}
