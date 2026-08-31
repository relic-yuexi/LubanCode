// /channels 与 /channel(多渠道消息接入单阶段 2 的命令面)。
//
// 唯一真源 docs/architecture/channels/configuration.md(§1 配置形状、§3
// 启动闸与"普通进程不启渠道"、§6 pairing、§11 账号锁)。本批命令面口径:
//   - /channels           列配置里的渠道账号 × 运行态快照;
//   - /channel show       单账号详情(策略/凭据来源/水位/最近迁移);
//   - /channel doctor     单账号体检(密钥来源只报名不报值、锁、账与队列,
//                         不发任何外部请求);
//   - /channel start|stop|restart
//                         管理动作。ChannelManager 没挂进本进程(普通交互
//                         进程的铁律,configuration.md §3)时只给
//                         "lubancode gateway run" 引导,不改变进程形态。
//
// 纯函数(FormatXxx)生成行,handler 只管打印与接线;单测直接钉纯函数。
// pairing list/approve/reject 与 add/login/logout/bind 留阶段 3 路由批
//(TODO §21 全集,按阶段拆)。
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app/commands/command_flow.hpp"
#include "channel/channel_config.hpp"
#include "channel/manager.hpp"
#include "cli/slash_commands.hpp"

namespace lubancode::app {

struct SlashDispatchContext;

// ---------------- 二级解析(纯函数,单测钉) ----------------

enum class ChannelCommandAction {
    Invalid,
    Overview,  // /channel 裸敲 = /channels 同义
    Show,
    Doctor,
    Start,
    Stop,
    Restart,
};

struct ParsedChannelCommand {
    ChannelCommandAction action = ChannelCommandAction::Invalid;
    std::string channel_id;   // show/doctor/start/stop/restart 的渠道 id
    std::string account_id;   // 可空 = 渠道 default_account 或唯一账号
    std::string bad_word;     // Invalid 时第一词原文
};

ParsedChannelCommand ParseChannelCommand(const std::string& args);

// ---------------- 行生成(纯函数,单测钉) ----------------

// /channels 总览。channels 可空(没配渠道);snapshots 可空(本进程没挂
// ChannelManager = 普通交互进程,给 gateway 引导行)。
std::vector<std::string> FormatChannelsOverview(
    const std::map<std::string, lubancode::channel::ChannelUserConfig>* channels,
    const std::vector<lubancode::channel::ChannelManager::AccountSnapshot>* snapshots);

// /channel show:单账号详情。找不到目标账号时返回一行"不在册"。
std::vector<std::string> FormatChannelShow(
    const lubancode::channel::ChannelManager::AccountSnapshot* snapshot);

// /channel doctor:密钥来源只报名不报值(InlinePlaintext 给 warning,
// configuration.md §4)。
std::vector<std::string> FormatChannelDoctor(
    const lubancode::channel::ChannelManager::AccountSnapshot* snapshot);

// ---------------- 执行(handler) ----------------

CommandFlow HandleSlashChannels(SlashDispatchContext& ctx,
                                const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashChannel(SlashDispatchContext& ctx,
                               const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
