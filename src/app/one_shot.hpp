// 单发模式(位置参数):按给定配置问一句,走 agent loop、带工具,不做
// 交互会话的事——不挂 ask_user、不排队、不写 session 存档。
// 实现在 one_shot.cpp(编译边界);交互头(interactive_session.hpp)的
// 解耦在后续 commit 里收,这里只留窄接口。
#pragma once

#include <string>
#include <vector>

#include "cli/theme.hpp"
#include "config/config.hpp"
#include "config/settings_local.hpp"

namespace lubancode::app {

// 单发模式(位置参数):也走 agent loop,同样支持工具,只是只问这一句。
// 管道/单发场景下 spinner_enabled 传进来的必然是 false(RunCli 里按
// DetectConsoleCapability().is_console 算好的),这里不用再判断一次。
// model_instructions:模型目录里当前模型的 base_instructions(RunCli 按
// 目录算好传进来,不在目录就是空串)。单发模式没有 /model,不用会话级
// 状态和包装层,构造 AgentLoop 时直接拼进系统提示,结构跟交互模式发出去
// 的一模一样;think/context_window 的目录应用同样由 RunCli 预先并进
// config,这里不重复判断——保持这个函数只管"按给定配置问一句"。
// soul_content(0.16.x 魂法分家):当前魂文件的原始内容(RunCli 按配置的
// soul 名读好传进来),单发模式没有 /soul,构造时直接叠加在系统提示最后
// (WithModelInstructions 之后,压轴),跟交互模式发出去的结构一模一样;
// 空串 = 不叠加。
// harness_output(Harbor Harness 派生 JSONL 单):--output 点名的绝对落点,
// 空串 = 没给,不导出。给了就在 one-shot 收口(CloseSession 之后)把
// canonical 轨迹投影成便携 JSONL 原子写到该路径;stdout 不混收据,
// stderr 打一行稳定可解析的收据(schema/session_id/records/sha256/path)。
// 导出失败不吞:退出码按"执行成败优先,执行成而导出败给导出失败码 3"
// 结算,stderr 带 session_id 与补导命令(单子 §三/§七)。
int AskOnce(const lubancode::config::Config& config, const std::string& question, bool auto_confirm,
            const lubancode::cli::Theme& theme, const std::string& persona, bool spinner_enabled,
            const lubancode::config::SettingsLocal& settings_local,
            const std::string& model_instructions = std::string(), const std::string& soul_content = std::string(),
            const std::vector<std::string>& package_dirs = std::vector<std::string>{},
            const std::string& harness_output = std::string());

}  // namespace lubancode::app
