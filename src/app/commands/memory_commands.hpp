// 终端接线收尾单:/memory 命令 presenter。原先整只 HandleMemoryCommand
// (387 行单函数)住在 interactive_session 大类里,按病灶二拆出:命令
// 产出数据行(project_memory 的调用与结果),presenter 负责怎么画(输出
// 全走 TerminalPort);大类只留分派(一行调用)与会话状态。
//
// ensure_tool:会话侧 EnsureMemoryTool 的接线——on/learn 档位打开后要把
// memory_save 工具补注册进 registry,这事归会话(工具栈在它那),命令层
// 只回调报一声。

#pragma once

#include <functional>
#include <string>

namespace lubancode::memory {
class ProjectMemory;
}
namespace lubancode::cli {
struct Theme;
}

namespace lubancode::app {

struct MemoryCommandContext {
    lubancode::memory::ProjectMemory* project_memory = nullptr;  // 空 = 未装配(unavailable)
    const lubancode::cli::Theme* theme = nullptr;
    // on/learn 成功后回调(补注册 memory_save;可空 = 单测/无 registry 场景)。
    std::function<void()> ensure_tool;
};

// /memory 的全部动作:status/on/off/use/learn/review/accept/reject/edit/
// why/list/remember/forget/rebuild/stale/verify/refresh/show/open/migrate。
// 用法不对打用法;project_memory 未装配打 unavailable。
void HandleMemoryCommand(const MemoryCommandContext& ctx, const std::string& raw_args);

}  // namespace lubancode::app
