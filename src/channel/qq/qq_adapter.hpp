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
#include "channel/qq/qq_media.hpp"
#include "channel/qq/qq_messages.hpp"
#include "channel/qq/qq_spool.hpp"

namespace lubancode::channel::qq {

// manifest 能力声明(§五:首版只宣称 websocket/direct/text/send/credentials,
// 不借全功能表虚报媒体/streaming)。
nlohmann::json QqBotCapabilities();

// 连接状态快照(连接状态单 §三:QQ session 独占平台连接状态,适配器透传
// 结构化快照,宿主负责输出;不另养一份猜测状态)。
struct ConnectionFailure {
    std::string stage;       // 失败发生阶段(kStage*)
    std::string error_code;  // 稳定码
    std::string detail;      // 脱敏说明
    std::int64_t at_ms = 0;
    int attempt = 0;         // 尝试编号(与 BackoffScheduled.attempt 同轮,§四)
};
struct ConnectionSnapshot {
    bool thread_alive = false;   // 网关线程存活(≠ connected)
    bool connected = false;      // 只在 READY/RESUMED 后 true;断线/停止立即 false
    std::string stage;           // 当前阶段(kStage*;未启动 = "idle")
    std::optional<ConnectionFailure> last_failure;      // 当前最近失败(连接成功后清)
    std::vector<ConnectionFailure> failure_history;     // 成功时归档(留最近 8 笔)
    int retry_count = 0;             // BackoffScheduled 的 attempt
    std::int64_t next_retry_at_ms = 0;
    std::int64_t connected_since_ms = 0;  // 本轮在线起点(0 = 未在线)
    std::int64_t updated_at_ms = 0;       // 快照记账时刻
};

class QqBotAdapter final : public ChannelBridgeTransport {
public:
    struct Options {
        std::string channel_id = "qqbot";
        std::string account_id;
        ChannelAccountUserConfig config;       // 账号配置(transport/app_id/…)
        ResolvedChannelCredential credential;  // 进程内持有,不外泄
        std::filesystem::path state_root;      // spool 落位(宿主递进)
        QqHttpFunc http;                       // 生产 MakeDefaultHttpFunc;测试注假
        // 媒体路(Q4):上传/下载用的独立 seam(限额与信令路不同;生产
        // MakeMediaHttpFunc)。空 = 复用 http。
        QqHttpFunc media_http;
        std::int64_t media_hard_timeout_ms = 60'000;  // 下载/上传硬墙
        std::int64_t max_media_bytes = 20 * 1024 * 1024;  // 收发同帽(20 MiB,
                                                          // 三口径取最小)
        std::function<std::unique_ptr<IGatewayTransport>()> transport_factory;
        std::string ca_pem;                    // wss 信任锚(生产探测/配置)
        std::function<std::int64_t()> now_ms;
        // 生产:GET {api_base}/gateway 取 wss URL;测试注固定地址。
        std::string api_base = "https://api.sgroup.qq.com";
        std::string bots_base = "https://bots.qq.com";
        // §四:装配预检确认的信任根加载失败(稳定码 + 脱敏 detail,wiring
        // 从 ResolvedTrustStore 递进来)。非空 = gateway_url_provider 入口
        // 直接短路——本地已知 TLS 不可用就不发 token/gateway 请求,阻断
        // 无效重试;重启/配置变化后重新装配再加载。网络断线不受此拦。
        std::string trust_load_block_code;
        std::string trust_load_block_detail;
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
    // 平台连接状态快照(§三):线程存活/connected/阶段/最近失败/重试账。
    // 脱敏口径:字段全部来自 GatewayEvent 的稳定账,不碰凭据。
    ConnectionSnapshot ConnectionState() const;
    // token 管理器的进程内借用(Q7 菜单/面板发布器共用——单飞刷新不重复
    // 取 token)。借用方不得另开刷新路。
    QqTokenManager* token_manager() { return &token_manager_; }

private:
    // Bridge 帧分派(宿主锁内上下文)。
    void HandleHostFrame(const nlohmann::json& frame);
    void ReplyResult(std::int64_t id, const nlohmann::json& result);
    void ReplyDomainError(std::int64_t id, DomainErrorName name, const std::string& detail);
    void EmitNotification(BridgeMethod method, const nlohmann::json& params);
    // 网关事件落地:spool 先落,再编 channel.inbound 通知进 to_host;
    // 连接事件同步记 ConnectionState 的账。
    void HandleGatewayEvent(const GatewayEvent& event);
    // 起网关/发送线程与 spool。返回 false = spool 开不了账(不虚报 started)。
    bool StartGatewayLocked();
    void StopGatewayLocked(const std::string& reason);
    void SenderLoop();
    // Q6:互动回应队列的消费(PUT /interactions/{id},一次,不重试)。
    void ProcessAcks();
    std::string NextDeliveryId();

    Options options_;
    QqTokenManager token_manager_;
    // 发送队列项:宿主 channel.send 的 request_id + 冻结载荷。Q4:可带一枚
    // 出站媒体(宿主 outbox 冻结的产物引用;发送线程上传后走 msg_type=7)。
    struct PendingSend {
        std::int64_t request_id = 0;
        C2cSendRequest request;
        std::optional<QqOutboundMedia> media;
        int attempts = 0;
    };
    std::vector<PendingSend> send_queue_;  // 由 host_mutex_ 保护(与 to_host 同锁)
    // Q6 互动回应队列(发送线程消费;与 send_queue_ 同锁同唤醒)。
    struct PendingAck {
        std::int64_t request_id = 0;
        std::string interaction_id;
        int code = 0;
    };
    std::vector<PendingAck> ack_queue_;
    std::optional<QqMessageSender> sender_;
    std::optional<QqMediaUploader> uploader_;  // Q4:file_info 两步上传
    std::optional<QqSpoolStore> spool_;

    std::unique_ptr<QqGatewaySession> session_;
    std::unique_ptr<std::thread> gateway_thread_;
    std::unique_ptr<std::thread> sender_thread_;
    std::condition_variable sender_wake_;
    std::atomic<bool> stop_{false};

    // ConnectionState 的账(网关线程写/宿主线程读,独立小锁,不与
    // host_mutex_ 交叉)。
    mutable std::mutex connection_mutex_;
    ConnectionSnapshot connection_;

    std::mutex host_mutex_;         // to_host_ 与 send_queue_ 的账
    std::vector<std::byte> to_host_;
    FrameDecoder host_frame_decoder_;  // 宿主来向帧解码(仅宿主线程喂)

    std::mt19937 delivery_rng_{std::random_device{}()};
    std::atomic<std::uint64_t> delivery_counter_{0};
};

}  // namespace lubancode::channel::qq
