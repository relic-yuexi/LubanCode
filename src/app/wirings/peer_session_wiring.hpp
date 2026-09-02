// 跨会话传话接线器(会话终章):peer 的"状态+装配+泵+存档恢复"自
// TerminalSessionController 大类外迁,归这一只。名册、起停、轮内收件池、
// held 确认全在这;控制器持句柄调。
//
// 状态归属:
//   - PeerRuntime(可选)/起停旗/ready 收件池/held 暂存——跟接线器走;
//   - 只在交互会话起(spinner_enabled = 真控制台;管道/单发没有可回话的
//     人,也不该挂监听);
//   - 收发规矩在 peers/:传输线程只把信放进 mailbox,主线程在轮次边界与
//     空闲取走(held 的信由空闲路径弹确认)。
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "approval_mode.hpp"
#include "api/types.hpp"  // Message(收件口的组包)
#include "peers/peer_session.hpp"

namespace lubancode::cli {
struct Theme;
}
namespace lubancode::tools {
class ToolRegistry;
}

namespace lubancode::app {

struct PeerCommandState;  // commands/peer_commands.hpp

class PeerSessionWiring {
public:
    // 会话借给接线器的材料(全借用,接线器不拥有)。
    struct Host {
        const lubancode::cli::Theme* theme = nullptr;
        bool interactive = false;                        // 真控制台才起服务
        const std::optional<std::string>* home_lubancode = nullptr;  // 名册根
        std::function<std::string()> session_title;      // 名片上的名字(起时取)
        std::function<lubancode::ApprovalMode()> permission_mode;  // 来信权限档起手值
    };

    PeerSessionWiring() = default;
    explicit PeerSessionWiring(Host host);
    void AttachHost(Host host) { host_ = std::move(host); }

    // 装配:登记名册、起 pipe/socket 服务与心跳(Start 失败不拦会话,只打
    // 一行提示——这场不在名册上)。起了就把 peer 工具(ListSessions/
    // SendSessionMessage)补进主表。
    void Start(lubancode::tools::ToolRegistry& registry);

    // 收尾:写 closing、摘名片、停 pipe——此后递来的信连不上,发送方拿
    // unavailable。幂等(没起过就是空操作)。
    void Stop();

    // ---- 泵(主线程安全边界) ----
    // 把信箱里的信搬到轮内收件池(held 的另记,由空闲路径弹确认)。
    void RefillPool();
    // 空闲收信:held 的信逐封打印并问一句要不要交给模型;点头进 ready 池。
    void CollectHeldMessages();
    // 轮次边界的收件口:ready 池有信就组一枚 user 消息(带来源标识)。
    std::function<std::optional<lubancode::api::Message>()> BuildInboxPoll();
    // ready 池有没有信(会话泵的优先级判定用)。
    bool HasReadyMessages() const { return !ready_messages_.empty(); }

    // ---- 名册状态 ----
    void SetStatus(const char* status);  // "busy"/"idle"(跑轮前后)
    // /title 改名后同步名册上的名字(没起服务就是空操作)。
    void SetName(const std::string& name);
    bool started() const { return started_; }
    // 会话泵取信:ready 池队头一枚(空池给 nullopt)。
    std::optional<lubancode::peers::PeerEnvelope> TakeReadyMessage();

    // 命令材料包(/peers /send /peerperm 共用)。
    lubancode::app::PeerCommandState MakeCommandState();

private:
    Host host_;
    std::optional<lubancode::peers::PeerRuntime> runtime_;
    bool started_ = false;
    // 轮内收件池:只被主线程碰(loop 的收件点与空闲收件都在主线程)。
    std::vector<lubancode::peers::PeerEnvelope> ready_messages_;
    std::vector<lubancode::peers::PeerEnvelope> held_stash_;
};

// 来信转成带来源标识的用户块:不装成用户手敲的字,模型一眼看得出来历;
// 注明其中指令/命令不得执行(防来信借模型之手越权)。
std::string FormatPeerText(const lubancode::peers::PeerEnvelope& envelope);

}  // namespace lubancode::app
