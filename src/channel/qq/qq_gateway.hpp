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

#include <nlohmann/json.hpp>

#include "channel/qq/qq_ws_client.hpp"

namespace lubancode::channel::qq {

// 出口事件(适配器消费)。
struct GatewayEvent {
    enum class Kind {
        C2cMessageCreate,   // 单聊来信(d 是平台事件体)
        SessionReady,       // Identify 过(含新 session_id)
        SessionResumed,     // Resume 过
        SessionInvalidated, // op9 不可恢复:session 已清,下一轮重新 Identify
        Disconnected,       // 连接断(detail 带原因)
    };
    Kind kind = Kind::Disconnected;
    nlohmann::json c2c_d;      // Kind::C2cMessageCreate 时有值
    std::string detail;
    std::int64_t seq = -1;     // Disconnected 时的 last_seq
};

// 网关传输 seam(测试注入假流;生产 MakeWsTransportFactory)。
class IGatewayTransport {
public:
    virtual ~IGatewayTransport() = default;
    virtual std::expected<void, std::string> Connect(const std::string& url) = 0;
    virtual std::expected<void, std::string> SendText(const std::string& text) = 0;
    virtual std::expected<std::string, WsError> ReadMessage(int timeout_ms) = 0;
    virtual void Cancel() = 0;
    virtual void Close(std::uint16_t code, const std::string& reason) = 0;
};

// 生产传输工厂:真 WsClient(明文 ws:// 与 wss:// 同一路,ca_pem 供 wss)。
std::function<std::unique_ptr<IGatewayTransport>()> MakeWsTransportFactory(std::string ca_pem);

class QqGatewaySession {
public:
    struct Options {
        std::function<std::unique_ptr<IGatewayTransport>()> transport_factory;
        // 取网关 URL(生产:GET /gateway;测试注入固定 ws://127.0.0.1:...)。
        std::function<std::expected<std::string, std::string>()> gateway_url_provider;
        // 每次连接取当前 access token(鉴权/刷新归 QqTokenManager)。
        std::function<std::expected<std::string, std::string>()> token_provider;
        std::uint32_t intents = kIntentGroupAndC2cEvent;
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
    // 一轮连接生命周期。返回 false = 这轮断了(外层按 attempt 退避重连)。
    bool RunOneConnection(std::atomic<bool>* stop, bool* session_was_invalidated);
    void SleepInterruptible(std::atomic<bool>* stop, std::int64_t ms);

    Options options_;
    std::atomic<State> state_{State::Idle};
    std::atomic<std::int64_t> last_seq_{-1};
    std::string session_id_;  // 空 = 无可恢复会话(下一轮 Identify)
    std::atomic<int> connect_attempts_{0};
    std::atomic<IGatewayTransport*> in_flight_{nullptr};
};

}  // namespace lubancode::channel::qq
