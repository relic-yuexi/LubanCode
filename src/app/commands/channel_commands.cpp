#include "app/commands/channel_commands.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iterator>
#include <sstream>
#include <utility>

#include "config/config.hpp"  // Config::channels(账号表;原先经注册表头间接带进)
#include "channel/account_state.hpp"
#include "channel/ingress_store.hpp"
#include "cli/terminal_frame.hpp"  // frame::*(TUI 排版批 5a:/channel 渲染段)
#include "cli/terminal_port.hpp"
#include "platform/console.hpp"  // GetScreenInfo:整族框宽同一把尺

namespace lubancode::app {

using lubancode::channel::ChannelAccountState;
using lubancode::channel::ChannelAccountStateName;
using lubancode::channel::ChannelManager;
using lubancode::channel::ChannelUserConfig;
using lubancode::channel::CredentialSourceName;
using lubancode::channel::DmPolicyName;
using lubancode::channel::GroupPolicyName;

namespace {

// ---- TUI 排版批 5a(/channel 全族)的公共小件 ------------------------------
//
// 渲染段只调 cli::frame::* 三助手 + divider(批 0 基件,约定见
// docs/development/tui_style.md)。本文件历史文案为硬编码中文,批 2 裁量 1
// 在此同样适用:既有句子一字不添不改进 frame,表头/列名用数据 schema 名;
// 句内冒号按 SentenceField 拆两列。

namespace frame = lubancode::cli::frame;

int ChannelFrameWidth() {
    if (const auto info = lubancode::platform::GetScreenInfo()) {
        return info->width;
    }
    return 0;
}

void AppendLines(std::vector<std::string>& out, std::vector<std::string> lines) {
    out.insert(out.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
}

std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

frame::Field SentenceField(const std::string& sentence,
                           frame::FieldAccent accent = frame::FieldAccent::None) {
    const std::size_t colon = sentence.find(':');
    if (colon == std::string::npos) {
        return frame::Field{"", sentence, accent};
    }
    return frame::Field{TrimAscii(sentence.substr(0, colon)), TrimAscii(sentence.substr(colon + 1)), accent};
}

// 单句/多句通知 → 无标题键值对框的行(handler 里就地落盘)。
std::vector<std::string> NoticeLines(const lubancode::cli::Theme& theme,
                                     std::initializer_list<std::string> sentences,
                                     frame::FieldAccent accent = frame::FieldAccent::None) {
    std::vector<frame::Field> fields;
    for (const std::string& sentence : sentences) {
        fields.push_back(SentenceField(sentence, accent));
    }
    return frame::RenderKeyValues({}, fields, theme, frame::Light(), ChannelFrameWidth());
}

// 运行态 state 列的语义色(批 5a):Running 通=pass;退避/降级/禁用是业务性
// 跳过=skip;四个不可恢复终态(Misconfigured/TrustRequired/NeedsLogin/
// Fatal)是真失败=error 档;其余过渡态不填色。
frame::CellTone AccountStateTone(ChannelAccountState state) {
    if (state == ChannelAccountState::Running) {
        return frame::CellTone::Pass;
    }
    if (IsUnrecoverableAccountState(state)) {
        return frame::CellTone::Fail;
    }
    if (state == ChannelAccountState::Backoff || state == ChannelAccountState::Degraded ||
        state == ChannelAccountState::Disabled) {
        return frame::CellTone::Skip;
    }
    return frame::CellTone::Normal;
}

}  // namespace

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
    const std::vector<ChannelManager::AccountSnapshot>* snapshots,
    const lubancode::cli::Theme& theme) {
    std::vector<std::string> lines;
    const bool no_config = channels == nullptr || channels->empty();
    const bool no_runtime = snapshots == nullptr || snapshots->empty();
    if (no_config && no_runtime) {
        AppendLines(lines, NoticeLines(theme, {"没配渠道。全局 ~/.lubancode/config.json 的 channels 段里加账号后,这里会列出来。"}));
        return lines;
    }
    if (!no_config) {
        // 配置表:渠道行与账号行同表(渠道行只占 channel/state 两列,default
        // 标注并进 state 值);enabled/disabled 在 state 列上 pass/skip 分档。
        std::vector<frame::TableColumn> columns;
        columns.push_back({"channel"});
        columns.push_back({"account"});
        columns.push_back({"state"});
        columns.push_back({"dm"});
        columns.push_back({"group"});
        columns.push_back({"secret"});
        columns.push_back({"transport"});
        columns.push_back({"agent"});
        std::vector<frame::TableRow> rows;
        for (const auto& [channel_id, channel] : *channels) {
            std::string channel_state =
                channel.enabled ? std::string("[enabled]") : std::string("[disabled]");
            if (!channel.default_account.empty()) {
                channel_state += " default=" + channel.default_account;
            }
            rows.push_back(frame::TableRow{
                {channel_id, "", channel_state, "", "", "", "", ""},
                {frame::CellTone::Normal, frame::CellTone::Normal,
                 channel.enabled ? frame::CellTone::Pass : frame::CellTone::Skip}});
            for (const auto& [account_id, account] : channel.accounts) {
                rows.push_back(frame::TableRow{
                    {"", account_id, account.enabled ? "[enabled]" : "[disabled]",
                     DmPolicyName(account.dm_policy), GroupPolicyName(account.group_policy),
                     CredentialSourceName(lubancode::channel::DescribeCredentialSource(account)),
                     account.transport.empty() ? "(未选)" : account.transport, account.agent},
                    {frame::CellTone::Normal, frame::CellTone::Normal,
                     account.enabled ? frame::CellTone::Pass : frame::CellTone::Skip}});
            }
        }
        AppendLines(lines,
                    frame::RenderTable(
                        "渠道账号(配置来自全局 config;渠道默认关闭,普通交互进程不启 sidecar)",
                        columns, rows, theme, frame::Light(), ChannelFrameWidth()));
    }
    if (snapshots == nullptr) {
        AppendLines(lines, NoticeLines(theme, {"gateway not running——本进程是普通交互形态,不拉 sidecar、不开连接。",
                                               "要常驻渠道账号,跑 lubancode gateway run(阶段 9 落地)。"}));
        return lines;
    }
    if (snapshots->empty()) {
        AppendLines(lines, NoticeLines(theme, {"本进程没挂渠道账号(ChannelManager 空册)。"}));
        return lines;
    }
    if (no_config) {
        // 配置侧已无 channels 段但账号还在跑(reload 未落):运行态照实列。
        AppendLines(lines,
                    NoticeLines(theme, {"渠道账号(全局配置已无 channels 段;以下为本进程运行态)"}));
    }
    {
        // 运行态表:数值列(gen/retry/inbox/dead_letter)右对齐;state 列按
        // AccountStateTone 分 pass/skip/error 档(批 5a 语义色)。
        std::vector<frame::TableColumn> columns;
        columns.push_back({"account"});
        columns.push_back({"state"});
        columns.push_back({"gen", 0, /*align_right=*/true});
        columns.push_back({"retry_at", 0, /*align_right=*/true});
        columns.push_back({"inbox", 0, /*align_right=*/true});
        columns.push_back({"dead_letter", 0, /*align_right=*/true});
        columns.push_back({"lock"});
        std::vector<frame::TableRow> rows;
        for (const auto& snapshot : *snapshots) {
            rows.push_back(frame::TableRow{
                {snapshot.channel_id + "/" + snapshot.account_id,
                 ChannelAccountStateName(snapshot.state), std::to_string(snapshot.generation),
                 snapshot.retry_at_ms > 0 ? std::to_string(snapshot.retry_at_ms) : std::string(),
                 std::to_string(snapshot.inbox_pending), std::to_string(snapshot.dead_letter_count),
                 snapshot.lock_held ? std::string() : std::string("(锁未持有)")},
                {frame::CellTone::Normal, AccountStateTone(snapshot.state)}});
        }
        AppendLines(lines, frame::RenderTable("运行态", columns, rows, theme, frame::Light(),
                                              ChannelFrameWidth()));
    }
    return lines;
}

std::vector<std::string> FormatChannelShow(const ChannelManager::AccountSnapshot* snapshot,
                                           const lubancode::cli::Theme& theme) {
    std::vector<std::string> lines;
    if (snapshot == nullptr) {
        AppendLines(lines, NoticeLines(theme, {"账号不在册。/channels 先看清单。"}));
        return lines;
    }
    // 键值对框:短字段一组(状态/策略/密钥来源/水位/入站账),标题=定位串
    //(批 2 裁量 3:引导下文的尾冒号剥掉)。
    std::vector<frame::Field> fields;
    {
        std::ostringstream row;
        row << "状态: " << ChannelAccountStateName(snapshot->state)
            << "  generation=" << snapshot->generation;
        fields.push_back(SentenceField(row.str()));
    }
    {
        std::ostringstream row;
        row << "策略: dm=" << DmPolicyName(snapshot->dm_policy)
            << "  group=" << GroupPolicyName(snapshot->group_policy);
        fields.push_back(SentenceField(row.str()));
    }
    fields.push_back(SentenceField(std::string("密钥来源: ") + CredentialSourceName(snapshot->credential)));
    {
        std::ostringstream row;
        row << "水位: inbox=" << snapshot->inbox_pending
            << "  dead_letter=" << snapshot->dead_letter_count
            << "  pairing_pending=" << snapshot->pairing_pending
            << "  pairing_approved=" << snapshot->pairing_approved;
        fields.push_back(SentenceField(row.str()));
    }
    if (!snapshot->ingress_state_counts.empty()) {
        std::ostringstream row;
        row << "入站账:";
        for (const auto& [state, count] : snapshot->ingress_state_counts) {
            row << " " << state << "=" << count;
        }
        fields.push_back(SentenceField(row.str()));
    }
    AppendLines(lines,
                frame::RenderKeyValues("渠道 " + snapshot->channel_id + " / 账号 " + snapshot->account_id,
                                       fields, theme, frame::Light(), ChannelFrameWidth()));
    if (!snapshot->recent_transitions.empty()) {
        // 迁移表:transition 列保持 "from -> to" 连排(与旧输出同形),reason
        // 与 retry_at 各归一列。
        std::vector<frame::TableColumn> columns;
        columns.push_back({"transition"});
        columns.push_back({"reason"});
        columns.push_back({"retry_at", 0, /*align_right=*/true});
        std::vector<frame::TableRow> rows;
        for (const auto& transition : snapshot->recent_transitions) {
            rows.push_back(frame::TableRow{
                {std::string(ChannelAccountStateName(transition.from)) + " -> " +
                    ChannelAccountStateName(transition.to),
                 transition.reason,
                 transition.retry_at_ms > 0 ? std::to_string(transition.retry_at_ms) : std::string()}});
        }
        AppendLines(
            lines, frame::RenderTable("最近迁移", columns, rows, theme, frame::Light(), ChannelFrameWidth()));
    }
    return lines;
}

std::vector<std::string> FormatChannelDoctor(const ChannelManager::AccountSnapshot* snapshot,
                                             const lubancode::cli::Theme& theme) {
    std::vector<std::string> lines;
    if (snapshot == nullptr) {
        AppendLines(lines, NoticeLines(theme, {"账号不在册。/channels 先看清单。"}));
        return lines;
    }
    // 键值对框:密钥来源只报名不报值(configuration.md §4);env/file 正常走
    // pass 档,明文/缺失是真问题走 error 档,两行句子按原行各进一条(第二
    // 行 key 空、续排)。
    std::vector<frame::Field> fields;
    switch (snapshot->credential) {
        case lubancode::channel::CredentialSource::FromEnv:
            fields.push_back(SentenceField("密钥: 环境变量来源。ok。", frame::FieldAccent::Pass));
            break;
        case lubancode::channel::CredentialSource::FromFile:
            fields.push_back(
                SentenceField("密钥: 文件来源。ok(文件权限须本用户可读)。", frame::FieldAccent::Pass));
            break;
        case lubancode::channel::CredentialSource::InlinePlaintext:
            fields.push_back(
                SentenceField("密钥: WARNING——config 里放了明文 secret。日志与 trace 会打码,",
                              frame::FieldAccent::Error));
            fields.push_back(frame::Field{"", "但明文留在配置文件里不是久居之计,建议改 secret_env。",
                                          frame::FieldAccent::Error});
            break;
        case lubancode::channel::CredentialSource::Missing:
            fields.push_back(SentenceField("密钥: 未配置(secret_env/secret_file 都没给)。",
                                           frame::FieldAccent::Error));
            fields.push_back(SentenceField("启动闸: CredentialsMissing——不会重试风暴,补上密钥再起。",
                                           frame::FieldAccent::Error));
            break;
    }
    fields.push_back(SentenceField(snapshot->lock_held ? "账号锁: 本进程持有。"
                                                       : "账号锁: 未持有(账号没在跑或已停)。"));
    {
        std::ostringstream row;
        row << "队列: inbox=" << snapshot->inbox_pending << "/"
            << lubancode::channel::InboxLimits{}.max_pending_total << "  dead_letter="
            << snapshot->dead_letter_count;
        fields.push_back(SentenceField(row.str()));
    }
    if (snapshot->pairing_pending > 0) {
        fields.push_back(SentenceField("pairing: " + std::to_string(snapshot->pairing_pending) +
                                       " 条待审,看 /channel pairing list <channel> [account]。"));
    }
    if (IsUnrecoverableAccountState(snapshot->state)) {
        fields.push_back(SentenceField(std::string("状态: ") + ChannelAccountStateName(snapshot->state) +
                                           "——不可自动恢复,须人工处理后再起。",
                                       frame::FieldAccent::Error));
    } else if (snapshot->state == ChannelAccountState::Backoff) {
        fields.push_back(SentenceField("状态: backoff,下一次尝试 retry_at=" +
                                           std::to_string(snapshot->retry_at_ms) + "(第 " +
                                           std::to_string(snapshot->backoff_attempt) + " 次退避)。",
                                       frame::FieldAccent::Skip));
    }
    fields.push_back(
        SentenceField("(体检不发平台请求;Package trust 与 manifest 检查在挂载侧,/package doctor 看。)",
                      frame::FieldAccent::Muted));
    AppendLines(lines,
                frame::RenderKeyValues("渠道 " + snapshot->channel_id + " / 账号 " + snapshot->account_id +
                                           " 体检",
                                       fields, theme, frame::Light(), ChannelFrameWidth()));
    return lines;
}

std::vector<std::string> FormatChannelPairingList(
    const std::string& account_label, const std::vector<ChannelManager::PendingPairingView>* pending,
    std::int64_t now_ms, const lubancode::cli::Theme& theme) {
    std::vector<std::string> lines;
    if (pending == nullptr) {
        AppendLines(lines,
                    NoticeLines(theme, {"本进程没挂 ChannelManager(普通交互形态);配对账在 Gateway 进程里。"}));
        return lines;
    }
    if (pending->empty()) {
        AppendLines(lines, NoticeLines(theme, {"没有待审配对。dm_policy=pairing 的账号收到未知 sender 来信会在这里挂账。"}));
        return lines;
    }
    // 列表:label=sender、value=剩余时长,行尾 hint 挂操作提示(与旧输出逐行
    // 带的操作句一字不差,只是挪进 hint 列)。
    std::vector<frame::ListRow> rows;
    for (const auto& view : *pending) {
        const std::int64_t remaining_ms = view.expires_at_ms - now_ms;
        rows.push_back(frame::ListRow{
            "sender " + view.sender_id,
            remaining_ms <= 0 ? std::string("(已过期)") : "剩 " + std::to_string(remaining_ms / 1000) + " 秒",
            "/channel pairing approve <channel> <code>", frame::Bullet::None});
    }
    AppendLines(lines,
                frame::RenderList(account_label + " 待审配对(" + std::to_string(pending->size()) + " 条)",
                                  rows, theme, frame::Light(), ChannelFrameWidth()));
    return lines;
}

namespace {

void PrintLines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        lubancode::cli::TermOut() << line << "\n";
    }
}

void PrintNotice(const lubancode::cli::Theme& theme, std::initializer_list<std::string> sentences,
                 frame::FieldAccent accent = frame::FieldAccent::None) {
    PrintLines(NoticeLines(theme, sentences, accent));
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

// 用法清单进键值对框:key 取首词("/channel"),value 是其余参数说明——五条
// 用法天然对齐(句子是既有文案,按首空格拆列,批 2 裁量 1/2 同款)。
void PrintChannelUsage(const lubancode::cli::Theme& theme) {
    std::vector<frame::Field> fields;
    for (const std::string& line : {std::string("/channel show <channel> [account]"),
                                    std::string("/channel doctor <channel> [account]"),
                                    std::string("/channel start|stop|restart <channel> [account]"),
                                    std::string("/channel pairing list <channel> [account]"),
                                    std::string("/channel pairing approve|reject <channel> [account] <code>")}) {
        const std::size_t space = line.find(' ');
        fields.push_back(frame::Field{line.substr(0, space), TrimAscii(line.substr(space + 1))});
    }
    PrintLines(frame::RenderKeyValues("用法", fields, theme, frame::Light(), ChannelFrameWidth()));
}

// 会话主题优先;装配没递(异常路径/CLI 直调)时按批 2 裁量 4 现起——管道/
// 重定向探测不到真彩自然降 plain。
const lubancode::cli::Theme& ChannelTheme(const ChannelCommandContext& ctx) {
    if (ctx.theme != nullptr) {
        return *ctx.theme;
    }
    static const lubancode::cli::Theme fallback =
        lubancode::cli::ResolveTheme(std::string(), lubancode::cli::DetectConsoleCapability().colors_enabled);
    return fallback;
}

}  // namespace

CommandFlow HandleSlashChannels(const ChannelCommandContext& ctx,
                                const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    const lubancode::cli::Theme& theme = ChannelTheme(ctx);
    const auto* channels = ctx.config != nullptr && ctx.config->channels.empty()
                               ? nullptr
                               : (ctx.config != nullptr ? &ctx.config->channels : nullptr);
    const auto snapshots = ctx.channel_manager != nullptr ? ctx.channel_manager->Snapshots()
                                                          : std::vector<ChannelManager::AccountSnapshot>();
    PrintLines(FormatChannelsOverview(
        channels, ctx.channel_manager != nullptr ? &snapshots : nullptr, theme));
    return CommandFlow::Continue;
}

CommandFlow HandleSlashChannel(const ChannelCommandContext& ctx,
                               const lubancode::cli::ParsedSlashCommand& parsed) {
    const ParsedChannelCommand command = ParseChannelCommand(parsed.args);
    const lubancode::cli::Theme& theme = ChannelTheme(ctx);
    const auto* channels = ctx.config != nullptr && !ctx.config->channels.empty()
                               ? &ctx.config->channels
                               : nullptr;
    switch (command.action) {
        case ChannelCommandAction::Overview:
            HandleSlashChannels(ctx, parsed);
            return CommandFlow::Continue;
        case ChannelCommandAction::Show: {
            if (command.channel_id.empty()) {
                PrintChannelUsage(theme);
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
            PrintLines(FormatChannelShow(found, theme));
            if (found == nullptr && ctx.channel_manager == nullptr) {
                PrintNotice(theme, {"本进程没挂 ChannelManager(普通交互形态);/channels 看配置侧。"});
            }
            return CommandFlow::Continue;
        }
        case ChannelCommandAction::Doctor: {
            if (command.channel_id.empty()) {
                PrintChannelUsage(theme);
                return CommandFlow::Continue;
            }
            std::string resolved;
            const ChannelManager::AccountSnapshot* found =
                FindSnapshot(ctx.channel_manager, channels, command.channel_id,
                             command.account_id, &resolved);
            if (found == nullptr && ctx.channel_manager != nullptr) {
                // 命中的是"没起"的账号:拿配置侧信息给一份纯配置体检。
                PrintNotice(theme, {"账号没在跑,只按配置侧体检(" + command.channel_id + ")。"});
            }
            PrintLines(FormatChannelDoctor(found, theme));
            if (found == nullptr && ctx.channel_manager == nullptr) {
                PrintNotice(theme, {"本进程没挂 ChannelManager(普通交互形态)。"});
            }
            return CommandFlow::Continue;
        }
        case ChannelCommandAction::Start:
        case ChannelCommandAction::Stop:
        case ChannelCommandAction::Restart: {
            if (command.channel_id.empty()) {
                PrintChannelUsage(theme);
                return CommandFlow::Continue;
            }
            if (ctx.channel_manager == nullptr) {
                // configuration.md §3:/channel start 不改变进程形态。
                PrintNotice(theme, {"本进程是普通交互形态,渠道账号不在这里起。要常驻渠道,跑 "
                                    "lubancode gateway run(阶段 9 落地);普通进程只读状态。"});
                return CommandFlow::Continue;
            }
            std::string resolved;
            const bool has_account =
                FindSnapshot(ctx.channel_manager, channels, command.channel_id,
                             command.account_id, &resolved) != nullptr ||
                !command.account_id.empty();
            const std::string account = !command.account_id.empty() ? command.account_id : resolved;
            if (account.empty()) {
                PrintNotice(theme, {"定位不到账号(给全 <channel> <account> 再试)。"},
                            frame::FieldAccent::Error);
                return CommandFlow::Continue;
            }
            (void)has_account;
            const auto error = command.action == ChannelCommandAction::Start
                                   ? ctx.channel_manager->StartAccount(command.channel_id, account)
                                   : command.action == ChannelCommandAction::Stop
                                         ? ctx.channel_manager->StopAccount(command.channel_id, account)
                                         : ctx.channel_manager->RestartAccount(command.channel_id, account);
            if (error.has_value()) {
                PrintNotice(theme, {"失败: " + *error}, frame::FieldAccent::Error);
            } else {
                PrintNotice(theme, {"已提交(" + command.channel_id + "/" + account + ")。"});
            }
            return CommandFlow::Continue;
        }
        case ChannelCommandAction::PairingList:
        case ChannelCommandAction::PairingApprove:
        case ChannelCommandAction::PairingReject: {
            if (command.channel_id.empty()) {
                PrintChannelUsage(theme);
                return CommandFlow::Continue;
            }
            if (ctx.channel_manager == nullptr) {
                // 与 start 同规(§3):普通交互进程只读,不碰配对账。
                PrintNotice(theme, {"本进程是普通交互形态,配对账在 Gateway 进程里。"});
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
                PrintNotice(theme, {"定位不到账号(给全 <channel> <account> 再试)。"},
                            frame::FieldAccent::Error);
                return CommandFlow::Continue;
            }
            if (command.action == ChannelCommandAction::PairingList) {
                const auto pending = ctx.channel_manager->PendingPairings(command.channel_id, account);
                const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count();
                PrintLines(FormatChannelPairingList(command.channel_id + "/" + account, &pending,
                                                    now_ms, theme));
                return CommandFlow::Continue;
            }
            std::string error;
            const auto sender = command.action == ChannelCommandAction::PairingApprove
                                    ? ctx.channel_manager->ApprovePairing(command.channel_id, account,
                                                                          command.code, &error)
                                    : ctx.channel_manager->RejectPairing(command.channel_id, account,
                                                                         command.code, &error);
            if (sender.has_value()) {
                PrintNotice(theme, {std::string(command.action == ChannelCommandAction::PairingApprove
                                                    ? "已批准 "
                                                    : "已拒绝 ") +
                                    *sender + "(" + command.channel_id + "/" + account + ")。"});
            } else {
                PrintNotice(theme, {"失败: " + error}, frame::FieldAccent::Error);
            }
            return CommandFlow::Continue;
        }
        case ChannelCommandAction::Invalid:
            PrintNotice(theme, {"认不得 \"" + command.bad_word + "\"。"}, frame::FieldAccent::Error);
            PrintChannelUsage(theme);
            return CommandFlow::Continue;
    }
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
