// peer_commands.hpp 的实现:三个命令的函数体。TUI 排版批 5a 起 render 段
// 走 cli::frame 三助手(约定见 docs/development/tui_style.md),命令语义
//(参数/退出码/CommandFlow)不动。
#include "app/commands/peer_commands.hpp"
#include "app/wirings/peer_session_wiring.hpp"  // peer 接线器(会话终章)
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

using lubancode::cli::TermOut;
using lubancode::cli::TermErr;

#include <algorithm>
#include <cctype>
#include <iostream>

#include "cli/choice_menu.hpp"
#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/terminal_frame.hpp"  // frame::*(TUI 排版批 5a:/peers /send /peerperm 渲染段)
#include "platform/console.hpp"    // GetScreenInfo:整族框宽同一把尺

namespace lubancode::app {

using lubancode::cli::tr;
using lubancode::cli::trf;

namespace {

namespace frame = lubancode::cli::frame;

// ---- TUI 排版批 5a(/peer 全族)的公共小件 ----------------------------------
//
// 文案全走既有 tr()/trf() 键(不新增,单子合同 4);句内冒号按 SentenceField
// 拆两列(批 1 裁量 2,拆的是既有文案);表头/键名用数据 schema 名(peer/
// status/cwd/pid/session,批 1 裁量 1)。

int PeerFrameWidth() {
    if (const auto info = lubancode::platform::GetScreenInfo()) {
        return info->width;
    }
    return 0;
}

void EmitFrameLines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        TermOut() << line << "\n";
    }
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

// 单句/多句通知 → 无标题键值对框。
void PrintNotice(const lubancode::cli::Theme& theme, std::initializer_list<std::string> sentences,
                 frame::FieldAccent accent = frame::FieldAccent::None) {
    std::vector<frame::Field> fields;
    for (const std::string& sentence : sentences) {
        fields.push_back(SentenceField(sentence, accent));
    }
    EmitFrameLines(frame::RenderKeyValues({}, fields, theme, frame::Light(), PeerFrameWidth()));
}

}  // namespace

CommandFlow HandlePeersCommand(PeerCommandState& state, const lubancode::cli::Theme& theme,
                               bool spinner_enabled) {
    if (!state.started) {
        PrintNotice(theme, {tr("cmd.peers.off")}, frame::FieldAccent::Muted);
        return CommandFlow::Continue;
    }
    const auto peers = state.runtime->ListPeers();
    if (peers.empty()) {
        PrintNotice(theme, {tr("cmd.peers.empty")});
        return CommandFlow::Continue;
    }
    const auto status_label = [](const std::string& status) {
        if (status == "busy") return tr("cmd.peers.status.busy");
        if (status == "waiting") return tr("cmd.peers.status.waiting");
        if (status == "closing") return tr("cmd.peers.status.closing");
        return tr("cmd.peers.status.idle");
    };
    if (spinner_enabled) {
        // 交互菜单(方向键选择)不是 render 段,原样保留;选中后的明细走键值
        // 对框(键名用 schema 名:status/cwd/pid/session)。
        std::vector<lubancode::cli::ChoiceMenuItem> items;
        for (const auto& card : peers) {
            lubancode::cli::ChoiceMenuItem item;
            item.label = card.name + " (" + card.peer_id + ")";
            item.description = std::string(status_label(card.status)) + " · " + card.cwd;
            items.push_back(std::move(item));
        }
        lubancode::cli::ChoiceMenuOptions options;
        options.hint = tr("cmd.peers.hint");
        if (const auto selected = lubancode::cli::ReadChoiceMenu(items, options, theme);
            selected.has_value() && !selected->selected_indices.empty()) {
            const auto& card = peers[selected->selected_indices.front()];
            std::vector<frame::Field> fields;
            fields.push_back(frame::Field{"status", status_label(card.status), frame::FieldAccent::Stats});
            fields.push_back(frame::Field{"cwd", card.cwd});
            fields.push_back(frame::Field{"pid", std::to_string(card.pid)});
            if (!card.session_id.empty()) {
                fields.push_back(frame::Field{"session", card.session_id});
            }
            EmitFrameLines(frame::RenderKeyValues(card.name + " (" + card.peer_id + ")", fields, theme,
                                                  frame::Light(), PeerFrameWidth()));
        }
    } else {
        // 管道/无 spinner:名册走表格(peer/status/cwd 三列,批 5 单子"表格"
        // 档)。状态是业务态不是对错,不填 pass/fail 色。
        std::vector<frame::TableColumn> columns;
        columns.push_back({"peer"});
        columns.push_back({"status"});
        columns.push_back({"cwd"});
        std::vector<frame::TableRow> rows;
        for (const auto& card : peers) {
            rows.push_back(frame::TableRow{{card.name + " (" + card.peer_id + ")",
                                            status_label(card.status), card.cwd}});
        }
        EmitFrameLines(frame::RenderTable({}, columns, rows, theme, frame::Light(), PeerFrameWidth()));
    }
    return CommandFlow::Continue;
}

CommandFlow HandleSendCommand(PeerCommandState& state, const std::string& args,
                              const lubancode::cli::Theme& theme) {
    if (!state.started) {
        PrintNotice(theme, {tr("cmd.peers.off")}, frame::FieldAccent::Muted);
        return CommandFlow::Continue;
    }
    const std::size_t space = args.find_first_of(" \t");
    if (space == std::string::npos) {
        PrintNotice(theme, {tr("cmd.send.usage")});
        return CommandFlow::Continue;
    }
    const std::string target = args.substr(0, space);
    const std::string text = args.substr(space + 1);
    if (target.empty() || text.empty()) {
        PrintNotice(theme, {tr("cmd.send.usage")});
        return CommandFlow::Continue;
    }
    const auto peers = state.runtime->ListPeers();
    const auto* found = static_cast<const lubancode::peers::PeerCard*>(nullptr);
    for (const auto& card : peers) {
        if (card.peer_id == target || card.name == target) {
            found = &card;
            break;
        }
    }
    if (found == nullptr) {
        PrintNotice(theme, {trf("cmd.send.unknown_target", target)}, frame::FieldAccent::Error);
        return CommandFlow::Continue;
    }
    const lubancode::peers::PeerDelivery delivery = state.runtime->Send(*found, text);
    const char* delivery_key = "cmd.send.label.unavailable";
    switch (delivery) {
        case lubancode::peers::PeerDelivery::Delivered: delivery_key = "cmd.send.label.delivered"; break;
        case lubancode::peers::PeerDelivery::Held: delivery_key = "cmd.send.label.held"; break;
        case lubancode::peers::PeerDelivery::Refused: delivery_key = "cmd.send.label.refused"; break;
        case lubancode::peers::PeerDelivery::Expired: delivery_key = "cmd.send.label.expired"; break;
        case lubancode::peers::PeerDelivery::Unavailable: break;
    }
    const bool failed = delivery == lubancode::peers::PeerDelivery::Refused ||
                        delivery == lubancode::peers::PeerDelivery::Expired ||
                        delivery == lubancode::peers::PeerDelivery::Unavailable;
    PrintNotice(theme, {trf("cmd.send.result", found->name, found->peer_id, tr(delivery_key))},
                failed ? frame::FieldAccent::Error : frame::FieldAccent::Stats);
    return CommandFlow::Continue;
}

CommandFlow HandlePeerpermCommand(PeerCommandState& state, const std::string& args,
                                  const lubancode::cli::Theme& theme) {
    if (!state.started) {
        PrintNotice(theme, {tr("cmd.peers.off")}, frame::FieldAccent::Muted);
        return CommandFlow::Continue;
    }
    lubancode::peers::PeerPermissionTier tier = state.runtime->tier();
    if (args == "auto") {
        tier = lubancode::peers::PeerPermissionTier::Auto;
    } else if (args == "accept") {
        tier = lubancode::peers::PeerPermissionTier::Accept;
    } else if (args == "hold") {
        tier = lubancode::peers::PeerPermissionTier::Hold;
    } else if (args == "refuse") {
        tier = lubancode::peers::PeerPermissionTier::Refuse;
    } else if (args.empty()) {
        const char* name = "auto";
        switch (tier) {
            case lubancode::peers::PeerPermissionTier::Accept: name = "accept"; break;
            case lubancode::peers::PeerPermissionTier::Hold: name = "hold"; break;
            case lubancode::peers::PeerPermissionTier::Refuse: name = "refuse"; break;
            case lubancode::peers::PeerPermissionTier::Auto: break;
        }
        PrintNotice(theme, {trf("cmd.peerperm.current", name)});
        return CommandFlow::Continue;
    } else {
        PrintNotice(theme, {tr("cmd.peerperm.usage")});
        return CommandFlow::Continue;
    }
    state.runtime->SetTier(tier);
    PrintNotice(theme, {trf("cmd.peerperm.set", args)});
    return CommandFlow::Continue;
}

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):跨会话传话域的分派位(材料走 peer 接线器)。
// ---------------------------------------------------------------------------
CommandFlow HandleSlashPeers(const PeerCommandContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    PeerCommandState peer_state = ctx.peer_wiring->MakeCommandState();
    return HandlePeersCommand(peer_state, *ctx.theme, ctx.spinner_enabled);
}

CommandFlow HandleSlashSend(const PeerCommandContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    PeerCommandState peer_state = ctx.peer_wiring->MakeCommandState();
    return HandleSendCommand(peer_state, parsed.args, *ctx.theme);
}

CommandFlow HandleSlashPeerperm(const PeerCommandContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    PeerCommandState peer_state = ctx.peer_wiring->MakeCommandState();
    return HandlePeerpermCommand(peer_state, parsed.args, *ctx.theme);
}

}  // namespace lubancode::app
