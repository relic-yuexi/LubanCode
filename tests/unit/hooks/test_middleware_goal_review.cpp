// PostTurn/goal.review 内置槽位(轨迹 v3 §4.67 G3/§4.67.8):
//   - DecideGoalReview 纯决定矩阵:停止意图/预算尽/停态 → hold、相关后台
//     任务未收口 → wait、可评 → evaluate;缺键保守(hold),不默认放行;
//   - 内置槽位注册进同一中间件池:PostTurn/goal.review required,发布后
//     dispatch 真跑(内置实现只提出验收工作项,不在 PostTurn 栈里跑模型);
//   - Lua 同名替换:(hookPoint=PostTurn, name=goal.review) 高层定义胜出
//     ——替换的是策略;宿主门槛(DecideGoalReview 的保守缺省/状态提交)
//     不因 overwrite 绕过:goal 收口路仍以宿主侧快照与停止意图为准。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "hooks/middleware.hpp"
#include "hooks/middleware_builtins.hpp"

using namespace lubancode;
using namespace lubancode::hooks::middleware;

namespace {

nlohmann::json ReviewInput(nlohmann::json overrides = nlohmann::json::object()) {
    nlohmann::json input = nlohmann::json{
        {"lifecycle", "active"},
        {"phase", "idle"},
        {"waitTaskRefs", nlohmann::json::array()},
        {"stopRequested", false},
        {"budgetExhausted", false},
    };
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        input[it.key()] = it.value();
    }
    return input;
}

MiddlewareDispatcher MakeDispatcher(std::vector<MiddlewareDefinition> extra = {}) {
    MiddlewarePool pool;
    AddBuiltinRequestSlots(pool);
    AddBuiltinGoalReviewSlot(pool);
    for (auto& def : extra) {
        pool.AddDefinition(std::move(def));
    }
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    return MiddlewareDispatcher(std::move(*published));
}

}  // namespace

TEST_CASE("DecideGoalReview:决定矩阵,缺键保守") {
    // 基线:工作轮收口、无等待、无停止、预算在 → 可排验收。
    CHECK(DecideGoalReview(ReviewInput()).at("decision") == "evaluate");

    // 停止意图优先(§4.67.10:迟到结果不拉起新轮)。
    CHECK(DecideGoalReview(ReviewInput({{"stopRequested", true}})).at("decision") == "hold");
    // 预算尽:连验收请求也不豁免(§4.67.7)。
    CHECK(DecideGoalReview(ReviewInput({{"budgetExhausted", true}})).at("decision") == "hold");
    CHECK(DecideGoalReview(ReviewInput({{"lifecycle", "budget_exhausted"}})).at("decision") ==
          "hold");
    // 停态/终态不排。
    for (const char* lifecycle :
         {"paused", "awaiting_user", "blocked", "suspended_by_policy", "achieved", "cleared"}) {
        CHECK(DecideGoalReview(ReviewInput({{"lifecycle", lifecycle}})).at("decision") == "hold");
    }
    // 相关后台任务未收口:先走等待路径(无关进程不进 waitTaskRefs,不在此列)。
    CHECK(DecideGoalReview(ReviewInput(
                             {{"waitTaskRefs", nlohmann::json::array({"subagent-3"})}}))
              .at("decision") == "wait");
    CHECK(DecideGoalReview(ReviewInput({{"lifecycle", "waiting"}})).at("decision") == "wait");
    // 缺键保守:unknown lifecycle / 非 object 一律 hold,不默认放行。
    CHECK(DecideGoalReview(nlohmann::json::object()).at("decision") == "hold");
    CHECK(DecideGoalReview(nlohmann::json::array()).at("decision") == "hold");
    nlohmann::json partial = nlohmann::json{{"phase", "idle"}};
    CHECK(DecideGoalReview(partial).at("decision") == "hold");
}

TEST_CASE("内置槽位:PostTurn/goal.review required,注册发布后真跑") {
    MiddlewareDefinition def = BuiltinGoalReviewSlot();
    CHECK(def.point == HookPoint::PostTurn);
    CHECK(def.name == std::string(kGoalReviewSlot));
    CHECK(def.required);
    CHECK(def.stage == Stage::Default);

    // 池内注册(与 PreRequest 槽同一只池),发布后 dispatch:evaluate 路
    // 放行链尾(next 消费),决定值回给宿主。
    auto dispatcher = MakeDispatcher();
    DispatchTrigger trigger;
    trigger.input = ReviewInput();
    DispatchOutcome outcome = dispatcher.Dispatch(HookPoint::PostTurn, trigger,
                                                  /*terminal=*/[](const nlohmann::json& in) {
                                                      return in;
                                                  });
    REQUIRE(outcome.Ok());
    CHECK(outcome.value.at("decision") == "evaluate");
    const InvocationRecord* record = outcome.FindRecord("PostTurn/goal.review");
    REQUIRE(record != nullptr);
    CHECK(record->outcome == "completed");
    CHECK(record->next_consumed);

    // wait 路:短路(不消费 next),宿主按决定走等待路径。
    DispatchTrigger waiting_trigger;
    waiting_trigger.input =
        ReviewInput({{"waitTaskRefs", nlohmann::json::array({"subagent-9"})}});
    auto waiting = dispatcher.Dispatch(HookPoint::PostTurn, waiting_trigger, nullptr);
    REQUIRE(waiting.Ok());
    CHECK(waiting.value.at("decision") == "wait");
    record = waiting.FindRecord("PostTurn/goal.review");
    REQUIRE(record != nullptr);
    CHECK_FALSE(record->next_consumed);  // 短路:不在 PostTurn 栈里跑下游
}

TEST_CASE("同名替换:高层 Lua 定义胜出,替换的是策略不是门槛") {
    // 用户层同键定义(goal.review)覆盖 builtin——同键高层胜出(§三)。
    MiddlewareDefinition custom;
    custom.point = HookPoint::PostTurn;
    custom.stage = Stage::Default;
    custom.name = std::string(kGoalReviewSlot);
    custom.layer = SourceLayer::User;
    custom.source_label = "user hooks/goal_policy";
    custom.implementation_ref = "hooks/goal_policy#review";
    custom.builtin = [](const InvocationCtx&, const nlohmann::json& input,
                        NextCall& next) -> std::expected<HandlerReturn, HandlerError> {
        // 策略可替换:例如停态也允许排复核验收。但宿主门槛不在此——
        // goal 收口路吃宿主侧快照,不吞替代实现的输出当状态。
        nlohmann::json decision = input;
        decision["decision"] = "evaluate";
        decision["reason"] = "custom policy";
        if (next.calls() == 0) {
            next();
        }
        return HandlerReturn::Value(decision);
    };

    auto dispatcher = MakeDispatcher({custom});
    const auto& registry = dispatcher.registry();
    bool found_custom = false;
    for (const auto& selected : registry.Selected(HookPoint::PostTurn)) {
        if (selected->name == std::string(kGoalReviewSlot)) {
            found_custom = true;
            CHECK(selected->layer == SourceLayer::User);
        }
    }
    REQUIRE(found_custom);
    CHECK(registry.Overridden().size() == 1);  // builtin 被同名挤下,不进执行计划

    DispatchTrigger trigger;
    trigger.input = ReviewInput();
    auto outcome = dispatcher.Dispatch(HookPoint::PostTurn, trigger, nullptr);
    REQUIRE(outcome.Ok());
    CHECK(outcome.value.at("reason") == "custom policy");

    // 宿主门槛不被绕过:同一份材料问宿主纯函数,停态/停止意图仍 hold
    //(状态提交、预算、取消、证据准入、去重归宿主,§4.67.8)。
    CHECK(DecideGoalReview(ReviewInput({{"lifecycle", "paused"}})).at("decision") == "hold");
    CHECK(DecideGoalReview(ReviewInput({{"stopRequested", true}})).at("decision") == "hold");
}
