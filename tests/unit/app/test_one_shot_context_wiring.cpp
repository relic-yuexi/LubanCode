// one-shot 上下文压缩接线(one-shot 无人值守适配):从前 AskOnce 一处没接
// on_context_pressure,loop 的压力通报是空操作,长任务上下文只涨不压。
// 接线抽成 BuildOneShotContextWiring 这只小函数,这里钉两件事:
//   1. 材料在手 → 压力回调非空,每次通报现取一份材料包转进
//      HandleContextPressure(与交互会话同一只);
//   2. 材料缺席 → 不接(空回调),不拿空材料去撞压缩路。
// 不跑真压缩:PreRequest 未撞线、AfterHardTrim 没截东西两相都只看不动,
// 不碰 agent/backend,材料包只需 theme。
#include <doctest/doctest.h>

#include "app/one_shot.hpp"
#include "cli/theme.hpp"

namespace {

using lubancode::agent::ContextPressure;

}  // namespace

TEST_CASE("BuildOneShotContextWiring: 压力回调非空,通报现取材料转进 HandleContextPressure") {
    lubancode::cli::Theme theme;
    int material_calls = 0;
    const lubancode::agent::AgentWiring wiring = lubancode::app::BuildOneShotContextWiring([&]() {
        ++material_calls;
        lubancode::app::CompactSessionInputs in;
        in.theme = &theme;
        return in;
    });
    REQUIRE(static_cast<bool>(wiring.on_context_pressure));

    ContextPressure pre_request;
    pre_request.phase = ContextPressure::Phase::PreRequest;
    pre_request.projected_overflow = false;  // 未撞线:只看不压
    wiring.on_context_pressure(pre_request);
    CHECK(material_calls == 1);

    ContextPressure after_trim;
    after_trim.phase = ContextPressure::Phase::AfterHardTrim;
    after_trim.hard_truncated_results = false;  // 没截东西:不告警
    wiring.on_context_pressure(after_trim);
    CHECK(material_calls == 2);  // 每次通报现取,不缓存旧材料
}

TEST_CASE("BuildOneShotContextWiring: 材料缺席不接,回调为空") {
    const lubancode::agent::AgentWiring wiring = lubancode::app::BuildOneShotContextWiring({});
    CHECK_FALSE(static_cast<bool>(wiring.on_context_pressure));
}
