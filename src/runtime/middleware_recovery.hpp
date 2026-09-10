// hook dispatch 的恢复断点判例(LuaHook 单 P0-B,§7.3):从折叠出的
// HookDispatchView 判"resume 后怎么办"。只读:不执行脚本、不连 MCP
//(§7.3 只读 replay 零脚本执行);已完成的 invocation 不重跑,恢复动作按
// 断点分级。
//
// 断点表(§7.3 原文对齐):
//   输入已排队,hook 未 started          -> 按原输入推进(ProceedFromInput)
//   候选已存、效果未提交                 -> 核验后补提交(ReplayAdoptedEffects)
//   next 已消费、下游结果不明            -> unknown/暂停,不重调(PauseUnknown)
//   下游完成、后置未返回                 -> 保留下游结果(KeepDownstreamResult)
//   handler 返回已保存、效果尚缺         -> 补提交原返回(ResubmitHandlerReturn)
//   最终效果采用、接纳尚缺               -> 依预留身份补交一次(ResubmitFinalEffect)
//   批次部分完成                         -> 只推进待执行项(AdvancePendingOnly)
//   定义或 artifact 缺失/hash 不符       -> 明报缺口(ReportGap)
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/reader.hpp"

namespace lubancode::runtime {

enum class HookResumeAction {
    NothingToDo,          // 空 dispatch(skipped/无账):照常走
    ProceedFromInput,     // 行 1:原输入/接纳身份推进,不重复发行真人回合
    ReplayAdoptedEffects, // 行 2:核验原版本与引用后补提交已采用效果
    PauseUnknown,         // 行 3:下游结果不明 -> unknown/暂停,不重调
    KeepDownstreamResult, // 行 4:下游已完成,按后处理策略收口,不重跑
    ResubmitHandlerReturn,// 行 5:补提交原返回,不重新运行 handler
    ResubmitFinalEffect,  // 行 6:依预留身份补交一次,不重复注入
    AdvancePendingOnly,   // 行 7:恢复原批次,只推进待执行项
    ReportGap,            // 行 8:定义/artifact 缺失,明报缺口
};

struct HookResumePlan {
    HookResumeAction action = HookResumeAction::NothingToDo;
    std::string reason;   // 判例依据(人话,进诊断)
    // 工作版本:链上最后采用的 input.rewrite 候选(has_value = 有采用)。
    // "恢复输入匹配最后合法链"的判据:恢复方拿它与当前输入对表。
    std::optional<nlohmann::json> adopted_working_input;
    // 已完成/待执行的 invocation id(已完成项不重跑的对账底)。
    std::vector<std::string> completed_invocations;
    std::vector<std::string> pending_invocations;  // 未 started 或 running
    // 已采用的 context.append 文本(重复注入的判重底:已在账的不重复注入)。
    std::vector<std::string> adopted_context_appends;
    // 只读合同:恢复不执行任何脚本(Lua/MCP);恒 false,写在这儿给调用方
    // 与测试一个可断言的字段,不靠口头约定。
    bool executes_scripts = false;
};

// 一枚 dispatch 的恢复判例(§7.3 表)。dispatch_view 须来自 FoldHookDispatches
// (纯读折叠);current_definition_hashes 是当前注册表里同 hookId 的定义
// hash 集(空 = 调用方不做 hash 核验;不匹配 -> ReportGap,"不用今天脚本
// 猜昨天产物")。
HookResumePlan PlanHookResume(const trajectory::v3::HookDispatchView& dispatch_view,
                              const std::vector<std::string>& current_definition_hashes = {});

}  // namespace lubancode::runtime
