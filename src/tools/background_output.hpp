#pragma once

#include <functional>

#include "tools/tool.hpp"

namespace lubancode::tools {

class TaskLedger;

// 查后台任务清单和输出(只读,不需确认)。两种用法:
//   1. 不给 task_id —— 列出所有后台任务的摘要(task_id、状态、命令、PID、
//      日志路径),让模型先看全局再决定查哪一条。
//   2. 给 task_id   —— 该任务详情 + 日志文件尾部 tail_lines 行(默认 50)。
//      任务还在跑也能读(FILE_SHARE_READ 允许边写边读)。
// 配合 run_command 的 run_in_background:true 用:命令一后台起来,模型随时
// 回来查它跑完没、吐了什么。stop_background 工具管收尾。
//
// 停控两本账收口(后台代理管控三连 bug 单,Bug B):agent 工具 background
// 派的后台子代理记在会话台账(TaskLedger,int 编号,面板同款显示 id),
// 与后台命令登记簿是两本账。装配层经 SetAgentLedgerProvider 把台账接进
// 来之后,列表合并两类来源、按编号也能查到代理——"模型拿什么看见、就拿
// 什么能停"。没接(旧装配/单测)时行为与从前一字不差,只认命令登记簿。
class BackgroundOutputTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }
    Result execute(const nlohmann::json& input) override;

    // 接入会话台账(可空:不接 = 只认后台命令,老行为)。provider 返回的
    // 指针由装配方保活(AgentTool 与工具同 registry,生命周期同步)。
    void SetAgentLedgerProvider(std::function<TaskLedger*()> provider) {
        agent_ledger_provider_ = std::move(provider);
    }

private:
    std::function<TaskLedger*()> agent_ledger_provider_;
};

// 主动停掉一个后台任务(有副作用,需确认)。Windows TerminateProcess 根进程,
// POSIX kill(-pid) 杀整个进程组。已终态的任务不重复杀。
// 台账接入后,面板可见的后台子代理(agent 工具 background 派的,面板与
// agent 工具回执里的 #N 编号)同号可停——先认活命令(模型亲手起的),
// 命令登记簿查不到再停面板代理,两本账都认。
class StopBackgroundTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }
    ApprovalClass approval_class() const override { return ApprovalClass::External; }
    Result execute(const nlohmann::json& input) override;

    void SetAgentLedgerProvider(std::function<TaskLedger*()> provider) {
        agent_ledger_provider_ = std::move(provider);
    }

private:
    std::function<TaskLedger*()> agent_ledger_provider_;
};

}  // namespace lubancode::tools
