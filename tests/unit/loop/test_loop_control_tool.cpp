// /loop 单第 4 期:loop_control 窄工具(scope 校验、两档动作、schema 门)。

#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "tools/loop_control_tool.hpp"

using lubancode::tools::LoopControlState;
using lubancode::tools::LoopControlTool;

namespace {
struct Fixture {
    std::shared_ptr<LoopControlState> state = std::make_shared<LoopControlState>();
    LoopControlTool tool{state};
};
}  // namespace

TEST_CASE("不在 loop turn:旁路调到明拒") {
    Fixture fx;  // state->task_id 空
    const auto r = fx.tool.execute({{"action", "complete"}, {"task_id", "loop-1"}, {"reason", "好了"}});
    CHECK(r.is_error);
    CHECK(r.error_code == "loop.not_in_loop_turn");
    CHECK(fx.state->complete_requested == false);
}

TEST_CASE("complete:声明完成,账面立旗") {
    Fixture fx;
    fx.state->task_id = "loop-3";
    const auto r = fx.tool.execute(
        {{"action", "complete"}, {"task_id", "loop-3"}, {"reason", "部署已全绿"}});
    CHECK_FALSE(r.is_error);
    CHECK(fx.state->complete_requested);
    CHECK(fx.state->pause_requested == false);
    CHECK(r.content.find("loop-3") != std::string::npos);
}

TEST_CASE("pause:要用户处理,定义保留") {
    Fixture fx;
    fx.state->task_id = "loop-3";
    const auto r = fx.tool.execute(
        {{"action", "pause"}, {"task_id", "loop-3"}, {"reason", "需要用户确认删除策略"}});
    CHECK_FALSE(r.is_error);
    CHECK(fx.state->pause_requested);
    CHECK(fx.state->complete_requested == false);
}

TEST_CASE("scope:只能管当前 task,伪造 id 报 scope error") {
    Fixture fx;
    fx.state->task_id = "loop-3";
    const auto r = fx.tool.execute(
        {{"action", "complete"}, {"task_id", "loop-9"}, {"reason", "停别人的"}});
    CHECK(r.is_error);
    CHECK(r.error_code == "loop.scope");
    CHECK(r.content.find("loop-3") != std::string::npos);  // 人话里带正主
    CHECK(fx.state->complete_requested == false);
}

TEST_CASE("schema:缺项/类型不对/认不得的 action 全拒") {
    Fixture fx;
    fx.state->task_id = "loop-3";
    // 缺 reason。
    auto r = fx.tool.execute({{"action", "complete"}, {"task_id", "loop-3"}});
    CHECK(r.is_error);
    CHECK(r.error_code == "loop.schema_invalid");
    // 空 reason。
    r = fx.tool.execute({{"action", "complete"}, {"task_id", "loop-3"}, {"reason", ""}});
    CHECK(r.is_error);
    // 认不得的 action。
    r = fx.tool.execute({{"action", "delete"}, {"task_id", "loop-3"}, {"reason", "x"}});
    CHECK(r.is_error);
    CHECK(r.error_code == "loop.schema_invalid");
    // 类型不对。
    r = fx.tool.execute({{"action", 1}, {"task_id", "loop-3"}, {"reason", "x"}});
    CHECK(r.is_error);
    // 不是 object。
    r = fx.tool.execute(nlohmann::json::array({"complete"}));
    CHECK(r.is_error);
}

TEST_CASE("工具面:名字/schema/权限档") {
    Fixture fx;
    CHECK(fx.tool.name() == "loop_control");
    CHECK_FALSE(fx.tool.needs_confirm());  // 不改项目,不弹审批
    const auto schema = fx.tool.input_schema();
    CHECK(schema["additionalProperties"] == false);
    CHECK(schema["required"].size() == 3);
    // 动作枚举恰两档。
    bool saw_complete = false;
    bool saw_pause = false;
    for (const auto& v : schema["properties"]["action"]["enum"]) {
        saw_complete = saw_complete || v == "complete";
        saw_pause = saw_pause || v == "pause";
    }
    CHECK(saw_complete);
    CHECK(saw_pause);
}
