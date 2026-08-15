// hooks 运行时(进程级单份):装载配置、建信任账、立 dispatcher。会话启动
// 时 Setup 一次;回调装配层(turn_runner)、会话起落(cli_app 的
// SessionHookScope)、/hooks 管理面共用这一份。
//
// 空态:没配任何 hooks 时 Setup 出来的 dispatcher 是空的,HookRuntime()
// 照常返回指针(Empty() 为真),装配层据此不挂 hook 回调——与"没有 hooks
// 系统"行为逐字节一致。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "hooks/dispatcher.hpp"

namespace lubancode::app {

// 装载:config.hooks(user+project 相加后的合并账,条目各自带 source_path)
// -> 来源分级 -> definition hash -> project 信任审查。同时把会话级公共字段
// (cwd/permission_mode)填进 dispatcher 上下文;session_id 之后由会话层
// UpdateContext 补。未配置任何 hooks 时也是合法调用(空 dispatcher)。
//
// 返回给用户看的启动提示(未信任项目 hook 指路 /hooks、禁用账、信任账本
// 读不动的警告),调用方决定打不打、打到哪里(交互模式打 stderr,单发/
// 管道模式不打,保持管道输出干净)。
std::vector<std::string> SetupHookRuntime(const config::ConfigResult& config_result);

// 进程级 dispatcher。Setup 之前(或从未 Setup)= nullptr。指针在进程生命
// 期内稳定(内部 unique_ptr),存下来跨轮次用安全。
hooks::HookDispatcher* HookRuntime();

// 最近一次 Setup 的启动提示(供交互会话晚一点打)。
const std::vector<std::string>& HookStartupNotices();

// 会话层补上下文(session_id/transcript_path/turn_id 每轮换)。没 Setup 过
// 就空操作。
void UpdateHookRuntimeContext(hooks::HookContext context);

// 帮忙把当前确认档翻成协议字段值。
std::string HookPermissionModeText();

}  // namespace lubancode::app
