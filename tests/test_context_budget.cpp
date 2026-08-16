// ContextBudgetPlan(第四期)的单测:统一预算总账的公式、窗口未知/开销
// 超窗的边界、摘要目标与压缩请求输入预算分账不混(规格"全局 compact
// 预算"与"不能拿...混成一只数")。
#include <doctest/doctest.h>

#include <optional>

#include "agent/context_budget.hpp"

using namespace lubancode;
using agent::BuildContextBudgetPlan;
using agent::ContextBudgetInputs;

TEST_CASE("预算公式:窗口减各项开销,误差边只对估算口径收账") {
    ContextBudgetInputs inputs;
    inputs.window_tokens = 100000;
    inputs.stable_system_tokens = 2000;
    inputs.model_instructions_tokens = 1000;
    inputs.tool_schemas_tokens = 6000;
    inputs.current_user_turn_tokens = 500;
    inputs.protected_hot_zone_tokens = 12000;
    inputs.requested_output_reserve_tokens = 8192;
    inputs.compact_prompt_overhead_tokens = 512;
    inputs.protocol_headroom_tokens = 2048;
    inputs.tokenizer_error_margin_percent = 5;

    const auto plan = BuildContextBudgetPlan(inputs);
    REQUIRE(plan.compactable_history_budget.has_value());
    // 误差边 =(2000+1000+6000+500+12000+512)×5% = 22012×5% = 1100.6 → 1100
    CHECK(plan.tokenizer_error_margin == 1100);
    CHECK(plan.overhead_total() == 2000 + 1000 + 6000 + 500 + 12000 + 8192 + 512 + 2048 + 1100);
    CHECK(*plan.compactable_history_budget == 100000 - plan.overhead_total());
    // 实测口径(百分比 0)不收误差边。
    inputs.tokenizer_error_margin_percent = 0;
    const auto measured = BuildContextBudgetPlan(inputs);
    CHECK(measured.tokenizer_error_margin == 0);
    CHECK(*measured.compactable_history_budget == 100000 - measured.overhead_total());
}

TEST_CASE("两只数分账:压缩请求输入预算 ≠ 摘要产出目标") {
    ContextBudgetInputs inputs;
    inputs.window_tokens = 100000;
    inputs.requested_output_reserve_tokens = 8192;
    inputs.compact_prompt_overhead_tokens = 512;
    inputs.protocol_headroom_tokens = 2048;
    inputs.protected_hot_zone_tokens = 12000;
    inputs.current_user_turn_tokens = 500;

    const auto plan = BuildContextBudgetPlan(inputs);
    REQUIRE(plan.compact_call_input_budget.has_value());
    // 输入预算 = 窗口 - 输出预留 - 协议 - 压缩指令(热区不在压缩请求的
    // 固定开销里)。
    CHECK(*plan.compact_call_input_budget == 100000 - 8192 - 2048 - 512);
    // 摘要目标 = (可压缩预算 -(热区+当前轮))的一半,与输入预算不是一个数。
    CHECK(plan.summary_target_budget > 0);
    CHECK(plan.summary_target_budget != *plan.compact_call_input_budget);
}

TEST_CASE("窗口未知:不做拦截但明细照填;开销超窗:预算归零不翻负") {
    SUBCASE("窗口未知") {
        ContextBudgetInputs inputs;
        inputs.stable_system_tokens = 2000;
        const auto plan = BuildContextBudgetPlan(inputs);
        CHECK(!plan.compactable_history_budget.has_value());
        CHECK(!plan.compact_call_input_budget.has_value());
        CHECK(plan.summary_target_budget == 0);
        CHECK(plan.stable_system == 2000);  // 明细照填,展示层能列出占用
    }
    SUBCASE("开销超窗") {
        ContextBudgetInputs inputs;
        inputs.window_tokens = 4096;
        inputs.requested_output_reserve_tokens = 8192;  // 比窗口还大
        inputs.protocol_headroom_tokens = 2048;
        const auto plan = BuildContextBudgetPlan(inputs);
        REQUIRE(plan.compactable_history_budget.has_value());
        CHECK(*plan.compactable_history_budget == 0);
        REQUIRE(plan.compact_call_input_budget.has_value());
        CHECK(*plan.compact_call_input_budget == 0);
        CHECK(plan.summary_target_budget == 0);
    }
}
