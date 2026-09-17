// 飞书长连接网关状态机(飞书/企微设计单 F1,§5.1-§5.6/§5.9):
// HTTP 引导拿 WSS 地址(每轮重连重新引导)→ WS 拨号 → 激活即发 ping →
// 按 PingInterval 心跳 → 读超时 2×interval+5s(每帧重置)→ 事件帧 5 秒
// 拆包重组 → handler 后回 ACK 帧(复用原帧 + code 200,3 秒红线)→ 退避
// 重连(凭据类错不重试)。
//
// 与 QqGatewaySession 的对应关系:退避同一条 1s..60s 阶梯(configuration.md
// §10);"durable 游标"在飞书侧折算为 ACK code——handler 接住(spool 落盘)
// 回 200,没接住回 500 让平台重推(§5.6),宿主 ingress 按 provider_event_id
// 去重兜底。飞书无 session/resume 合同,断线即重引导重拨。
//
// 线程模型照 QQ:RunLoop 归一只网关线程;CancelInFlight 供停止方打断在途
// 连接(在途传输持共享所有权)。
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/feishu/feishu_frame.hpp"
#include "channel/feishu/feishu_proto.hpp"
#include "channel/transport/ws_client.hpp"

namespace lubancode::channel::feishu {

// 连接阶段稳定名(连接状态快照的 stage 字段;QQ 的 fetching_token/
// identifying 族是 QQ 侧口径,飞书无 WS 鉴权帧,阶段为:引导 → 拨号 →
// 在线 → 收口)。
inline constexpr char kStageBootstrapping[] = "bootstrapping";
inline constexpr char kStageConnecting[] = "connecting";
inline constexpr char kStageConnected[] = "connected";
inline constexpr char kStageStopped[] = "stopped";

// 连接失败的稳定账:失败阶段 + 稳定错误码 + 脱敏说明 + 是否凭据类错
//(§5.9:AuthFailed/Forbidden/ExceedConnLimit 客户端错不可重试)。
struct FeishuConnectError {
    std::string stage;        // kStage* 之一
    std::string error_code;   // 稳定码
    std::string detail;       // 脱敏人话(不带 secret/token/URL query)
    bool non_retryable = false;  // true = 凭据类客户端错,RunLoop 不再重连
};

// 引导产物(适配器的 endpoint_provider 递进来;HTTP 归适配器)。类型即
// feishu_proto 的 ParseBootstrapResponse 产物——单一形状,不在网关层再造
// 同形结构。
using FeishuEndpoint = FeishuBootstrapEndpoint;

// 出口事件(适配器消费)。
struct FeishuGatewayEvent {
    enum class Kind {
        MessageReceive,   // im.message.receive_v1 的事件载荷 JSON(已拼装完整);
                          // handler 返回的 ack 决定回平台的 ACK code
        UnsupportedEvent, // card 帧 / 未知事件类型 / 无效载荷:明确终结
                          //(detail 留痕),回 ACK 200 防重推
        Connected,        // WS 拨通 + 激活 ping 已发(§5.4"激活连接即发 ping")
        StageChanged,     // 连接阶段推进(stage 带稳定名)
        ConnectFailed,    // 一轮连接没到 Connected 就断(non_retryable 标记凭据类)
        BackoffScheduled, // 失败后的退避排程(attempt/next_retry_at_ms;不带根因)
        Disconnected,     // 在线过(Connected 后)断线(stage/error_code/detail = 根因)
        Stopped,          // RunLoop 收口(停止/致命凭据错)
    };
    Kind kind = Kind::Disconnected;
    nlohmann::json event_payload;  // Kind::MessageReceive 时有值
    std::string detail;
    std::string stage;             // StageChanged/ConnectFailed/Disconnected 时有值
    std::string error_code;        // ConnectFailed/Disconnected 的稳定码
    bool non_retryable = false;    // ConnectFailed 的凭据类标记
    int attempt = 0;               // 尝试编号(1 起),ConnectFailed 与 BackoffScheduled 同轮
    std::int64_t next_retry_at_ms = 0;  // BackoffScheduled:下一次尝试时刻
};

// handler 对业务事件的处理结果:Ok → ACK 200(平台不再重推);Retry →
// ACK 500(平台重推,宿主 ingress 去重兜底)。§5.6"3 秒红线"由 handler
// 侧纪律保证(map + spool 落盘 + emit,远在红线内)。
enum class FeishuGatewayAck { Ok, Retry };

// 网关传输 seam(测试注入假流;生产 MakeFeishuWsTransportFactory)。飞书
// 消息全走 BinaryMessage(§5.2),故是 SendBinary 而非 SendText。
class IFeishuGatewayTransport {
public:
    virtual ~IFeishuGatewayTransport() = default;
    virtual std::expected<void, FeishuConnectError> Connect(const std::string& url) = 0;
    virtual std::expected<void, std::string> SendBinary(const std::string& bytes) = 0;
    virtual std::expected<std::string, transport::WsError> ReadMessage(int timeout_ms) = 0;
    virtual void Cancel() = 0;
    virtual void Close(std::uint16_t code, const std::string& reason) = 0;
};

// 生产传输工厂:真 WsClient(明文 ws:// 与 wss:// 同一路,ca_pem 供 wss;
// trust_mode 透传 TLS 层)。升级失败时从 WsError.handshake_headers 裁决
// Handshake-Status/Handshake-Autherrcode(§5.2)。
std::function<std::unique_ptr<IFeishuGatewayTransport>()> MakeFeishuWsTransportFactory(
    std::string ca_pem,
    transport::TlsTrustMode trust_mode = transport::TlsTrustMode::ExplicitCa);

// WS 升级失败的稳定分型(§5.2):Handshake-Status 514=AuthFailed、403=
// Forbidden、缺省回退 HTTP 码;Handshake-Autherrcode 1000040350=
// ExceedConnLimit。客户端错(凭据/超限)non_retryable,服务端错可重试。
// 纯函数供测试直钉全表。
FeishuConnectError ClassifyFeishuHandshakeFailure(const transport::WsError& error);

class FeishuGatewaySession {
public:
    struct Options {
        std::function<std::unique_ptr<IFeishuGatewayTransport>()> transport_factory;
        // 每轮连接重新引导(设计单 §五"每次重连重新引导"):适配器闭包做
        // HTTP,失败带 FeishuConnectError(阶段 + 稳定码 + non_retryable)。
        std::function<std::expected<FeishuEndpoint, FeishuConnectError>()> endpoint_provider;
        // 事件回调;null 视同 Ok(观测型装配零负担)。
        std::function<FeishuGatewayAck(const FeishuGatewayEvent&)> on_event;
        std::function<std::int64_t()> now_ms;
        // PingInterval 夹取范围(§5.4 默认 120s;pong 载荷的 ClientConfig
        // 可覆盖——异常值夹回 sane 区间,不拿 0/负数掐死心跳)。
        std::int64_t min_ping_interval_secs = 5;
        std::int64_t max_ping_interval_secs = 600;
        // 分片拼装窗(§5.5:5 秒;缺片到期静默弃)。测试可压小换 CI 速度。
        std::int64_t fragment_window_ms = 5'000;
        int max_backoff_ms = 60'000;
        // 退避时长缩放(测试设 0.001 把秒级阶梯压成毫秒;生产恒 1.0)。
        double backoff_scale = 1.0;
    };

    explicit FeishuGatewaySession(Options options) : options_(std::move(options)) {}
    ~FeishuGatewaySession();

    // 网关线程入口。返回 = stop 置位,或致命(凭据类)错误已报账。
    void RunLoop(std::atomic<bool>* stop);

    // 停止方从外部打断在途连接/读。
    void CancelInFlight();

    // ---- 观测(诊断/测试) ----
    std::string state_name() const;
    int connect_attempts() const { return connect_attempts_.load(); }
    // 明确终结的未建模事件计数(card/未知事件类型/无效载荷)。
    std::uint64_t unsupported_event_count() const {
        return unsupported_events_.load();
    }
    // 分片缺片到期静默弃的组数(§5.5 观测口)。
    std::uint64_t dropped_fragment_groups() const {
        return dropped_fragments_.load();
    }

private:
    enum class State { Idle, Connecting, Running, Backoff, Stopped };
    // 一轮连接的生命周期账:stable = 这轮稳定过(收过任一完整帧,退避
    // 归零);fatal = 凭据类错(RunLoop 不再重连)。
    struct RunOutcome {
        bool stable = false;
        bool fatal = false;
    };

    RunOutcome RunOneConnection(std::atomic<bool>* stop, int attempt_number);
    void SleepInterruptible(std::atomic<bool>* stop, std::int64_t ms);
    FeishuGatewayAck EmitEvent(const FeishuGatewayEvent& event);
    // 一条数据帧的拆包重组与派发:sum<=1 直发;sum>1 按 message_id 缓存,
    // 拼齐才派发(§5.5)。返回 nullopt = 正常消化;值 = 致命错误码(发送
    // 失败一类,调用方断线)。
    std::optional<std::string> HandleDataFrame(const FeishuFrame& frame);
    // 完整事件载荷的派发 + ACK 帧回写(§5.6:复用原帧 + biz_rt + code)。
    // received_at_ms 是这条(组首片)帧到账时刻,biz_rt 记处理毫秒差。
    std::optional<std::string> DeliverEvent(const std::string& payload,
                                            const FeishuFrame& original_frame,
                                            std::int64_t received_at_ms);
    void PurgeExpiredFragments(std::int64_t now_ms);
    std::optional<std::int64_t> NextFragmentExpiryMs(std::int64_t now_ms) const;
    std::int64_t PingIntervalMs() const;
    std::int64_t ReadTimeoutMs() const;  // 2×pingInterval + 5s(§5.4)
    std::string SendPing(std::int32_t service);

    Options options_;
    std::atomic<State> state_{State::Idle};
    std::atomic<int> connect_attempts_{0};
    std::atomic<std::uint64_t> unsupported_events_{0};
    std::atomic<std::uint64_t> dropped_fragments_{0};
    // 当前连接的心跳节拍(网关线程独占写;RunOneConnection 内的局部账
    // 迁到成员是为 pong 覆盖 PingInterval 时两处共用)。
    std::int64_t ping_interval_ms_ = 120'000;
    std::int64_t service_id_ = 0;
    std::uint64_t next_seq_id_ = 1;
    // 分片拼装账(网关线程独占)。
    struct FragmentGroup {
        std::int64_t sum = 1;
        std::map<std::int64_t, std::string> parts;  // seq -> payload 片
        std::int64_t first_seen_ms = 0;
    };
    std::map<std::string, FragmentGroup> fragments_;
    // 本轮连接的传输(网关线程独占使用;Cancel 从停止线程来)。
    IFeishuGatewayTransport* transport_ = nullptr;
    // A08:在途传输共享所有权(取消方拷 shared_ptr 再调 Cancel)。
    std::mutex in_flight_mutex_;
    std::shared_ptr<IFeishuGatewayTransport> in_flight_;
};

}  // namespace lubancode::channel::feishu
