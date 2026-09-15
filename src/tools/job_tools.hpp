// job_handle 模式的模型可见工具族(异步工具单 P2;§8 拟定接口落名):
// job_get / job_wait / job_cancel 三枚,把 ToolJobCoordinator 的宿主侧
// 接口递给模型。start 不单设——异步能力的普通工具调用由批次闸门裁决
// 接单(接单结果即配那枚调用),模型不感知协调器。
//
// 访问控制:jobId 不是访问凭证(单 §8)——三枚工具都经协调器的授权
// 闸门(按目标 job 的工具名+入参复核),不是按调用方身份放行。工具
// 自身 ApprovalClass::None:问询/取消不弹确认,被拒结果如实回模型。
//
// 语义纪律(单 §8):
//   - job_get 终态带 resultRef 与 ≤32 KiB 预览,不自动重跑;
//   - job_wait 有界等待,超时回 pending+状态快照游标(timedOut=true),
//     不宣告任务失败;已完成的业务结果排在 wait 自身状态结果前
//     (单 §7 次序纪律的 wait 侧形状);
//   - job_cancel 回取消请求状态(cancel_requested|already_terminal),
//     不保证已终止。
#pragma once

#include <memory>
#include <string>

#include "tools/tool.hpp"
#include "tools/tool_job_coordinator.hpp"

namespace lubancode::tools {

class ToolRegistry;

// 三枚工具共用一只协调器(shared 保序:注册进 ToolRegistry 的工具与
// AsyncToolRuntime 的协调器同一份,谁先收场都不悬挂)。
std::unique_ptr<Tool> MakeJobGetTool(std::shared_ptr<ToolJobCoordinator> coordinator);
std::unique_ptr<Tool> MakeJobWaitTool(std::shared_ptr<ToolJobCoordinator> coordinator);
std::unique_ptr<Tool> MakeJobCancelTool(std::shared_ptr<ToolJobCoordinator> coordinator);

// 一次注册三枚(AsyncToolRuntime 装配用)。
void RegisterJobTools(ToolRegistry& registry, std::shared_ptr<ToolJobCoordinator> coordinator);

}  // namespace lubancode::tools
