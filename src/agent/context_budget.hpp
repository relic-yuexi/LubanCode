// ContextBudgetPlan(第四期):全局 compact 的统一预算总账。
//
// 现状的缺口(规格"全局 compact 预算"):CompactBudget 只扣输出预留与协议
// 头,摘要指令、工具声明、tokenizer 估算误差都没入账,也没有把"压缩后的
// 目标大小"和"压缩请求本身装得下多少"分开。这份总账按规格公式把每一项
// 摆开,/context 展示的就是同一份——预算看得见,不再各自为政。
//
// 公式(规格原文):
//   window
//   - stable_system            稳定 system 段
//   - model_instructions       模型目录指令段
//   - tool_schemas             工具声明
//   - current_user_turn        当前用户轮
//   - protected_hot_zone       热区(压缩时保住的最近轮)
//   - requested_output_reserve 输出预留(声明值或公开兜底)
//   - compact_prompt_overhead  压缩指令自身(摘要 prompt)
//   - protocol_headroom        协议与安全余量
//   - tokenizer_error_margin   估算口径误差边(按百分比)
//   = compactable_history_budget
//
// 另算两项,不许混成一只数:
//   summary_target_budget    摘要产出目标(压到多大)
//   compact_call_input_budget 一次压缩请求自身装得下多少输入
//
// 预算来源的档位由调用方定(provider token counting > 本地 tokenizer >
// UTF-8 估算 + 公开误差边);这里只管算术,不碰 IO。
#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace lubancode::agent {

// 总账的输入(全部按统一 token 口径估好再进来;tokenizer_error_margin_
// percent 声明"这些数是估算"的公开误差边,UTF-8 估算档默认 5)。
struct ContextBudgetInputs {
    std::optional<std::size_t> window_tokens;       // 有效窗口(nullopt = 未知,不做拦截)
    std::size_t stable_system_tokens = 0;           // 稳定 system 段
    std::size_t model_instructions_tokens = 0;      // 模型目录指令段
    std::size_t tool_schemas_tokens = 0;            // 工具声明(tools 数组)
    std::size_t current_user_turn_tokens = 0;       // 当前用户轮
    std::size_t protected_hot_zone_tokens = 0;      // 热区预算(kDefaultHotZoneTokens)
    std::size_t requested_output_reserve_tokens = 0;  // 输出预留(reserve_for_estimate)
    std::size_t compact_prompt_overhead_tokens = 0;   // 压缩指令自身
    std::size_t protocol_headroom_tokens = 2048;      // 协议余量(CompactBudget 同款)
    int tokenizer_error_margin_percent = 5;           // 估算误差边(实测 token 口径给 0)
};

struct ContextBudgetPlan {
    // 可压缩历史预算:nullopt = 窗口未知(不做拦截,但 /context 要明说
    // "未按窗口校验",不假装核过)。
    std::optional<std::size_t> compactable_history_budget;
    // 摘要产出目标:压完之后存档+热区希望落到的规模。与压缩请求自己的
    // 输入预算是两回事(规格"不能拿...混成一只数")。
    std::size_t summary_target_budget = 0;
    // 一次压缩请求(含指令)自身装得下的输入预算:map 分块按它切。
    std::optional<std::size_t> compact_call_input_budget;
    // 扣掉的各项明细(/context 展示用;单位 token)。
    std::size_t window = 0;
    std::size_t stable_system = 0;
    std::size_t model_instructions = 0;
    std::size_t tool_schemas = 0;
    std::size_t current_user_turn = 0;
    std::size_t protected_hot_zone = 0;
    std::size_t requested_output_reserve = 0;
    std::size_t compact_prompt_overhead = 0;
    std::size_t protocol_headroom = 0;
    std::size_t tokenizer_error_margin = 0;
    std::size_t overhead_total() const {
        return stable_system + model_instructions + tool_schemas + current_user_turn + protected_hot_zone +
               requested_output_reserve + compact_prompt_overhead + protocol_headroom + tokenizer_error_margin;
    }
};

// 纯函数,单测钉:按公式算总账。窗口未知时三个预算全给 nullopt/0,明细照
// 填(展示层仍能列出各占用)。
ContextBudgetPlan BuildContextBudgetPlan(const ContextBudgetInputs& inputs);

}  // namespace lubancode::agent
