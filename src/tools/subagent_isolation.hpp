// IsolationRooms(骨架拆解批三·病十四:AgentTool 六职拆分之隔件)。
// isolation=worktree 的房务与包装表:
//   - SetupRoom/FinishRoom:从 cwd 找仓库根、建房(agent- 前缀,fresh 基准)、
//     上锁;收工解锁、干净删房,有活留房并给结果文本附言。
//   - BuildIsolatedRegistry:把一张工具表整体包成"落在房里"的表——路径
//     入参按房解析成绝对路径,run_command 注入房作为工作目录;三道闸
//     (文件/cwd/git 改道)由工具自身按线程本地的隔离范围栈(IsolationGuard,
//     AgentLoop 跑动前压入)执行,这里只管包装。
// 子代理是进程内线程,共享进程 cwd——绝不能 chdir(会把主会话的读写全带
// 进沟里,多个隔离子代理并行时更互相踩脚)。做法是不动各工具内部,套
// 装饰层。包装件按引用持内层工具,源表必须活得比返回的表久。
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "runtime/worktree.hpp"
#include "tools/isolation.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

// isolation=worktree 的一站式准备:从 cwd 找仓库根、建房、上锁。失败给
// Result 错误,成功返回房信息。
std::optional<lubancode::cli::AgentWorktree> SetupIsolationRoom(const std::string& cwd,
                                                                const lubancode::cli::GitRunner& runner,
                                                                Tool::Result& error_out);

// 收工房务(派工单 §五):解锁;干净且无自有提交才删房,有活(未提交/
// 已提交)留房待主控复核,note 给结果文本,removed/awaiting_review/
// head_commit 进任务快照。
lubancode::cli::AgentWorktreeFinish FinishIsolationRoom(const lubancode::cli::AgentWorktree& room,
                                                        const lubancode::cli::GitRunner& runner);

// 把一张工具表整体包成"落在房里"的表(路径按房解析、cwd 注入房)。
std::unique_ptr<ToolRegistry> BuildIsolatedRegistry(ToolRegistry& source, const IsolationScope& scope);

}  // namespace lubancode::tools
