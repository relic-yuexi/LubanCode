// 内置 "agent_watch" 工具(《子代理监督器、agent_watch 与停滞恢复设计》
// §九,P1-0):主代理或直接父代理查看子任务监督面的只读窗口,可睡到任务
// 变化。它不承担故障检测——AgentSupervisor 一直在宿主里跑(单子 §9.1),
// 这里只把台账里的进展合同(四本时钟/健康/重试账)折成有界快照给模型。
//
// 铁律(单子 §十六):
//   - 只读:不 cancel、不 message、不起进程、不碰网络;传话走 agent_message,
//     停止走 Agent Dock 的 x。本工具零副作用。
//   - 有界:task_ids 最多 16 只、wait_ms 上限 30 秒、events 每任务至多 50 枚;
//     输出不含 thinking、正文、Secret 与完整工具参数——events 档只给
//     kind/工具名/错误位/流尾旗,diagnostic 档只给计数与稳定码。
//   - lineage:main(caller_task_id=0)可看整棵会话树;子代理只能看自己的
//     直接孩子——越 lineage 偷看兄弟任务一律稳定拒绝,不泄露"有没有这只
//     任务"之外的任何细节。
//   - 等待:WaitForRevision 用台账里的 condition variable,不忙轮询——
//     无变化时这条线程零 CPU 挂到超时/被唤醒。用户输入(介入消息入账)、
//     父取消、session close、ESC 打断都经台账或外部唤醒口提前叫醒。
//   - 工具描述明写"状态变化才再等,不要短周期轮询"——等修订,不是打卡。
//
// 挂载:main 挂 caller_task_id=0 的实例(整棵树 + diagnostic 档);每只
// 子代理的私有表挂绑定自己 task_id 的窄实例(只看直接孩子,无 diagnostic),
// 与 scoped agent/agent_message 同一道资格门(RunTask 第二段)。
#pragma once

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "tools/agent_tool.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

class AgentWatchTool : public Tool {
public:
    // agent_tool:目标台账(空指针 = 运行时不可用,execute 明确报
    // unavailable,不假报已查看)。caller_task_id:0 = main(无限定 +
    // diagnostic 档);非 0 = 某只子代理的窄实例,只能看它的直接孩子。
    explicit AgentWatchTool(AgentTool* agent_tool, int caller_task_id = 0)
        : agent_tool_(agent_tool), caller_task_id_(caller_task_id) {}

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }  // 只读等待,无副作用
    // 声明只读:恢复侧可安全建议重试(单子"Effect class 与恢复策略")。
    EffectClass effect_class() const override { return EffectClass::ReadOnlyLocal; }
    Idempotency idempotency() const override { return Idempotency::Idempotent; }
    Result execute(const nlohmann::json& input) override;
    // 取消旗贯通:等待谓词里查它,ESC/父取消经台账 notify 及时醒。
    Result execute(const nlohmann::json& input, const ToolExecutionContext& context) override;

private:
    AgentTool* agent_tool_;
    int caller_task_id_ = 0;
};

}  // namespace lubancode::tools
