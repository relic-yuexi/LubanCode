// /telemetry(端云协同可观测架构与 Telemetry 插件设计单 §24.2,实施分期
// T1 status 先行、T2 补 enable/disable/pause/resume/flush/spool/consent):
//   /telemetry [status]   只显示状态,不改变配置不发请求
//   enable|disable session|config  裸敲只给选项;用户明确选才动手
//                         (session = 当前进程;config = 写全局配置一枚布尔,
//                          项目配置永不暗改)
//   pause|resume          停/复出口(投影/spool 照常)
//   flush [毫秒]          seal + 有界赶发
//   spool [clear --confirm]  列账/两步确认删除
//   consent [grant|revoke]   §8.4 公网确认(回环免)
//   policy                T4 未落地,明说
#pragma once

#include "app/commands/command_flow.hpp"   // CommandFlow
#include "app/commands/command_registry.hpp"  // SlashDispatchContext
#include "cli/slash_commands.hpp"          // ParsedSlashCommand

namespace lubancode::app {

CommandFlow HandleSlashTelemetry(SlashDispatchContext& ctx,
                                 const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
