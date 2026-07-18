// M9:hooks 系统——config.json 里配的外部命令,在工具调用前后、会话开始
// 结束时跑一下。跟 run_command 工具是两回事:这里不经用户确认,也不走
// PowerShell(直接扔给平台默认 shell:Windows cmd.exe / POSIX /bin/sh),
// 复用的只是起子进程这一层机制(见 platform/process.hpp)。
//
// 语义(实现前 main.cpp/loop.cpp 那份 M9 任务书里定下来的):
//   pre_tool  —— 命中的钩子只要有一条退出码非零,就拦截:那次工具调用
//                不会真的执行,tool_result 填 is_error,内容里带退出码和
//                钩子输出的前几行,好让模型看得懂"为什么被拦、该怎么办"。
//   post_tool —— 工具已经跑完(不管成功/失败)才跑,退出码非零只打一行
//                警告,不影响已经产生的结果、不倒回去重跑。
//   session_start/session_end —— 没有 matcher,配几条跑几条,失败只警告,
//                不阻断会话本身。
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

constexpr int kDefaultHookTimeoutMs = 30000;

// pre_tool 钩子跑完的结果。intercepted == true 时,block_message 就是要塞
// 进 tool_result 里的 is_error 说明文本。
struct PreToolHookOutcome {
    bool intercepted = false;
    std::string block_message;
};

// 依次跑 hooks.pre_tool 里 matcher 命中 tool_name 的条目(matcher == "*"
// 或者精确等于 tool_name)。第一条退出码非零就判定拦截,不再跑后面的
// 条目;钩子命令本身起不来(spawn 失败)或超时,按"这条没拦住"处理,只
// 打警告,不影响放行。全部条目都放行(或者没有命中的条目)时,intercepted
// 为 false。
PreToolHookOutcome RunPreToolHooks(const config::HooksConfig& hooks, const std::string& tool_name,
                                    const nlohmann::json& tool_input);

// 依次跑 hooks.post_tool 里 matcher 命中的条目,不看退出码拦不拦——非零
// 只打一行警告(std::cerr),不影响已经产生的 result。
void RunPostToolHooks(const config::HooksConfig& hooks, const std::string& tool_name,
                       const nlohmann::json& tool_input, const Tool::Result& result);

// session_start/session_end:没有 matcher,配了几条跑几条,失败(非零退出码/
// 起不来/超时)只打警告,不阻断会话。
void RunSessionStartHooks(const config::HooksConfig& hooks);
void RunSessionEndHooks(const config::HooksConfig& hooks);

}  // namespace lubancode::tools
