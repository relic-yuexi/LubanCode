#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tools/tool.hpp"

namespace lubancode::tools {

// run_command 的几枚纯函数(Terminal-Bench 加固单抽出来,便于确定性测试)。
namespace run_command_detail {

// 未显式传 timeout_ms 时的默认超时(毫秒)。只看真正的可执行名:跳过前导
// `cd <dir> &&`、`sudo [-opts]`、`env [-opts] K=V`、`VAR=val`、`time`、
// `nohup` 这些前缀;构建/装依赖/测试类首词抬到 900000,其余 120000。参数
// 正文一概不扫(`echo "cmake"` 不抬)。python/python3 只在 `-m pip|pytest`
// 时抬。
int DefaultTimeoutMsForCommand(const std::string& command);

// POSIX 下选哪只 shell。requested 空 = 模型没指定:本机有 bash(bash_path
// 有值)就用它,没有回落 /bin/sh;"sh" 恒走 /bin/sh;"bash" 走 bash_path
// (调用方已验过装没装,没装时同样回落 /bin/sh 防呆)。id 进后台台账与隔离闸。
struct ShellChoice {
    std::string id;          // "bash" / "sh"
    std::string executable;  // 绝对路径
};
ShellChoice ChoosePosixShell(const std::string& requested, const std::optional<std::string>& bash_path);

// 非交互默认环境(POSIX 子进程):apt/pip/git 不弹问、python 不攒缓冲、
// 分页器不吊住。
std::vector<std::pair<std::string, std::string>> NonInteractiveEnvDefaults();

// 只挑父环境没设的那几条补(is_set 判"设没设",空串也算设过——用户显式
// 设成空是他的意思,不替他改)。
std::vector<std::pair<std::string, std::string>> FillMissingEnv(
    const std::vector<std::pair<std::string, std::string>>& defaults,
    const std::function<bool(const std::string&)>& is_set);

}  // namespace run_command_detail

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

    // 逐枚追踪单:注册元数据声明。
    lubancode::tools::EffectClass effect_class() const override { return lubancode::tools::EffectClass::LocalProcessUnknown; }
    lubancode::tools::Idempotency idempotency() const override { return lubancode::tools::Idempotency::NonIdempotent; }
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return true; }
    ApprovalClass approval_class() const override { return ApprovalClass::Command; }
    Result execute(const nlohmann::json& input) override;
    // 子代理 x 停止失效单:取消旗随调用递进——共享实例上的 cancel_ 只认
    // 装配层那根(主回合 ESC),子代理的 CancelChain 合并旗(面板 x/墙钟)
    // 从 context 走。两根都在时 context 优先,SetCancel 兜底(单测直调)。
    Result execute(const nlohmann::json& input, const ToolExecutionContext& context) override;

    // 进程生命线单(P1:前台取消通道):ESC 取消链。turn_runner 每轮灌
    //(与插件的 SetPluginCancel 同一条链)。置位后前台命令的等待循环每拍
    // 查旗,收整棵树、分型 cancelled(与超时分开记账)。不灌 = 行为与旧版
    // 完全一致。
    void SetCancel(const std::atomic<bool>* cancel) { cancel_ = cancel; }

private:
    // 公共实现:effective_cancel 是本调用真用的取消旗(execute 两个口各算
    // 各的)。置位即收整棵进程树,结果分型 cancelled(与超时分开记账)。
    Result Run(const nlohmann::json& input, const std::atomic<bool>* effective_cancel);

    const std::atomic<bool>* cancel_ = nullptr;
};

}  // namespace lubancode::tools
