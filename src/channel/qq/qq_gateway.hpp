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
// 在途连接(经原子指针只调 Cancel,不触碰其余状态)。
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/qq/qq_proto.hpp"  // kIntentGroupAndC2cEvent(默认 intents)
#include "channel/qq/qq_ws_client.hpp"

namespace lubancode::channel::qq {

// 连接阶段稳定名(连接状态单 §三:取令牌 → 查询地址 → TCP/TLS →
// WebSocket → Identify/Resume → READY/RESUMED)。阶段事件谁发:
// fetching_token/fetching_gateway_url 归适配器的 provider(它做 HTTP);
// connecting/identifying/connected 归网关状态机;stopped 归收口。
inline constexpr char kStageFetchingToken[] = "fetching_token";
inline constexpr char kStageFetchingGatewayUrl[] = "fetching_gateway_url";
inline constexpr char kStageConnecting[] = "connecting";
inline constexpr char kStageIdentifying[] = "identifying";
inline constexpr char kStageConnected[] = "connected";
inline constexpr char kStageStopped[] = "stopped";

// 连接失败的稳定账:失败阶段 + 稳定错误码 + 脱敏说明。provider/transport
// 的失败都折成它,适配器按它记"最近失败"(退避事件不改写)。
struct GatewayConnectError {
    std::string stage;       // kStage* 之一
    std::string error_code;  // 稳定码(见 qq_gateway.cpp 的码表注释)
    std::string detail;      // 脱敏人话(不带 token/secret/响应体)
    // §四:服务端 Retry-After 的建议退避下限(毫秒;0 = 无)。RunLoop 的
    // 退避取 max(阶梯值, retry_after_ms) 再封 max_backoff_ms 帽——429 服从
    // 有效 Retry-After 且有上限。
    std::int64_t retry_after_ms = 0;
};

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
    std::int64_t seq = -1;     // Disconnected 时的 last_seq
    std::string stage;         // StageChanged/ConnectFailed/Disconnected 时有值
    std::string error_code;    // ConnectFailed/Disconnected 的稳定码
    int attempt = 0;           // 尝试编号(1 起):ConnectFailed/Disconnected 与
                               // 随后的 BackoffScheduled 同轮同号(§四)
    std::int64_t next_retry_at_ms = 0;  // BackoffScheduled:下一次尝试时刻
};

// 网关传输 seam(测试注入假流;生产 MakeWsTransportFactory)。
class IGatewayTransport {
public:
    virtual ~IGatewayTransport() = default;
    virtual std::expected<void, GatewayConnectError> Connect(const std::string& url) = 0;
    virtual std::expected<void, std::string> SendText(const std::string& text) = 0;
    virtual std::expected<std::string, WsError> ReadMessage(int timeout_ms) = 0;
    virtual void Cancel() = 0;
    virtual void Close(std::uint16_t code, const std::string& reason) = 0;
};

// 生产传输工厂:真 WsClient(明文 ws:// 与 wss:// 同一路,ca_pem 供 wss;
// trust_mode 透传 TLS 层,见 qq_tls.hpp)。
std::function<std::unique_ptr<IGatewayTransport>()> MakeWsTransportFactory(
    std::string ca_pem, TlsTrustMode trust_mode = TlsTrustMode::ExplicitCa);

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
        std::function<void(const GatewayEvent&)> on_event;
        std::function<std::int64_t()> now_ms;
        int hello_timeout_ms = 10'000;
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
    std::int64_t last_seq() const { return last_seq_.load(); }
    std::string session_id() const;
    int connect_attempts() const { return connect_attempts_.load(); }

private:
    enum class State { Idle, Connecting, Authenticating, Running, Backoff, Stopped };
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

    Options options_;
    std::atomic<State> state_{State::Idle};
    std::atomic<std::int64_t> last_seq_{-1};
    std::string session_id_;  // 空 = 无可恢复会话(下一轮 Identify)
    std::atomic<int> connect_attempts_{0};
    std::atomic<IGatewayTransport*> in_flight_{nullptr};
};

}  // namespace lubancode::channel::qq
