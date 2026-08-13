#pragma once

#include "tools/tool.hpp"

namespace lubancode::tools {

// 查后台任务清单和输出(只读,不需确认)。两种用法:
//   1. 不给 task_id —— 列出所有后台任务的摘要(task_id、状态、命令、PID、
//      日志路径),让模型先看全局再决定查哪一条。
//   2. 给 task_id   —— 该任务详情 + 日志文件尾部 tail_lines 行(默认 50)。
//      任务还在跑也能读(FILE_SHARE_READ 允许边写边读)。
// 配合 run_command 的 run_in_background:true 用:命令一后台起来,模型随时
// 回来查它跑完没、吐了什么。stop_background 工具管收尾。
class BackgroundOutputTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }
    Result execute(const nlohmann::json& input) override;
};

// 主动停掉一个后台任务(有副作用,需确认)。Windows TerminateProcess 根进程,
// POSIX kill(-pid) 杀整个进程组。已终态的任务不重复杀。
class StopBackgroundTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }
    Result execute(const nlohmann::json& input) override;
};

}  // namespace lubancode::tools
