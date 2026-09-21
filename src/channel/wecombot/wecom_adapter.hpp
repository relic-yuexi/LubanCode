// WecombotAdapter:企业微信智能机器人进程内适配器(W1,设计单 §一/§六)。
//
// 定案同 QQ Q1:原生 C++ 进程内直连——本类实现 ChannelBridgeTransport
// 字节面,Bridge 隔离语义原样保留(握手/method/帧协议/关停次序),宿主的
// ChannelManager/ingress/pairing/路由零改动;"sidecar"是进程内线程组
//(一只网关线程 + 一只发送线程)。
//
// 线程与锁:宿主在 ChannelManager 锁内调 WriteToSidecar/DrainFromSidecar,
// 两条口只做入队/取队,不碰网络;IO 归内部线程。Bridge 帧收发(宿主来向
// 解码 + to_host 出站缓冲)归共用件 InProcessBridgeEndpoint(SV-08);发送
// 队列由 send_mutex_ 保护。凭据(ResolvedChannelCredential)只在进程内持
// 有,不落日志、不进错误文案。发送线程不碰 socket(一切帧由网关线程独占
// 写,见 wecom_gateway 头注);它管发送业务面:回话锚(req_id 透传)、
// markdown 分段、限流账、重试与回执分型。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "channel/bridge_endpoint.hpp"
#include "channel/bridge_protocol.hpp"
#include "channel/channel_config.hpp"
#include "channel/credentials.hpp"
#include "channel/frame.hpp"
#include "channel/manager.hpp"
#include "channel/qq/qq_spool.hpp"  // spool 件平台无关(Q1 落 qq 名下),复用
#include "channel/wecombot/wecom_gateway.hpp"

namespace lubancode::channel::wecombot {

// manifest 能力声明(W1 首版:文本进出,不虚报媒体/streaming/互动)。
nlohmann::json WecombotCapabilities();

// 连接状态快照(照 qq::ConnectionSnapshot 的口径:网关独占平台连接状态,
// 适配器透传结构化快照;装配层把它平移给 reporter)。
struct ConnectionFailure {
    std::string stage;       // 失败发生阶段(kStage*)
    std::string error_code;  // 稳定码
    std::string detail;      // 脱敏说明
    std::int64_t at_ms = 0;
    int attempt = 0;
};
struct ConnectionSnapshot {
    bool thread_alive = false;
    bool connected = false;      // 只在订阅成功后 true;断线/停止立即 false
    std::string stage;           // kStage*;未启动 = "idle"
    std::optional<ConnectionFailure> last_failure;
    std::vector<ConnectionFailure> failure_history;  // 成功时归档(留最近 8 笔)
    int retry_count = 0;
    std::int64_t next_retry_at_ms = 0;
    std::int64_t connected_since_ms = 0;
    std::int64_t updated_at_ms = 0;
};

// 发送侧限流账(§六 6.5:单会话回复+推送合计 30 条/分钟、1000 条/小时)。
// 发送线程独占使用;纯账目,时钟由调用方递(now_ms 注入,测试可用假钟)。
class WecomRateLedger {
public:
    struct Limits {
        std::size_t per_minute = 30;
        std::size_t per_hour = 1000;
    };

    explicit WecomRateLedger(Limits limits) : limits_(limits) {}

    // 距下一次允许发送还要等多久(毫秒;0 = 现在就能发)。滑窗按发送时刻
    // 记账:分钟窗满等最老一笔出窗,小时窗同理;两窗取更晚。
    std::int64_t WaitMs(const std::string& conversation_id, std::int64_t now_ms) const;
    // 记一笔发送(写帧成功/被平台接受时记;保守口径也可在递交前记)。
    void Record(const std::string& conversation_id, std::int64_t now_ms);

private:
    Limits limits_;
    mutable std::mutex mutex_;
    mutable std::map<std::string, std::deque<std::int64_t>> sends_;
};

class WecombotAdapter final : public ChannelBridgeTransport {
public:
    struct Options {
        std::string channel_id = "wecombot";
        std::string account_id;
        ChannelAccountUserConfig config;       // 账号配置(app_id 存 BotID)
        ResolvedChannelCredential credential;  // 进程内持有,不外泄
        std::filesystem::path state_root;      // spool 落位(宿主递进)
        WecomTransportFactory transport_factory;
        std::function<std::int64_t()> now_ms;
        std::string endpoint = std::string(kWecomDefaultEndpoint);
        // 网关节拍(生产官方口径;测试全可压小)。
        std::int64_t ping_interval_ms = 30'000;
        int subscribe_timeout_ms = 10'000;
        int missed_ack_limit = 2;
        int max_backoff_ms = 60'000;
        double backoff_scale = 1.0;
        int drain_poll_ms = 200;
        // 发送面。
        int respond_ack_timeout_ms = 5'000;
        int sender_backoff_base_ms = 1'000;  // 逐段重试退避基(1s/2s/4s)
        WecomRateLedger::Limits rate_limits;
        std::size_t max_req_id_anchors = 4096;  // msgid → req_id 锚容量帽
        // 装配预检确认的信任根加载失败(照 QQ §四:非空即短路联网重试)。
        std::string trust_load_block_code;
        std::string trust_load_block_detail;
    };

    explicit WecombotAdapter(Options options);
    ~WecombotAdapter() override;

    WecombotAdapter(const WecombotAdapter&) = delete;
    WecombotAdapter& operator=(const WecombotAdapter&) = delete;

    // ---- ChannelBridgeTransport(宿主在 manager 锁内调;只入队/取队) ----
    void WriteToSidecar(const std::byte* data, std::size_t size) override;
    std::vector<std::byte> DrainFromSidecar() override;

    // ---- 观测(诊断/测试) ----
    std::size_t spool_pending_count() const;
    bool gateway_thread_running() const { return gateway_thread_ != nullptr; }
    std::string gateway_state() const {
        return session_ ? session_->state_name() : std::string("idle");
    }
    ConnectionSnapshot ConnectionState() const;
    // 只记日志不入模型的两本账(§六 6.4 事件回调 / 认不得的帧)。
    std::uint64_t ignored_event_count() const { return ignored_event_count_.load(); }
    std::uint64_t unsupported_frame_count() const { return unsupported_frame_count_.load(); }
    // 故障注入(仅测试):spool 落盘恒败。生产不得调用。
    void SetSpoolAppendFaultForTest(bool fail);

private:
    // 发送队列项:宿主 channel.send 的 request_id + 冻结载荷。
    struct PendingSend {
        std::int64_t request_id = 0;
        std::string conversation_id;
        std::string content;                    // markdown 正文(分段在递交时做)
        std::string anchor_msgid;               // 被动回复锚(来信 msgid)
        std::string outbound_delivery_id;       // 宿主 delivery 账
        int attempts = 0;                       // 整段重试轮数
        std::size_t chunk_cursor = 0;           // 已送出的分段(重发从此起)
    };

    // Bridge 帧分派(宿主锁内上下文)。
    void HandleHostFrame(const nlohmann::json& frame);
    void ReplyResult(std::int64_t id, const nlohmann::json& result);
    void ReplyDomainError(std::int64_t id, DomainErrorName name, const std::string& detail);
    void EmitNotification(BridgeMethod method, const nlohmann::json& params);
    // 网关事件落地:消息先落 spool 再编 channel.inbound 通知;连接事件记
    // ConnectionState 的账。
    void HandleGatewayEvent(const WecomGatewayEvent& event);
    bool StartGatewayLocked();
    void StopGatewayLocked(const std::string& reason);
    void SenderLoop();
    // 发送线程主路:锚查证 → 限流等待 → 分段递交 → 回执分型;重试不重发
    // 已送段(chunk_cursor)。
    void DeliverPending(PendingSend pending);
    std::string NextDeliveryId();
    void RecordAnchor(const std::string& msgid, const std::string& req_id);
    std::string LookupAnchor(const std::string& msgid);
    bool SleepSendInterruptible(std::int64_t ms);

    Options options_;
    std::atomic<bool> stop_{false};

    // spool 件复用 qq 实现(先落盘再上报/ACK 后清理,平台无关)。
    using SpoolStore = channel::qq::QqSpoolStore;
    std::optional<SpoolStore> spool_;

    std::vector<PendingSend> send_queue_;  // send_mutex_ 保护(sender_wake_ 同锁)
    std::unique_ptr<WecomGatewaySession> session_;
    std::unique_ptr<std::thread> gateway_thread_;
    std::unique_ptr<std::thread> sender_thread_;
    std::condition_variable sender_wake_;
    WecomRateLedger rate_;

    // msgid → 回调 req_id 锚(respond 透传合同):网关线程写、发送线程读。
    std::mutex anchor_mutex_;
    std::map<std::string, std::string> anchors_;
    std::deque<std::string> anchor_order_;  // FIFO 淘汰序(max_req_id_anchors 帽)

    std::atomic<std::uint64_t> ignored_event_count_{0};
    std::atomic<std::uint64_t> unsupported_frame_count_{0};

    // ConnectionState 的账(网关线程写/宿主线程读,独立小锁)。
    mutable std::mutex connection_mutex_;
    ConnectionSnapshot connection_;

    // Bridge 帧收发机械(SV-08 共用件):解码循环/出站缓冲/输出锁全在
    // bridge_;本类只剩 HandleHostFrame 业务分派与平台账。
    InProcessBridgeEndpoint bridge_;
    std::mutex send_mutex_;  // send_queue_ 的账

    // delivery id 生成:原子计数 + 时钟拼法,不养 rng——NextDeliveryId 会
    // 被网关线程(入站)与宿主线程(出站兜底)并发调,rand 引擎不线程安全。
    std::atomic<std::uint64_t> delivery_counter_{0};
};

}  // namespace lubancode::channel::wecombot
