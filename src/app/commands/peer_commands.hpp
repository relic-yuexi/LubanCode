// 跨会话传话类 slash 命令:/peers 名册、/send 递话、/peerperm 权限档。
// 命令只借 PeerCommandState 里的引用干活,不拥有 runtime;会话
// (InteractiveSession)在命令执行期间保证存活。
#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)

#include <optional>
#include <string>
#include <vector>

#include "peers/peer_session.hpp"
#include "app/commands/command_flow.hpp"
#include "cli/theme.hpp"

namespace lubancode::app {

// 三个命令共用的窄状态:全部是借用。
struct PeerCommandState {
    std::optional<lubancode::peers::PeerRuntime>& runtime;  // 会话持有,可空
    bool started = false;  // Start() 成功过才有 /peers /send /peerperm 可言
    // 轮内收件池(held 的确认弹问在会话的空闲路径,不在这组命令里)。
    std::vector<lubancode::peers::PeerEnvelope>& ready_messages;
    std::vector<lubancode::peers::PeerEnvelope>& held_stash;
};

// /peers:列名册;真控制台给方向键菜单,管道给纯文本行。
CommandFlow HandlePeersCommand(PeerCommandState& state, const lubancode::cli::Theme& theme,
                               bool spinner_enabled);

// /send <目标> <正文>:按 peer_id 或名字找人,把话递过去;目标不明、缺
// 参数、没起服务各打各的说明。
CommandFlow HandleSendCommand(PeerCommandState& state, const std::string& args,
                              const lubancode::cli::Theme& theme);

// /peerperm [auto|accept|hold|refuse]:看/设来信权限档。
CommandFlow HandlePeerpermCommand(PeerCommandState& state, const std::string& args);

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):跨会话传话域的分派位(peers/send/peerperm)。
// case 体原样自 interactive_session 的大 switch 搬来。
// ---------------------------------------------------------------------------
struct SlashDispatchContext;
CommandFlow HandleSlashPeers(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashSend(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashPeerperm(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
