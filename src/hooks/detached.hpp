// 后台子代理的 hooks 执行器(外挂会话)。
//
// 线程规矩(dispatcher.hpp 的"后台执行面"契约):
//   - 主线程在起后台任务前造一份本会话:从 dispatcher 拷一份只读策略快照
//     与会话上下文。快照含信任/禁用账——后台线程照快照执行,主会话中途
//     trust/disable 只影响之后新起的会话,不在跑的这份不追改。
//   - 后台线程只调 Emit/PostWarning:真跑钩子(EmitDetached,不碰 dispatcher
//     账本),跑完的记录投进 dispatcher 的外部队列,告警随行。主会话在
//     安全点(轮起/轮收//hooks)AdoptExternalRecords 归并落账、刷 UI。
//   - 同步决策(PreToolUse/PermissionRequest)由这份快照执行给出:deny 照
//     拒、allow 照放;ask 在后台没有终端可问,由调用方(agent_tool)明示
//     降级为拒绝并 PostWarning,不静默绕过。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "hooks/dispatcher.hpp"

namespace lubancode::hooks {

class DetachedHookSession {
public:
    // dispatcher 为空指针或定义表为空时造出来的是空会话(Empty() 为真),
    // 后台路径据此整个跳过,行为与从前一致。
    DetachedHookSession(HookDispatcher* dispatcher, HookContext base_context);

    bool Empty() const { return snapshot_.empty(); }

    // 快照里某事件有没有可能命中(含未信任/禁用的——它们记 skipped)。
    bool HasHandlersFor(HookEvent event) const;

    // 后台线程调用:对快照跑一次事件(匹配/信任/并发/归并与 Emit 同核),
    // 记录投递进 dispatcher 外部队列,不碰账本。返回归并结果。
    HookEventResult Emit(HookEvent event, const HookPayload& payload);

    // 后台线程调用:明示降级这类要给用户看的话,随下一批记录一起被安全点
    // 收走。告警不进 HookRunRecord——记录管钩子,告警管宿主自己的降级决定。
    void PostWarning(const std::string& warning);

    // 子代理身份(agent_id/agent_type/parent_agent_id/turn_id)由 agent_tool
    // 在后台线程里覆写一次;本类不加锁,覆写须在第一次 Emit 之前。
    HookContext& context() { return context_; }
    const HookContext& context() const { return context_; }

private:
    HookDispatcher* dispatcher_ = nullptr;  // 进程级稳定指针(hook_runtime)
    std::vector<HookDefinition> snapshot_;
    HookContext context_;
};

}  // namespace lubancode::hooks
