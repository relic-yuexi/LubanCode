// peer_commands.hpp 的实现:三个命令的函数体,原样搬自会话主循环的
// slash case,行为一字未改。
#include "app/commands/peer_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

using lubancode::cli::TermOut;
using lubancode::cli::TermErr;

#include <iostream>

#include "cli/choice_menu.hpp"
#include "cli/console_input.hpp"
#include "cli/i18n.hpp"

namespace lubancode::app {

using lubancode::cli::tr;
using lubancode::cli::trf;

CommandFlow HandlePeersCommand(PeerCommandState& state, const lubancode::cli::Theme& theme,
                               bool spinner_enabled) {
    if (!state.started) {
        TermOut() << theme.stats << tr("cmd.peers.off") << theme.reset << "\n";
        return CommandFlow::Continue;
    }
    const auto peers = state.runtime->ListPeers();
    if (peers.empty()) {
        TermOut() << tr("cmd.peers.empty") << "\n";
        return CommandFlow::Continue;
    }
    const auto status_label = [](const std::string& status) {
        if (status == "busy") return tr("cmd.peers.status.busy");
        if (status == "waiting") return tr("cmd.peers.status.waiting");
        if (status == "closing") return tr("cmd.peers.status.closing");
        return tr("cmd.peers.status.idle");
    };
    if (spinner_enabled) {
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
            TermOut() << theme.tool_line << card.name << " (" << card.peer_id << ")" << theme.reset << "\n"
                      << theme.stats << "  " << status_label(card.status) << " · cwd " << card.cwd << "\n"
                      << "  pid " << card.pid
                      << (card.session_id.empty() ? std::string() : " · session " + card.session_id)
                      << theme.reset << "\n";
        }
    } else {
        for (const auto& card : peers) {
            TermOut() << "- " << card.name << " (" << card.peer_id << ") · " << status_label(card.status)
                      << " · " << card.cwd << "\n";
        }
    }
    return CommandFlow::Continue;
}

CommandFlow HandleSendCommand(PeerCommandState& state, const std::string& args,
                              const lubancode::cli::Theme& theme) {
    if (!state.started) {
        TermOut() << theme.stats << tr("cmd.peers.off") << theme.reset << "\n";
        return CommandFlow::Continue;
    }
    const std::size_t space = args.find_first_of(" \t");
    if (space == std::string::npos) {
        TermOut() << tr("cmd.send.usage") << "\n";
        return CommandFlow::Continue;
    }
    const std::string target = args.substr(0, space);
    const std::string text = args.substr(space + 1);
    if (target.empty() || text.empty()) {
        TermOut() << tr("cmd.send.usage") << "\n";
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
        TermOut() << theme.error << trf("cmd.send.unknown_target", target) << theme.reset << "\n";
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
    TermOut() << (failed ? theme.error : theme.stats)
              << trf("cmd.send.result", found->name, found->peer_id, tr(delivery_key)) << theme.reset << "\n";
    return CommandFlow::Continue;
}

CommandFlow HandlePeerpermCommand(PeerCommandState& state, const std::string& args) {
    if (!state.started) {
        TermOut() << tr("cmd.peers.off") << "\n";
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
        TermOut() << trf("cmd.peerperm.current", name) << "\n";
        return CommandFlow::Continue;
    } else {
        TermOut() << tr("cmd.peerperm.usage") << "\n";
        return CommandFlow::Continue;
    }
    state.runtime->SetTier(tier);
    TermOut() << trf("cmd.peerperm.set", args) << "\n";
    return CommandFlow::Continue;
}

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):跨会话传话域的分派位。
// ---------------------------------------------------------------------------
CommandFlow HandleSlashPeers(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    PeerCommandState peer_state{*ctx.peer_runtime, *ctx.peer_started, *ctx.peer_ready_messages,
                                 *ctx.peer_held_stash};
    return HandlePeersCommand(peer_state, *ctx.theme, ctx.spinner_enabled);
}

CommandFlow HandleSlashSend(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    PeerCommandState peer_state{*ctx.peer_runtime, *ctx.peer_started, *ctx.peer_ready_messages,
                                 *ctx.peer_held_stash};
    return HandleSendCommand(peer_state, parsed.args, *ctx.theme);
}

CommandFlow HandleSlashPeerperm(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    PeerCommandState peer_state{*ctx.peer_runtime, *ctx.peer_started, *ctx.peer_ready_messages,
                                 *ctx.peer_held_stash};
    return HandlePeerpermCommand(peer_state, parsed.args);
}

}  // namespace lubancode::app
