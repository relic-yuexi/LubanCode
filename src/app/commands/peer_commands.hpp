// 跨会话传话类 slash 命令:/peers 名册、/send 递话、/peerperm 权限档。
// 命令只借 PeerCommandState 里的引用干活,不拥有 runtime;会话
// (InteractiveSession)在命令执行期间保证存活。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "agent/peer_session.hpp"
#include "app/commands/command_flow.hpp"
#include "cli/theme.hpp"

namespace lubancode::app {

// 三个命令共用的窄状态:全部是借用。
struct PeerCommandState {
    std::optional<lubancode::agent::PeerRuntime>& runtime;  // 会话持有,可空
    bool started = false;  // Start() 成功过才有 /peers /send /peerperm 可言
    // 轮内收件池(held 的确认弹问在会话的空闲路径,不在这组命令里)。
    std::vector<lubancode::agent::PeerEnvelope>& ready_messages;
    std::vector<lubancode::agent::PeerEnvelope>& held_stash;
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

}  // namespace lubancode::app
