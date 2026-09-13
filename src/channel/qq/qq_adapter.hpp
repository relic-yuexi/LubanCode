// QqBotAdapter:QQ 进程内适配器(QQ 机器人接入单 Q1,§十五定案件)。
//
// 定案(todo §十五):原生 C++ 进程内直连——本类实现 ChannelBridgeTransport
// 字节面,Bridge 隔离语义原样保留(握手/method/帧协议/关停次序),宿主的
// ChannelManager/ingress/pairing/路由一行不改;"sidecar"从子进程换成进程内
// 线程组(一只网关线程 + 一只发送线程),无 spawn、无 stdio、无环境继承。
//
// 线程与锁:宿主在 ChannelManager 锁内调 WriteToSidecar/DrainFromSidecar,
// 两条口只做入队/取队,绝不在锁内碰网络;IO 归内部线程。to_host_ 缓冲由
// 自家 mutex 保护。凭据(ResolvedChannelCredential)只在进程内持有,不落
// 日志、不进任何错误文案。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "channel/channel_config.hpp"
#include "channel/credentials.hpp"
#include "channel/manager.hpp"
#include "channel/qq/qq_auth.hpp"
#include "channel/qq/qq_gateway.hpp"
#include "channel/qq/qq_http.hpp"
#include "channel/qq/qq_messages.hpp"
#include "channel/qq/qq_spool.hpp"

namespace lubancode::channel::qq {

// manifest 能力声明(§五:首版只宣称 websocket/direct/text/send/credentials,
// 不借全功能表虚报媒体/streaming)。
nlohmann::json QqBotCapabilities();

class QqBotAdapter final : public ChannelBridgeTransport {
public:
    struct Options {
        std::string channel_id = "qqbot";
        std::string account_id;
        ChannelAccountUserConfig config;       // 账号配置(transport/app_id/…)
        ResolvedChannelCredential credential;  // 进程内持有,不外泄
        std::filesystem::path state_root;      // spool 落位(宿主递进)
        QqHttpFunc http;                       // 生产 MakeDefaultHttpFunc;测试注假
        std::function<std::unique_ptr<IGatewayTransport>()> transport_factory;
        std::string ca_pem;                    // wss 信任锚(生产探测/配置)
        std::function<std::int64_t()> now_ms;
        // 生产:GET {api_base}/gateway 取 wss URL;测试注固定地址。
        std::string api_base = "https://api.sgroup.qq.com";
        std::string bots_base = "https://bots.qq.com";
    };

    explicit QqBotAdapter(Options options);
    ~QqBotAdapter() override;

    QqBotAdapter(const QqBotAdapter&) = delete;
    QqBotAdapter& operator=(const QqBotAdapter&) = delete;

    // ---- ChannelBridgeTransport(宿主在 manager 锁内调;只入队/取队) ----
    void WriteToSidecar(const std::byte* data, std::size_t size) override;
    std::vector<std::byte> DrainFromSidecar() override;

    // ---- 观测(诊断/测试) ----
    std::size_t spool_pending_count() const;
    bool gateway_thread_running() const { return gateway_thread_ != nullptr; }
    std::string gateway_state() const {
        return session_ ? session_->state_name() : std::string("idle");
    }

private:
    // Bridge 帧分派(宿主锁内上下文)。
    void HandleHostFrame(const nlohmann::json& frame);
    void ReplyResult(std::int64_t id, const nlohmann::json& result);
    void ReplyDomainError(std::int64_t id, DomainErrorName name, const std::string& detail);
    void EmitNotification(BridgeMethod method, const nlohmann::json& params);
    // 网关事件落地:spool 先落,再编 channel.inbound 通知进 to_host。
    void HandleGatewayEvent(const GatewayEvent& event);
    // 起网关/发送线程与 spool。返回 false = spool 开不了账(不虚报 started)。
    bool StartGatewayLocked();
    void StopGatewayLocked(const std::string& reason);
    void SenderLoop();
    std::string NextDeliveryId();

    Options options_;
    QqTokenManager token_manager_;
    // 发送队列项:宿主 channel.send 的 request_id + 冻结载荷。
    struct PendingSend {
        std::int64_t request_id = 0;
        C2cSendRequest request;
        int attempts = 0;
    };
    std::vector<PendingSend> send_queue_;  // 由 host_mutex_ 保护(与 to_host 同锁)
    std::optional<QqMessageSender> sender_;
    std::optional<QqSpoolStore> spool_;

    std::unique_ptr<QqGatewaySession> session_;
    std::unique_ptr<std::thread> gateway_thread_;
    std::unique_ptr<std::thread> sender_thread_;
    std::condition_variable sender_wake_;
    std::atomic<bool> stop_{false};

    std::mutex host_mutex_;         // to_host_ 与 send_queue_ 的账
    std::vector<std::byte> to_host_;
    FrameDecoder host_frame_decoder_;  // 宿主来向帧解码(仅宿主线程喂)

    std::mt19937 delivery_rng_{std::random_device{}()};
    std::atomic<std::uint64_t> delivery_counter_{0};
};

}  // namespace lubancode::channel::qq
