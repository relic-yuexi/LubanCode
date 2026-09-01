#include "app/commands/channel_commands.hpp"

#include <cctype>
#include <chrono>
#include <sstream>

#include "app/commands/command_registry.hpp"  // SlashDispatchContext 完整定义
#include "channel/account_state.hpp"
#include "channel/ingress_store.hpp"
#include "cli/terminal_port.hpp"

namespace lubancode::app {

using lubancode::channel::ChannelAccountState;
using lubancode::channel::ChannelAccountStateName;
using lubancode::channel::ChannelManager;
using lubancode::channel::ChannelUserConfig;
using lubancode::channel::CredentialSourceName;
using lubancode::channel::DmPolicyName;
using lubancode::channel::GroupPolicyName;

ParsedChannelCommand ParseChannelCommand(const std::string& args) {
    ParsedChannelCommand parsed;
    std::string rest = args;
    // 剥两端空白。
    const auto first_non_space = rest.find_first_not_of(" \t");
    if (first_non_space == std::string::npos) {
        parsed.action = ChannelCommandAction::Overview;
        return parsed;
    }
    rest = rest.substr(first_non_space);
    const auto last_non_space = rest.find_last_not_of(" \t");
    rest = rest.substr(0, last_non_space + 1);

    std::string word = rest;
    std::string tail;
    const auto space = rest.find_first_of(" \t");
    if (space != std::string::npos) {
        word = rest.substr(0, space);
        tail = rest.substr(space + 1);
    }
    std::string lower = word;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // /channel pairing <list|approve|reject> ...(阶段 3):两词动作。
    if (lower == "pairing") {
        std::string sub = tail;
        const auto sub_first = sub.find_first_not_of(" \t");
        if (sub_first == std::string::npos) {
            parsed.action = ChannelCommandAction::Invalid;
            parsed.bad_word = "pairing";
            return parsed;
        }
        sub = sub.substr(sub_first);
        std::string sub_word = sub;
        std::string sub_tail;
        const auto sub_space = sub.find_first_of(" \t");
        if (sub_space != std::string::npos) {
            sub_word = sub.substr(0, sub_space);
            sub_tail = sub.substr(sub_space + 1);
        }
        std::string sub_lower = sub_word;
        for (char& c : sub_lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (sub_lower == "list") {
            parsed.action = ChannelCommandAction::PairingList;
        } else if (sub_lower == "approve") {
            parsed.action = ChannelCommandAction::PairingApprove;
        } else if (sub_lower == "reject") {
            parsed.action = ChannelCommandAction::PairingReject;
        } else {
            parsed.action = ChannelCommandAction::Invalid;
            parsed.bad_word = "pairing " + sub_word;
            return parsed;
        }
        // 参数:<channel> [account](list)与 <channel> [account] <code>
        //(approve/reject)。词序固定,按位置分:approve/reject 收到 3 词时
        // 第 3 词当 code,4 词时第 3 词 account、第 4 词 code。
        std::vector<std::string> words;
        std::string remaining = sub_tail;
        while (words.size() < 4) {
            const auto first = remaining.find_first_not_of(" \t");
            if (first == std::string::npos) break;
            remaining = remaining.substr(first);
            const auto next_space = remaining.find_first_of(" \t");
            if (next_space == std::string::npos) {
                words.push_back(remaining);
                break;
            }
            words.push_back(remaining.substr(0, next_space));
            remaining = remaining.substr(next_space + 1);
        }
        const bool needs_code =
            parsed.action == ChannelCommandAction::PairingApprove ||
            parsed.action == ChannelCommandAction::PairingReject;
        if (words.empty()) {
            parsed.action = ChannelCommandAction::Invalid;
            parsed.bad_word = "pairing " + sub_word;
            return parsed;
        }
        parsed.channel_id = words[0];
        if (!needs_code) {
            if (words.size() >= 2) parsed.account_id = words[1];
            return parsed;
        }
        if (words.size() == 2) {
            parsed.code = words[1];
        } else if (words.size() >= 3) {
            parsed.account_id = words[1];
            parsed.code = words[2];
        }
        if (parsed.code.empty()) {
            parsed.action = ChannelCommandAction::Invalid;
            parsed.bad_word = "pairing " + sub_word;
        }
        return parsed;
    }

    if (lower == "show") parsed.action = ChannelCommandAction::Show;
    else if (lower == "doctor") parsed.action = ChannelCommandAction::Doctor;
    else if (lower == "start") parsed.action = ChannelCommandAction::Start;
    else if (lower == "stop") parsed.action = ChannelCommandAction::Stop;
    else if (lower == "restart") parsed.action = ChannelCommandAction::Restart;
    else {
        parsed.action = ChannelCommandAction::Invalid;
        parsed.bad_word = word;
        return parsed;
    }

    // 目标:<channel> [account]。
    if (!tail.empty()) {
        const auto tail_first = tail.find_first_not_of(" \t");
        if (tail_first != std::string::npos) {
            tail = tail.substr(tail_first);
            const auto tail_space = tail.find_first_of(" \t");
            if (tail_space == std::string::npos) {
                parsed.channel_id = tail;
            } else {
                parsed.channel_id = tail.substr(0, tail_space);
                std::string account = tail.substr(tail_space + 1);
                const auto account_first = account.find_first_not_of(" \t");
                if (account_first != std::string::npos) {
                    account = account.substr(account_first);
                    const auto account_last = account.find_last_not_of(" \t");
                    parsed.account_id = account.substr(0, account_last + 1);
                }
            }
        }
    }
    return parsed;
}

std::vector<std::string> FormatChannelsOverview(
    const std::map<std::string, ChannelUserConfig>* channels,
    const std::vector<ChannelManager::AccountSnapshot>* snapshots) {
    std::vector<std::string> lines;
    const bool no_config = channels == nullptr || channels->empty();
    const bool no_runtime = snapshots == nullptr || snapshots->empty();
    if (no_config && no_runtime) {
        lines.push_back("没配渠道。全局 ~/.lubancode/config.json 的 channels 段里加账号后,这里会列出来。");
        return lines;
    }
    if (no_config) {
        // 配置侧已无 channels 段但账号还在跑(reload 未落):运行态照实列。
        lines.push_back("渠道账号(全局配置已无 channels 段;以下为本进程运行态):");
    } else {
        lines.push_back("渠道账号(配置来自全局 config;渠道默认关闭,普通交互进程不启 sidecar):");
    }
    if (!no_config) {
        for (const auto& [channel_id, channel] : *channels) {
            lines.push_back("  " + channel_id +
                            (channel.enabled ? "  [enabled]" : "  [disabled]") +
                            (channel.default_account.empty()
                                 ? std::string()
                                 : "  default=" + channel.default_account));
            for (const auto& [account_id, account] : channel.accounts) {
                std::ostringstream row;
                row << "    " << account_id << (account.enabled ? "  [enabled]" : "  [disabled]");
                row << "  dm=" << DmPolicyName(account.dm_policy);
                row << "  group=" << GroupPolicyName(account.group_policy);
                row << "  secret="
                    << CredentialSourceName(lubancode::channel::DescribeCredentialSource(account));
                if (account.transport.empty()) {
                    row << "  transport=(未选)";
                } else {
                    row << "  transport=" << account.transport;
                }
                if (!account.agent.empty()) {
                    row << "  agent=" << account.agent;
                }
                lines.push_back(row.str());
            }
        }
    }
    if (snapshots == nullptr) {
        lines.push_back("gateway not running——本进程是普通交互形态,不拉 sidecar、不开连接。");
        lines.push_back("要常驻渠道账号,跑 lubancode gateway run(阶段 9 落地)。");
        return lines;
    }
    if (snapshots->empty()) {
        lines.push_back("本进程没挂渠道账号(ChannelManager 空册)。");
        return lines;
    }
    lines.push_back("运行态:");
    for (const auto& snapshot : *snapshots) {
        std::ostringstream row;
        row << "  " << snapshot.channel_id << "/" << snapshot.account_id
            << "  " << ChannelAccountStateName(snapshot.state)
            << "  gen=" << snapshot.generation;
        if (snapshot.retry_at_ms > 0) {
            row << "  retry_at=" << snapshot.retry_at_ms;
        }
        row << "  pending=" << snapshot.inbox_pending
            << "  dead_letter=" << snapshot.dead_letter_count;
        if (!snapshot.lock_held) {
            row << "  (锁未持有)";
        }
        lines.push_back(row.str());
    }
    return lines;
}

std::vector<std::string> FormatChannelShow(const ChannelManager::AccountSnapshot* snapshot) {
    std::vector<std::string> lines;
    if (snapshot == nullptr) {
        lines.push_back("账号不在册。/channels 先看清单。");
        return lines;
    }
    lines.push_back("渠道 " + snapshot->channel_id + " / 账号 " + snapshot->account_id + ":");
    lines.push_back(std::string("  状态: ") + ChannelAccountStateName(snapshot->state) +
                    "  generation=" + std::to_string(snapshot->generation));
    lines.push_back(std::string("  策略: dm=") + DmPolicyName(snapshot->dm_policy) +
                    "  group=" + GroupPolicyName(snapshot->group_policy));
    lines.push_back(std::string("  密钥来源: ") + CredentialSourceName(snapshot->credential));
    {
        std::ostringstream row;
        row << "  水位: inbox=" << snapshot->inbox_pending
            << "  dead_letter=" << snapshot->dead_letter_count
            << "  pairing_pending=" << snapshot->pairing_pending
            << "  pairing_approved=" << snapshot->pairing_approved;
        lines.push_back(row.str());
    }
    if (!snapshot->ingress_state_counts.empty()) {
        std::ostringstream row;
        row << "  入站账:";
        for (const auto& [state, count] : snapshot->ingress_state_counts) {
            row << " " << state << "=" << count;
        }
        lines.push_back(row.str());
    }
    if (!snapshot->recent_transitions.empty()) {
        lines.push_back("  最近迁移:");
        for (const auto& transition : snapshot->recent_transitions) {
            std::ostringstream row;
            row << "    " << ChannelAccountStateName(transition.from) << " -> "
                << ChannelAccountStateName(transition.to);
            if (!transition.reason.empty()) {
                row << "  (" << transition.reason << ")";
            }
            if (transition.retry_at_ms > 0) {
                row << "  retry_at=" << transition.retry_at_ms;
            }
            lines.push_back(row.str());
        }
    }
    return lines;
}

std::vector<std::string> FormatChannelDoctor(const ChannelManager::AccountSnapshot* snapshot) {
    std::vector<std::string> lines;
    if (snapshot == nullptr) {
        lines.push_back("账号不在册。/channels 先看清单。");
        return lines;
    }
    lines.push_back("渠道 " + snapshot->channel_id + " / 账号 " + snapshot->account_id + " 体检:");
    // 密钥来源:只报名不报值(configuration.md §4)。
    switch (snapshot->credential) {
        case lubancode::channel::CredentialSource::FromEnv:
            lines.push_back("  密钥: 环境变量来源。ok。");
            break;
        case lubancode::channel::CredentialSource::FromFile:
            lines.push_back("  密钥: 文件来源。ok(文件权限须本用户可读)。");
            break;
        case lubancode::channel::CredentialSource::InlinePlaintext:
            lines.push_back("  密钥: WARNING——config 里放了明文 secret。日志与 trace 会打码,");
            lines.push_back("        但明文留在配置文件里不是久居之计,建议改 secret_env。");
            break;
        case lubancode::channel::CredentialSource::Missing:
            lines.push_back("  密钥: 未配置(secret_env/secret_file 都没给)。");
            lines.push_back("        启动闸: CredentialsMissing——不会重试风暴,补上密钥再起。");
            break;
    }
    lines.push_back(snapshot->lock_held ? "  账号锁: 本进程持有。"
                                        : "  账号锁: 未持有(账号没在跑或已停)。");
    {
        std::ostringstream row;
        row << "  队列: inbox=" << snapshot->inbox_pending << "/"
            << lubancode::channel::InboxLimits{}.max_pending_total << "  dead_letter="
            << snapshot->dead_letter_count;
        lines.push_back(row.str());
    }
    if (snapshot->pairing_pending > 0) {
        lines.push_back("  pairing: " + std::to_string(snapshot->pairing_pending) +
                        " 条待审,看 /channel pairing list <channel> [account]。");
    }
    if (IsUnrecoverableAccountState(snapshot->state)) {
        lines.push_back(std::string("  状态: ") + ChannelAccountStateName(snapshot->state) +
                        "——不可自动恢复,须人工处理后再起。");
    } else if (snapshot->state == ChannelAccountState::Backoff) {
        lines.push_back("  状态: backoff,下一次尝试 retry_at=" +
                        std::to_string(snapshot->retry_at_ms) +
                        "(第 " + std::to_string(snapshot->backoff_attempt) + " 次退避)。");
    }
    lines.push_back("  (体检不发平台请求;Package trust 与 manifest 检查在挂载侧,/package doctor 看。)");
    return lines;
}

std::vector<std::string> FormatChannelPairingList(
    const std::vector<ChannelManager::PendingPairingView>* pending, std::int64_t now_ms) {
    std::vector<std::string> lines;
    if (pending == nullptr) {
        lines.push_back("本进程没挂 ChannelManager(普通交互形态);配对账在 Gateway 进程里。");
        return lines;
    }
    if (pending->empty()) {
        lines.push_back("没有待审配对。dm_policy=pairing 的账号收到未知 sender 来信会在这里挂账。");
        return lines;
    }
    lines.push_back("待审配对(" + std::to_string(pending->size()) + " 条):");
    for (const auto& view : *pending) {
        std::ostringstream row;
        row << "  sender " << view.sender_id;
        const std::int64_t remaining_ms = view.expires_at_ms - now_ms;
        if (remaining_ms <= 0) {
            row << "  (已过期)";
        } else {
            row << "  剩 " << (remaining_ms / 1000) << " 秒";
        }
        row << "  /channel pairing approve <channel> <code>";
        lines.push_back(row.str());
    }
    return lines;
}

namespace {

void PrintLines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        lubancode::cli::TermOut() << line << "\n";
    }
}

// 从配置与运行册里定位账号快照;account_id 空时按 default_account 或唯一账号。
const ChannelManager::AccountSnapshot* FindSnapshot(
    ChannelManager* manager, const std::map<std::string, ChannelUserConfig>* channels,
    const std::string& channel_id, const std::string& account_id, std::string* resolved) {
    if (manager == nullptr) return nullptr;
    std::string account = account_id;
    if (account.empty() && channels != nullptr) {
        const auto channel = channels->find(channel_id);
        if (channel != channels->end()) {
            if (!channel->second.default_account.empty()) {
                account = channel->second.default_account;
            } else if (channel->second.accounts.size() == 1) {
                account = channel->second.accounts.begin()->first;
            }
        }
    }
    if (account.empty()) return nullptr;
    const auto snapshots = manager->Snapshots();
    for (const auto& snapshot : snapshots) {
        if (snapshot.channel_id == channel_id && snapshot.account_id == account) {
            *resolved = account;
            return &snapshot;  // 调用方立即消费,悬垂风险只在这帧内
        }
    }
    return nullptr;
}

void PrintChannelUsage() {
    lubancode::cli::TermOut() << "用法:\n";
    lubancode::cli::TermOut() << "  /channel show <channel> [account]\n";
    lubancode::cli::TermOut() << "  /channel doctor <channel> [account]\n";
    lubancode::cli::TermOut() << "  /channel start|stop|restart <channel> [account]\n";
    lubancode::cli::TermOut() << "  /channel pairing list <channel> [account]\n";
    lubancode::cli::TermOut() << "  /channel pairing approve|reject <channel> [account] <code>\n";
}

}  // namespace

CommandFlow HandleSlashChannels(SlashDispatchContext& ctx,
                                const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    const auto* channels = ctx.config != nullptr && ctx.config->channels.empty()
                               ? nullptr
                               : (ctx.config != nullptr ? &ctx.config->channels : nullptr);
    const auto snapshots = ctx.channel_manager != nullptr ? ctx.channel_manager->Snapshots()
                                                          : std::vector<ChannelManager::AccountSnapshot>();
    PrintLines(FormatChannelsOverview(
        channels, ctx.channel_manager != nullptr ? &snapshots : nullptr));
    return CommandFlow::Continue;
}

CommandFlow HandleSlashChannel(SlashDispatchContext& ctx,
                               const lubancode::cli::ParsedSlashCommand& parsed) {
    const ParsedChannelCommand command = ParseChannelCommand(parsed.args);
    const auto* channels = ctx.config != nullptr && !ctx.config->channels.empty()
                               ? &ctx.config->channels
                               : nullptr;
    switch (command.action) {
        case ChannelCommandAction::Overview:
            HandleSlashChannels(ctx, parsed);
            return CommandFlow::Continue;
        case ChannelCommandAction::Show: {
            if (command.channel_id.empty()) {
                PrintChannelUsage();
                return CommandFlow::Continue;
            }
            const auto snapshots = ctx.channel_manager != nullptr
                                       ? ctx.channel_manager->Snapshots()
                                       : std::vector<ChannelManager::AccountSnapshot>();
            const ChannelManager::AccountSnapshot* found = nullptr;
            for (const auto& snapshot : snapshots) {
                if (snapshot.channel_id != command.channel_id) continue;
                if (!command.account_id.empty() && snapshot.account_id != command.account_id) {
                    continue;
                }
                found = &snapshot;
                break;
            }
            PrintLines(FormatChannelShow(found));
            if (found == nullptr && ctx.channel_manager == nullptr) {
                lubancode::cli::TermOut()
                    << "本进程没挂 ChannelManager(普通交互形态);/channels 看配置侧。\n";
            }
            return CommandFlow::Continue;
        }
        case ChannelCommandAction::Doctor: {
            if (command.channel_id.empty()) {
                PrintChannelUsage();
                return CommandFlow::Continue;
            }
            std::string resolved;
            const ChannelManager::AccountSnapshot* found =
                FindSnapshot(ctx.channel_manager, channels, command.channel_id,
                             command.account_id, &resolved);
            if (found == nullptr && ctx.channel_manager != nullptr) {
                // 命中的是"没起"的账号:拿配置侧信息给一份纯配置体检。
                lubancode::cli::TermOut() << "账号没在跑,只按配置侧体检(" << command.channel_id
                                          << ")。\n";
            }
            PrintLines(FormatChannelDoctor(found));
            if (found == nullptr && ctx.channel_manager == nullptr) {
                lubancode::cli::TermOut()
                    << "本进程没挂 ChannelManager(普通交互形态)。\n";
            }
            return CommandFlow::Continue;
        }
        case ChannelCommandAction::Start:
        case ChannelCommandAction::Stop:
        case ChannelCommandAction::Restart: {
            if (command.channel_id.empty()) {
                PrintChannelUsage();
                return CommandFlow::Continue;
            }
            if (ctx.channel_manager == nullptr) {
                // configuration.md §3:/channel start 不改变进程形态。
                lubancode::cli::TermOut()
                    << "本进程是普通交互形态,渠道账号不在这里起。要常驻渠道,跑 "
                       "lubancode gateway run(阶段 9 落地);普通进程只读状态。\n";
                return CommandFlow::Continue;
            }
            std::string resolved;
            const bool has_account =
                FindSnapshot(ctx.channel_manager, channels, command.channel_id,
                             command.account_id, &resolved) != nullptr ||
                !command.account_id.empty();
            const std::string account = !command.account_id.empty() ? command.account_id : resolved;
            if (account.empty()) {
                lubancode::cli::TermOut() << "定位不到账号(给全 <channel> <account> 再试)。\n";
                return CommandFlow::Continue;
            }
            (void)has_account;
            const auto error = command.action == ChannelCommandAction::Start
                                   ? ctx.channel_manager->StartAccount(command.channel_id, account)
                                   : command.action == ChannelCommandAction::Stop
                                         ? ctx.channel_manager->StopAccount(command.channel_id, account)
                                         : ctx.channel_manager->RestartAccount(command.channel_id, account);
            if (error.has_value()) {
                lubancode::cli::TermOut() << "失败: " << *error << "\n";
            } else {
                lubancode::cli::TermOut() << "已提交(" << command.channel_id << "/" << account
                                          << ")。\n";
            }
            return CommandFlow::Continue;
        }
        case ChannelCommandAction::PairingList:
        case ChannelCommandAction::PairingApprove:
        case ChannelCommandAction::PairingReject: {
            if (command.channel_id.empty()) {
                PrintChannelUsage();
                return CommandFlow::Continue;
            }
            if (ctx.channel_manager == nullptr) {
                // 与 start 同规(§3):普通交互进程只读,不碰配对账。
                lubancode::cli::TermOut()
                    << "本进程是普通交互形态,配对账在 Gateway 进程里。\n";
                return CommandFlow::Continue;
            }
            // 定位账号:显式给 account 用之;否则 default_account 或唯一账号。
            std::string account = command.account_id;
            if (account.empty() && channels != nullptr) {
                const auto channel = channels->find(command.channel_id);
                if (channel != channels->end()) {
                    if (!channel->second.default_account.empty()) {
                        account = channel->second.default_account;
                    } else if (channel->second.accounts.size() == 1) {
                        account = channel->second.accounts.begin()->first;
                    }
                }
            }
            if (account.empty()) {
                lubancode::cli::TermOut() << "定位不到账号(给全 <channel> <account> 再试)。\n";
                return CommandFlow::Continue;
            }
            if (command.action == ChannelCommandAction::PairingList) {
                const auto pending = ctx.channel_manager->PendingPairings(command.channel_id, account);
                const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count();
                lubancode::cli::TermOut() << command.channel_id << "/" << account << ":\n";
                PrintLines(FormatChannelPairingList(&pending, now_ms));
                return CommandFlow::Continue;
            }
            std::string error;
            const auto sender = command.action == ChannelCommandAction::PairingApprove
                                    ? ctx.channel_manager->ApprovePairing(command.channel_id, account,
                                                                          command.code, &error)
                                    : ctx.channel_manager->RejectPairing(command.channel_id, account,
                                                                         command.code, &error);
            if (sender.has_value()) {
                lubancode::cli::TermOut()
                    << (command.action == ChannelCommandAction::PairingApprove ? "已批准 " : "已拒绝 ")
                    << *sender << "(" << command.channel_id << "/" << account << ")。\n";
            } else {
                lubancode::cli::TermOut() << "失败: " << error << "\n";
            }
            return CommandFlow::Continue;
        }
        case ChannelCommandAction::Invalid:
            lubancode::cli::TermOut() << "认不得 \"" << command.bad_word << "\"。\n";
            PrintChannelUsage();
            return CommandFlow::Continue;
    }
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
