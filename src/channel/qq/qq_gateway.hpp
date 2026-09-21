// QQ 网关事件连接状态机(QQ 机器人接入单 Q1):Hello→Identify/Resume→
// Dispatch/Heartbeat/ACK→Reconnect/Invalid Session→断线退避(§十五
// gateway-events 模块)。
//
// opcode 语义唯一依据官方 payload 页(op0/1/2/6/7/9/10/11 表)。状态迁移
// 显式可见(state_name 观测),不依赖传输层隐式行为。退避序列照
// configuration.md §10:1s,2s,4s,8s,16s,30s,60s 帽 + 10% jitter;连接稳定
// (Ready 后收到首个 Heartbeat ACK)归零。
//
// 线程模型:RunLoop 归一只网关线程;CancelInFlight 供停止方从外部打断
// 在途连接(在途传输持共享所有权——取消方拷走 shared_ptr 再调 Cancel,
// 连接线程清账/析构不产生悬空指针;A08)。
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/qq/qq_proto.hpp"  // kIntentGroupAndC2cEvent(默认 intents)
#include "channel/transport/gateway_transport.hpp"  // 传输 seam(SV-07 迁中立位)

namespace lubancode::channel::qq {

// 连接阶段稳定名(连接状态单 §三:取令牌 → 查询地址 → TCP/TLS →
// WebSocket → Identify/Resume → READY/RESUMED)。阶段事件谁发:
// fetching_token/fetching_gateway_url 归适配器的 provider(它做 HTTP);
// connecting/identifying/connected 归网关状态机;stopped 归收口。
inline constexpr char kStageFetchingToken[] = "fetching_token";
inline constexpr char kStageFetchingGatewayUrl[] = "fetching_gateway_url";
inline constexpr char kStageConnecting[] = "connecting";
inline constexpr char kStageIdentifying[] = "identifying";
// A02:Resume 发出后到 RESUMED 之前的独立阶段——状态显式"恢复中",不提前
// 报 connected(官方 Resume 合同:先补发遗漏事件,补完才下发 RESUMED)。
inline constexpr char kStageResuming[] = "resuming";
inline constexpr char kStageConnected[] = "connected";
inline constexpr char kStageStopped[] = "stopped";

// ---- 迁移期兼容别名(SV-07:连接快照与传输 seam 已升中立位——
// channel/connection_state.hpp 与 channel/transport/gateway_transport.hpp,
// 三平台同款;QQ/企微网关与 app 装配层共用)----
// 旧调用方(channel::qq::ConnectionSnapshot/ConnectionFailure/
// GatewayConnectError/IGatewayTransport/MakeWsTransportFactory,含既有
// 测试册)照旧编译;新代码直接用 channel::/channel::transport:: 命名。
// 全仓 grep 零引用后可删本段。
using ConnectionFailure = channel::ConnectionFailure;
using ConnectionSnapshot = channel::ConnectionSnapshot;
using GatewayConnectError = transport::GatewayConnectError;
using IGatewayTransport = transport::IGatewayTransport;
inline std::function<std::unique_ptr<IGatewayTransport>()> MakeWsTransportFactory(
    std::string ca_pem, transport::TlsTrustMode trust_mode = transport::TlsTrustMode::ExplicitCa) {
    return transport::MakeWsTransportFactory(std::move(ca_pem), trust_mode);
}

// 网关 URL 查询的 HTTP 失败分类(网关 400 诊断单 §四):状态 + 有界 body
// + 白名单诊断头投影 -> 稳定码 + 脱敏说明。纯函数供测试直钉全表。
//   - 区分请求/权限拒绝(gateway_url_bad_request/forbidden)、鉴权失效
//     (unauthorized)、限流(rate_limited)、服务故障(server_error)与响应
//     格式错(bad_response);未知 4xx 只报 status/是否 JSON/平台 code/
//     trace,不猜原因。
//   - 平台原始 message 可能回显敏感值,默认不透传——detail 只带状态码、
//     是否 JSON、平台 code 数值与 trace 标识。
//   - Retry-After 只认纯数字秒(HTTP-date 不解析);合法时给 retry_after_ms
//     (毫秒),RunLoop 退避取 max(阶梯, retry_after) 封 max_backoff_ms。
struct GatewayHttpFailureClass {
    std::string code;
    std::string detail;
    std::int64_t retry_after_ms = 0;
};
// diagnostic_headers 是 QqHttpResponse::diagnostic_headers 同款(白名单
// 投影,小写名);这里不引 qq_http.hpp,保持 gateway 头自足。
GatewayHttpFailureClass ClassifyGatewayHttpFailure(
    int status, const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& diagnostic_headers);

// 出口事件(适配器消费)。
struct GatewayEvent {
    enum class Kind {
        C2cMessageCreate,   // 单聊来信(d 是平台事件体)
        InteractionCreate,  // 互动回调(Q6 按钮点击;d 是平台事件体)
        UnsupportedDispatch, // 收到但未建模的 Dispatch(FRIEND_ADD 等兄弟事件):
                            // detail = 平台事件类型名。宿主须给明确终结记录
                            //(A04:计数/留痕),返回 Persisted 后游标才推进。
        SessionReady,       // Identify 过(含新 session_id)
        SessionResumed,     // Resume 过
        SessionInvalidated, // op9 不可恢复:session 已清,下一轮重新 Identify
        StageChanged,       // 连接阶段推进(stage 带稳定名)
        ConnectFailed,      // 一轮连接尝试没到 READY 就断(stage/error_code/detail = 根因)
        BackoffScheduled,   // 失败后的退避排程(attempt/next_retry_at_ms;不带根因)
        Disconnected,       // 在线过(READY/RESUMED 后)断线(stage/error_code/detail = 根因)
        Stopped,            // RunLoop 收口(停止)
    };
    Kind kind = Kind::Disconnected;
    nlohmann::json c2c_d;      // Kind::C2cMessageCreate 时有值
    nlohmann::json interaction_d;  // Kind::InteractionCreate 时有值
    std::string detail;
    std::int64_t seq = -1;     // Disconnected 时的 last_seq;业务事件带自身 s
    std::string stage;         // StageChanged/ConnectFailed/Disconnected 时有值
    std::string error_code;    // ConnectFailed/Disconnected 的稳定码
    int attempt = 0;           // 尝试编号(1 起):ConnectFailed/Disconnected 与
                               // 随后的 BackoffScheduled 同轮同号(§四)
    std::int64_t next_retry_at_ms = 0;  // BackoffScheduled:下一次尝试时刻
};

// 接收回调的落盘结果(A04:落盘游标)。宿主(适配器)对一条业务事件的
// 处理是否已达到"可终结"状态——网关据此推进"已安全接收的连续序号"
//(durable 游标,Resume 从它起)。
//   - Persisted:已耐久接住(spool 落账)或已有明确终结记录(含"已判
//     不支持"的计数、判无效的留痕)。游标推进。
//   - PersistFailed:这条没接住(如磁盘满/权限拒)。网关不推进 durable
//     游标、不跨过失败事件,按可恢复故障断线退避;Resume 从 durable 游标
//     起,平台补发包含这条——不丢信。
enum class GatewayEventAck {
    Persisted,
    PersistFailed,
};

class QqGatewaySession {
public:
    struct Options {
        std::function<std::unique_ptr<IGatewayTransport>()> transport_factory;
        // 取网关 URL(生产:GET /gateway;测试注入固定 ws://127.0.0.1:...)。
        // 失败带 GatewayConnectError(取令牌/查地址的失败阶段与稳定码)。
        std::function<std::expected<std::string, GatewayConnectError>()> gateway_url_provider;
        // 每次连接取当前 access token(鉴权/刷新归 QqTokenManager)。
        std::function<std::expected<std::string, GatewayConnectError>()> token_provider;
        // Q6 起默认订阅单聊 + 互动(审批按钮回调);测试若要钉旧行为可显式
        // 覆盖为 kIntentGroupAndC2cEvent。
        std::uint32_t intents = kIntentDefaultBot;
        // 业务事件(C2cMessageCreate/InteractionCreate/UnsupportedDispatch)
        // 的回调返回落盘结果;控制/阶段事件恒按 Persisted 消费。null 回调
        // 视同 Persisted(观测型装配零负担)。
        std::function<GatewayEventAck(const GatewayEvent&)> on_event;
        std::function<std::int64_t()> now_ms;
        int hello_timeout_ms = 10'000;
        // A02:鉴权循环的"总期限"(不是单帧超时)。Identify 只认有效 READY;
        // Resume 收补发事件、等 RESUMED,总期限到仍未完成即断线走退避——
        // 不拿一条业务补发冒充上线。测试可调小换 CI 速度。
        int ready_timeout_ms = 10'000;
        int missed_ack_limit = 2;    // 连续 N 次心跳无 ACK 判死线
        int max_backoff_ms = 60'000;
        // 退避时长缩放(测试设 0.001 把秒级阶梯压成毫秒;生产恒 1.0)。
        double backoff_scale = 1.0;
    };

    explicit QqGatewaySession(Options options) : options_(std::move(options)) {}
    ~QqGatewaySession();

    // 网关线程入口。返回 = stop 置位(或致命配置,已报 Disconnected)。
    void RunLoop(std::atomic<bool>* stop);

    // 停止方从外部打断在途连接/读。
    void CancelInFlight();

    // ---- 观测(诊断/测试) ----
    std::string state_name() const;
    // 已收到的最新序号(心跳口径:平台合同"携带客户端收到的最新的 s",
    // 不与落盘承诺混)。
    std::int64_t last_seq() const { return last_seq_.load(); }
    // 已安全接收的连续序号(A04 durable 游标):Resume 从它起。只有宿主
    // 回调对含该序号的业务事件返回 Persisted 后才推进。
    std::int64_t durable_seq() const { return durable_seq_.load(); }
    std::string session_id() const;
    int connect_attempts() const { return connect_attempts_.load(); }
    // A09:未知/服务端不该发的 opcode 记账(会话内累计)——不冒充心跳、
    // 不断连,观测口供测试与诊断对账。
    int unexpected_op_count() const { return unexpected_ops_.load(); }

private:
    enum class State { Idle, Connecting, Authenticating, Resuming, Running, Backoff, Stopped };
    // 一轮连接的生命周期与收口账:stable = 这轮稳定过(收到过 ACK,退避
    // 归零);retry_after_ms = 本轮失败带的服务端 Retry-After 建议(0 = 无)。
    struct RunOutcome {
        bool stable = false;
        std::int64_t retry_after_ms = 0;
    };
    // attempt_number = 本轮尝试编号(1 起);ConnectFailed/Disconnected 事件
    // 带上它,与随后的 BackoffScheduled.attempt 关联同次尝试(§四)。
    RunOutcome RunOneConnection(std::atomic<bool>* stop, bool* session_was_invalidated,
                                int attempt_number);
    void SleepInterruptible(std::atomic<bool>* stop, std::int64_t ms);
    // 出口事件的统一口:null on_event / 非业务事件按 Persisted 消费。
    GatewayEventAck EmitEvent(const GatewayEvent& event);
    // 一条 Dispatch(s>=0)的按序落账:last_seq 先记(已收到);业务事件经
    // 宿主回调,ack==Persisted 才推进 durable 游标;PersistFailed 回 false
    //(调用方按可恢复故障断线,不跨过这条)。
    bool AcceptDispatch(const GatewayPayload& payload);

    Options options_;
    std::atomic<State> state_{State::Idle};
    // 游标两本账(A04):last_seq = 已收到(心跳合同);durable_seq = 已安全
    // 接收的连续序号(Resume 承诺)。写都归网关线程,读可跨线程。
    std::atomic<std::int64_t> last_seq_{-1};
    std::atomic<std::int64_t> durable_seq_{-1};
    // session_id 与在途传输的跨线程访问口:session_id 网关线程写/宿主线程
    // 读(A08:核对线程约束——std::string 无原子性,过锁);in_flight 共享
    // 所有权,取消方在锁外调 Cancel。
    mutable std::mutex session_state_mutex_;
    std::string session_id_;  // 空 = 无可恢复会话(下一轮 Identify)
    std::atomic<int> connect_attempts_{0};
    // A09:未知/服务端不该发的 opcode 计数(叠加自 #112)。
    std::atomic<int> unexpected_ops_{0};
    // A08:在途传输共享所有权(取消方拷 shared_ptr 再调 Cancel)。
    std::mutex in_flight_mutex_;
    std::shared_ptr<IGatewayTransport> in_flight_;
};

}  // namespace lubancode::channel::qq
