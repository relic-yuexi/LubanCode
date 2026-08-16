#include "agent/context_budget.hpp"

namespace lubancode::agent {

ContextBudgetPlan BuildContextBudgetPlan(const ContextBudgetInputs& inputs) {
    ContextBudgetPlan plan;
    plan.stable_system = inputs.stable_system_tokens;
    plan.model_instructions = inputs.model_instructions_tokens;
    plan.tool_schemas = inputs.tool_schemas_tokens;
    plan.current_user_turn = inputs.current_user_turn_tokens;
    plan.protected_hot_zone = inputs.protected_hot_zone_tokens;
    plan.requested_output_reserve = inputs.requested_output_reserve_tokens;
    plan.compact_prompt_overhead = inputs.compact_prompt_overhead_tokens;
    plan.protocol_headroom = inputs.protocol_headroom_tokens;

    // 估算误差边只对"估算口径"的输入收账:百分比是 0(实测 token)就不加,
    // 不白扣。
    if (inputs.tokenizer_error_margin_percent > 0) {
        const std::size_t estimated =
            plan.stable_system + plan.model_instructions + plan.tool_schemas + plan.current_user_turn +
            plan.protected_hot_zone + plan.compact_prompt_overhead;
        plan.tokenizer_error_margin = estimated * static_cast<std::size_t>(inputs.tokenizer_error_margin_percent) / 100;
    }

    if (!inputs.window_tokens.has_value()) {
        return plan;  // 窗口未知:不做拦截,三项预算空着,展示层明说
    }
    plan.window = *inputs.window_tokens;
    if (plan.window <= plan.overhead_total()) {
        // 窗口连开销都盖不住:可压缩预算为零(任何历史都装不下),
        // 输入预算同零——调用方据此直接拒绝,不静默截史。
        plan.compactable_history_budget = std::size_t{0};
        plan.compact_call_input_budget = std::size_t{0};
        plan.summary_target_budget = std::size_t{0};
        return plan;
    }
    plan.compactable_history_budget = plan.window - plan.overhead_total();

    // 压缩请求自身的输入预算:窗口 - 输出预留 - 协议余量 - 压缩指令。
    // 热区与当前用户轮在这里不扣——它们只是压缩的对象之一,不是压缩请求
    // 的固定开销(map 分块时自然按内容进来)。
    const std::size_t call_overhead =
        plan.requested_output_reserve + plan.protocol_headroom + plan.compact_prompt_overhead;
    plan.compact_call_input_budget =
        *inputs.window_tokens > call_overhead ? *inputs.window_tokens - call_overhead : std::size_t{0};

    // 摘要产出目标:热区之外的冷区全换成存档后,存档 + 热区 + 当前轮要能
    // 落回"可压缩预算"内;存档目标取剩余预算的一半(公开的经验起点,
    // /context 与调阈值都看得见这一项)。
    const std::size_t after_hot = plan.protected_hot_zone + plan.current_user_turn + plan.stable_system +
                                  plan.model_instructions + plan.tool_schemas;
    plan.summary_target_budget = *plan.compactable_history_budget > after_hot
                                     ? (*plan.compactable_history_budget - after_hot) / 2
                                     : std::size_t{0};
    return plan;
}

}  // namespace lubancode::agent
