// workflow 执行器装配根(骨架拆解反弹·问题 3):BuildWorkflowExecutors
// 自 commands/workflow_commands.cpp 搬来——装配 AgentProfile 解析、backend、
// registry、event sink、自定义 agent resolver、子流程 runtime 这类"把零件
// 接起来"的活归 wirings/,命令文件只管调用。材料包
// (WorkflowCommandContext/WorkflowExecutorContext)仍由命令层拼,形状住
// workflow_commands.hpp,这里只接。
#pragma once

#include <map>
#include <memory>
#include <string>

#include "app/commands/workflow_commands.hpp"  // 材料包(装配的入参)

namespace lubancode::app {

// 拼执行器表(transform/template/tool/agent/llm/skill/interaction/subflow)。
// wf_catalog_root 是 catalog 锚点(project_root/user_root),prompt 相对路径
// 按 id 对应条目的目录读。
std::map<lubancode::workflow::NodeKind, std::shared_ptr<lubancode::workflow::NodeExecutor>>
BuildWorkflowExecutors(const WorkflowCommandContext& wf_ctx, const WorkflowExecutorContext& exec_ctx,
                       const std::string& workflow_id);

}  // namespace lubancode::app
