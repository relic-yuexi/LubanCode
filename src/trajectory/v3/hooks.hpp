// hook 事件账(§4.22-4.24):同一套注册/调度合同覆盖内置函数、脚本、
// 进程、插件与远端 handler。dispatch 是挂点触发身份,invocation 是一枚
// handler 在本次触发中的调用身份;hook.completed 只表示 handler 已返回,
// 改写是否采用另记 hook.effects.applied/rejected(候选效果先存、runtime
// 验证后采用,原内容留档)。
//
// 子执行(§4.23):hook 发起的底层调用是宿主派生执行——独立 actionId、
// payload 带 parentActionId/hookInvocationId/来源后端;它不冒充模型新
// 声明的 tool_call,也不向 provider 多发一条同 ID 的最终 tool 结果。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/journal.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::trajectory::v3 {

// handler 注册快照(§4.22):当时匹配的顺序、定义版本与失败处置。
struct HookHandlerSpec {
    std::string hook_id;
    std::string definition_hash;  // resume 不得改读今天的脚本解释昨天(§4.22)
    std::string handler_kind;     // builtin/script/process/plugin/remote
    int definition_order = 0;
    std::string failure_policy;   // block/keep_original/fallback

    nlohmann::json ToJson() const {
        return nlohmann::json::object({{"hookId", hook_id},
                                       {"definitionHash", definition_hash},
                                       {"handlerKind", handler_kind},
                                       {"definitionOrder", definition_order},
                                       {"failurePolicy", failure_policy}});
    }
};

class HookDispatchSession {
public:
    // 挂点触发:hook.dispatch.requested(hookPoint + matchedHandlers 快照)。
    // turn/step/action 继承触发对象身份,未到对应层不虚填;hook 不自行开
    // 真人回合(§4.22)。
    static HookDispatchSession Dispatch(V3Writer& writer, std::string hook_dispatch_id,
                                        std::string hook_point,
                                        std::optional<std::string> turn_id,
                                        std::optional<std::string> step_id,
                                        std::optional<std::string> action_id,
                                        const std::vector<HookHandlerSpec>& handlers,
                                        std::optional<nlohmann::json> input_ref,
                                        Durability durability = Durability::ProcessCrash);

    // 无匹配/禁用/去重/上游停止:hook.skipped(reason)。可在 dispatch 结果
    // 中汇总未匹配项,不为每条无关配置刷一行(§4.22)。
    static WriteReceipt Skip(V3Writer& writer, std::string hook_dispatch_id,
                            std::string hook_point, std::string reason,
                            std::optional<std::string> turn_id,
                            std::optional<std::string> step_id,
                            std::optional<std::string> action_id,
                            Durability durability = Durability::ProcessCrash);

    // 一枚 handler 的调用链(§4.22):started -> completed/failed/cancelled/
    // unknown。handler 返回 deny/rewrite 也算 completed,decision 说明决定,
    // 不混成脚本执行失败。
    WriteReceipt BeginInvocation(V3Writer& writer, std::string hook_invocation_id,
                                 const HookHandlerSpec& spec,
                                 Durability durability = Durability::ProcessCrash);
    WriteReceipt CompleteInvocation(V3Writer& writer,
                                    std::optional<std::string> decision,
                                    std::optional<nlohmann::json> output_ref,
                                    std::optional<std::uint64_t> duration_ms,
                                    Durability durability = Durability::PowerLoss);
    WriteReceipt FailInvocation(V3Writer& writer, std::string error_code,
                                std::optional<std::uint64_t> duration_ms,
                                Durability durability = Durability::PowerLoss);
    WriteReceipt CancelInvocation(V3Writer& writer, std::string reason,
                                  Durability durability = Durability::PowerLoss);
    WriteReceipt MarkInvocationUnknown(V3Writer& writer, std::string reason,
                                       Durability durability = Durability::PowerLoss);

    // 效果采用(§4.22):completed ≠ 改写已采用。候选先存,验证后
    // applied(带输出版本/采用值引用/校验结果)或 rejected(带原因)。
    WriteReceipt ApplyEffect(V3Writer& writer, std::string effect_type,
                             std::optional<nlohmann::json> input_ref,
                             std::optional<nlohmann::json> output_ref,
                             std::optional<nlohmann::json> applied_value_ref,
                             std::optional<std::string> validation,
                             Durability durability = Durability::PowerLoss);
    WriteReceipt RejectEffect(V3Writer& writer, std::string effect_type, std::string reason,
                              Durability durability = Durability::PowerLoss);

    // 子执行(§4.23):宿主派生执行,独立 actionId,payload 带
    // parentActionId/hookInvocationId/backend;完整走工具生命周期账。
    ToolActionSession BeginSubExecution(V3Writer& writer, const ToolIdentity& identity,
                                        std::string backend,
                                        Durability durability = Durability::ProcessCrash) const;

    const std::string& dispatch_id() const { return dispatch_id_; }
    const std::string& hook_point() const { return hook_point_; }
    const std::optional<std::string>& current_invocation_id() const {
        return current_invocation_id_;
    }

private:
    HookDispatchSession(std::string dispatch_id, std::string hook_point,
                        std::optional<std::string> turn_id, std::optional<std::string> step_id,
                        std::optional<std::string> action_id);
    EventDraft BaseDraft(EventKindV3 kind) const;

    std::string dispatch_id_;
    std::string hook_point_;
    std::optional<std::string> turn_id_;
    std::optional<std::string> step_id_;
    std::optional<std::string> action_id_;
    // 当前 invocation(一次一枚;串行合同见 §4.48,并发由调用方各开 session)。
    std::optional<std::string> current_invocation_id_;
    std::optional<std::string> current_hook_id_;
    bool invocation_started_ = false;
    bool invocation_closed_ = false;
};

}  // namespace lubancode::trajectory::v3
