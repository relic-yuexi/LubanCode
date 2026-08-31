// 内置 "agent_message" 工具:主模型给运行中子代理传增量要求的窄通道。
// 只当邮差:插话 != 派新任务 != 打断(规格"主代理缺少给运行中子代理
// 主动传话的工具"):
//   - 不新建任务——那是 "agent" 工具的事;
//   - 不打断子代理正在执行的工具——消息进 TaskRecord::inbox,在"当前
//     工具收尾、下一次模型请求发出之前"的安全边界注入;
//   - 不复活终态任务——已结束的一律明确拒收,不改投 main、不悄悄另起
//     一只代理、不回假"已通知"。以后若要让已结束任务续跑,另立
//     agent_followup(明确"会开启新轮次、上下文与权限来源写清"),若要
//     立刻停手,另立 agent_interrupt——都不塞进这把工具。
// 执行只调现有正式入口 AgentTool::SendTaskMessage(),消息落同一本
// TaskRecord::inbox(与用户查看态传话、排队转投共用一份账),不造第二
// 套 mailbox,不经磁盘临时文件,不绕 main queue 一圈。
// 返回可审计 JSON(status/task_id/pending_count);失败分五类:task not
// found / task already finished / not a direct child / empty invalid
// message / agent runtime unavailable。
//
// scoped(P1-1 §一):main 挂的是 caller_task_id=0 的无限定实例(可投任意
// 存活任务,规格 §9.3 末两行)。subagent 挂的是绑定自己 task_id 的窄实例
// ——execute() 先查目标任务的 parent_task_id 是否等于 caller_task_id,不
// 等就稳定拒收(not_child),绝不越级投给非直接孩子的任务(单子 §9.3
// "首版只放直接孩子,最好查,也最少误投")。
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "tools/agent_tool.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

class AgentMessageTool : public Tool {
public:
    // agent_tool:目标台账。空指针 = 运行时不可用(没配子代理通道的入口),
    // execute 明确报 unavailable,不假报已通知。caller_task_id:0 = main
    // (不限定,可投任意存活任务);非 0 = 只挂给某只子代理的窄实例,只能
    // 投它自己的直接孩子。
    explicit AgentMessageTool(AgentTool* agent_tool, int caller_task_id = 0)
        : agent_tool_(agent_tool), caller_task_id_(caller_task_id) {}

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }  // 只入队,无副作用
    Result execute(const nlohmann::json& input) override;

private:
    AgentTool* agent_tool_;
    int caller_task_id_ = 0;
};

}  // namespace lubancode::tools
