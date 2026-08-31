// /telemetry(端云协同可观测架构与 Telemetry 插件设计单 §24.2,实施分期
// T1"/telemetry status、/doctor telemetry 本地面"):
//   /telemetry          只显示本地状态,不改变配置,不发任何请求(§24.2
//                       "裸 /telemetry 只显示状态")
//   /telemetry status   同上
// enable/disable/pause/resume/flush/spool/consent/policy 是 §24.2 的完整
// 命令族,按分期落在 T2(exporter/consent 进来才有意义);T1 敲了明说
// 未实现,不装样子。
#pragma once

#include "app/commands/command_flow.hpp"   // CommandFlow
#include "app/commands/command_registry.hpp"  // SlashDispatchContext
#include "cli/slash_commands.hpp"          // ParsedSlashCommand

namespace lubancode::app {

CommandFlow HandleSlashTelemetry(SlashDispatchContext& ctx,
                                 const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
