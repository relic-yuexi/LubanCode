// 跨会话传话(同机首版):一场会话的传话运行时(PeerRuntime)。
//
// 把名册(peer_registry)、信箱(peer_mailbox)、传输(platform/peer_transport_*)
// 拼成一个进程内单例式的外观,main.cpp 只跟这一层打交道:
//   Start()  —— 登记名片、起 pipe/socket 服务线程、起心跳线程;
//   Stop()   —— 写 closing、摘名片、停服务(此后发来的消息连不上,
//               发送方自然拿到 unavailable);
//   SetStatus/SetName —— 名片字段变了就重写;
//   ListPeers —— 名册(顺手清陈条);
//   Send —— 给某场会话递一张字条,拿回 delivered/held/refused/expired/
//               unavailable;
//   DrainIncoming —— 主线程在轮次边界/空闲时取走待读的信。
//
// 收件时机与权限的规矩(规格"收件时机""权限"两节)落在这里:
//   - 传输线程只做防线 + 入队(PeerMailbox),不碰 history、不碰终端;
//   - hold 的信入队时记一笔,取走后由 main.cpp 弹确认,用户点头才交给
//     模型;点头与否都不影响传输层已经回掉的 held;
//   - refuse 档直接回绝,不入队;
//   - 默认档(auto)按两边权限模式与 cwd 距离算(DefaultReceiveTier)。
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "peers/peer_mailbox.hpp"
#include "peers/peer_registry.hpp"
#include "platform/peer_transport.hpp"

namespace lubancode::peers {

// 取走的一条来信:envelope + 它进来时是不是被扣住(hold)了。
struct PeerIncoming {
    PeerEnvelope envelope;
    bool held = false;
};

struct PeerRuntimeOptions {
    std::filesystem::path registry_dir;      // 名册目录(用户级)
    std::string name;                        // 显示名(可由 /title 派生)
    std::string session_id;                  // 会话存档 id,可为空
    std::string cwd;                         // 会话工作目录
    std::function<int()> permission_mode;    // 本场确认档:0=confirm 1=auto 2=yolo
};

class PeerRuntime {
public:
    explicit PeerRuntime(PeerRuntimeOptions options);
    ~PeerRuntime();
    PeerRuntime(const PeerRuntime&) = delete;
    PeerRuntime& operator=(const PeerRuntime&) = delete;

    // 登记 + 起线程。失败(目录建不了、端口被占)返回 false,会话照常
    // 跑,只是这场"不在名册上"。
    bool Start(std::string* error = nullptr);
    void Stop();

    void SetStatus(const std::string& status);  // idle | busy | waiting | closing
    void SetName(const std::string& name);
    void SetCwd(const std::string& cwd);

    const PeerCard& self() const { return own_; }
    PeerPermissionTier tier() const { return tier_.load(); }
    void SetTier(PeerPermissionTier tier) { tier_.store(tier); }

    std::vector<PeerCard> ListPeers() const;

    // 给 target 递一张字条。message_id 自动生成;reply_to 可空。
    PeerDelivery Send(const PeerCard& target, const std::string& text,
                      const std::optional<std::string>& reply_to = std::nullopt);

    // 主线程取走待读的信(原顺序),内部清空。held 的信由调用方弹确认。
    std::vector<PeerIncoming> DrainIncoming();

private:
    std::string HandleRequestOnTransportThread(const std::string& payload);
    void RewriteCard();  // 心跳线程/主线程共用,写名片

    PeerRuntimeOptions options_;
    PeerCard own_;
    PeerRegistry registry_;
    PeerMailbox mailbox_;
    platform::PeerPipeServer server_;
    std::thread heartbeat_thread_;
    std::atomic<bool> running_{false};
    std::atomic<PeerPermissionTier> tier_{PeerPermissionTier::Auto};
    mutable std::mutex card_mutex_;                   // own_ 的读写(心跳线程/主线程)
    std::mutex held_mutex_;                           // held_ids_(传输线程写,Drain 读)
    std::unordered_set<std::string> held_ids_;
};

// 平台默认 endpoint:Windows 是具名管道名,POSIX 是临时目录下的 socket 路径。
std::string DefaultPeerEndpoint(const std::string& peer_id);

}  // namespace lubancode::peers
