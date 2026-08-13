#pragma once

#include "tools/tool.hpp"

namespace lubancode::tools {

// 跑一条 shell 命令,把标准输出/标准错误合流捕获,带上退出码回传。
// 入参可选 "shell": "powershell"(默认)或 "cmd",分别经 PowerShell 或
// cmd.exe 执行。执行前要经用户确认(needs_confirm() == true)。超时会强制
// 杀掉整棵进程树,不会挂死。
//
// 入参可选 "run_in_background": true 时不等命令跑完,spawn 成功立刻返回
// task_id、PID 和日志文件路径。task_id 进 BackgroundTaskRegistry(tools/
// background_tasks.hpp)的台账,一条 watcher 线程轮询探活,命令完成时主
// 交互循环收到通知打一行"[后台任务 #N 完成]";之后用 background_output
// 工具按 task_id 查状态/读输出,stop_background 工具收尾。起 dev server、
// watch 进程这类要跨命令存活的长命进程,或者想后台跑完不阻塞对话的短任务,
// 都走这个模式。timeout_ms 对后台模式无意义,会被忽略。
class RunCommandTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }
    Result execute(const nlohmann::json& input) override;
};

}  // namespace lubancode::tools
