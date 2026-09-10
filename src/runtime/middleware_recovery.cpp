// 恢复判例实现(LuaHook 单 P0-B,§7.3)。
#include "runtime/middleware_recovery.hpp"

#include <algorithm>

namespace lubancode::runtime {

namespace {

bool Contains(const std::vector<std::string>& values, const std::string& needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

}  // namespace

HookResumePlan PlanHookResume(const trajectory::v3::HookDispatchView& dispatch_view,
                              const std::vector<std::string>& current_definition_hashes) {
    using trajectory::v3::HookInvocationView;
    HookResumePlan plan;

    // 行 8 先判:定义 hash 与当前注册表不符 -> 明报缺口,不用今天脚本猜
    // 昨天产物(§7.3;§4.22 resume 不改读今天的脚本)。
    if (!current_definition_hashes.empty()) {
        for (const HookInvocationView& invocation : dispatch_view.invocations) {
            if (!invocation.definition_hash.empty() &&
                !Contains(current_definition_hashes, invocation.definition_hash)) {
                plan.action = HookResumeAction::ReportGap;
                plan.reason = "定义 hash 不符: " + invocation.hook_id + " 记账 " +
                              invocation.definition_hash.substr(0, 12) +
                              ";当前注册表无此定义(§7.3 明报缺口)";
                return plan;
            }
        }
    }

    // 工作版本与已采用 appends 恒随行(恢复输入匹配最后合法链的判据)。
    if (dispatch_view.has_adopted_working_input) {
        plan.adopted_working_input = dispatch_view.adopted_working_input;
    }
    plan.adopted_context_appends = dispatch_view.adopted_context_appends;

    for (const HookInvocationView& invocation : dispatch_view.invocations) {
        if (invocation.status == "completed" || invocation.status == "denied") {
            plan.completed_invocations.push_back(invocation.invocation_id);
        } else {
            plan.pending_invocations.push_back(invocation.invocation_id);
        }
    }

    if (dispatch_view.skipped || !dispatch_view.requested) {
        plan.action = HookResumeAction::NothingToDo;
        plan.reason = dispatch_view.skipped ? "dispatch 整体 skipped(" + dispatch_view.skip_reason + ")"
                                           : "无 dispatch 事实";
        return plan;
    }

    // 行 1:输入已排队,hook 未 started -> 按原输入推进,不重复发行真人回合。
    if (dispatch_view.invocations.empty()) {
        plan.action = HookResumeAction::ProceedFromInput;
        plan.reason = "requested 而无 started(§7.3 行 1):按原输入/接纳身份推进";
        return plan;
    }

    // running 的 invocation:洋葱序里最深的那枚(last started)是崩溃点。
    // 它之前的 running 都是外层帧(自己的 next 还在栈上)。§7.3 行 3/4 按
    // 下游收据是否可查分级。
    std::optional<std::size_t> deepest_running;
    for (std::size_t i = 0; i < dispatch_view.invocations.size(); ++i) {
        if (dispatch_view.invocations[i].status == "running") {
            deepest_running = i;  // 取最后一个(嵌套最深)
        }
    }
    if (deepest_running.has_value()) {
        const HookInvocationView& invocation = dispatch_view.invocations[*deepest_running];
        if (invocation.continuation_consumed) {
            // 它的 next 消费过:下游在它栈里跑。有比它更晚 started 且全部
            // 收口的 invocation = 下游收据在账(行 4:保留下游结果,不重跑);
            // 没有更晚的 = 下游只剩链尾(真发送/真执行),收据不明(行 3)。
            const bool has_later_completed = *deepest_running + 1 < dispatch_view.invocations.size();
            if (has_later_completed) {
                plan.action = HookResumeAction::KeepDownstreamResult;
                plan.reason = "下游 invocation 已完成、外层后置未返回(" + invocation.invocation_id +
                              ",§7.3 行 4):保留原下游结果,按后处理策略收口,不重跑";
            } else {
                plan.action = HookResumeAction::PauseUnknown;
                plan.reason = "next 已消费、下游结果不明(" + invocation.invocation_id +
                              ",§7.3 行 3):查执行收据;无法核实则 unknown/暂停,不重调";
            }
            return plan;
        }
        // started 未返回、next 未消费:本帧没产生下游执行。有候选 = 行 2
        // 补提交;没候选 = 行 7 只推进待执行项。
        if (invocation.outputs_proposed.empty()) {
            plan.action = HookResumeAction::AdvancePendingOnly;
            plan.reason = "started 未返回、无候选(" + invocation.invocation_id +
                          ",§7.3 行 7):只推进待执行项,已完成项不重跑";
            return plan;
        }
        plan.action = HookResumeAction::ReplayAdoptedEffects;
        plan.reason = "候选已存、效果未提交(" + invocation.invocation_id +
                      ",§7.3 行 2):核验原版本与引用后补提交或拒绝";
        return plan;
    }

    // 全部 invocation 已收口。failed:required 失败不伪装成功——按行 5 的
    // 反面处理:明败留在账上,恢复方按失败处置(pause),不重跑(已完成
    // 项不重跑同样覆盖 failed 项)。
    if (dispatch_view.folded_status == "failed") {
        plan.action = HookResumeAction::KeepDownstreamResult;
        plan.reason = "invocation 失败已收口(§7.3):失败事实保留,不重跑;按失败处置收口";
        return plan;
    }
    if (dispatch_view.folded_status == "unknown") {
        plan.action = HookResumeAction::PauseUnknown;
        plan.reason = "终态 unknown(§7.3):结果未知不自动重试副作用";
        return plan;
    }
    if (dispatch_view.folded_status == "cancelled") {
        plan.action = HookResumeAction::NothingToDo;
        plan.reason = "dispatch 已取消(§7.3):取消后只准清理";
        return plan;
    }

    // 行 7:批次部分完成——matched 快照里有、started 从未发生(tool1/2 已
    // 有终态、3/4 从未 started):恢复原批次,只推进待执行项。
    if (dispatch_view.invocations.size() < dispatch_view.matched_handlers.size()) {
        plan.action = HookResumeAction::AdvancePendingOnly;
        plan.reason = "批次部分完成(§7.3 行 7):已完成 hook 不重跑,只推进待执行项";
        return plan;
    }

    // 行 5:handler 返回已保存、效果尚缺——completed 在账,after_next/
    // short_circuit 提案里带的效果清单有尚未 settled 的项:补提交原返回,
    // 不重新运行 handler。
    for (const HookInvocationView& invocation : dispatch_view.invocations) {
        if (invocation.status != "completed") {
            continue;
        }
        for (const auto& [phase, candidate] : invocation.outputs_proposed) {
            if (phase == "before_next" || !candidate.is_object()) {
                continue;
            }
            const auto proposed_effects = candidate.find("effects");
            if (proposed_effects == candidate.end() || !proposed_effects->is_array()) {
                continue;
            }
            for (const auto& proposed : *proposed_effects) {
                const std::string type = proposed.value("type", std::string());
                if (type.empty()) {
                    continue;
                }
                const bool settled = std::any_of(
                    invocation.effects.begin(), invocation.effects.end(),
                    [&](const trajectory::v3::HookEffectView& effect) {
                        return effect.effect_type == type && effect.applied;
                    });
                if (!settled) {
                    plan.action = HookResumeAction::ResubmitHandlerReturn;
                    plan.reason = "handler 返回已保存、效果尚缺(" + invocation.invocation_id + " 的 " +
                                  type + ",§7.3 行 5):补提交原返回,不重新运行";
                    return plan;
                }
            }
        }
    }

    if (dispatch_view.folded_status == "denied") {
        plan.action = HookResumeAction::NothingToDo;
        plan.reason = "业务 deny 已收口:deny 事实保留,不洗成 allow";
        return plan;
    }
    plan.action = HookResumeAction::ResubmitFinalEffect;
    plan.reason = "全部 handler 已返回、效果在账(§7.3 行 6):依预留身份补交一次,"
                  "不重复注入、不重复召回";
    return plan;
}

}  // namespace lubancode::runtime
