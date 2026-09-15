#include "channel/qq/qq_gateway.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>
#include <utility>

#include "channel/qq/qq_proto.hpp"

namespace lubancode::channel::qq {

namespace {

// configuration.md §10 的退避阶梯(秒)。
constexpr int kBackoffSeconds[] = {1, 2, 4, 8, 16, 30, 60};
constexpr int kBackoffSteps = static_cast<int>(sizeof(kBackoffSeconds) / sizeof(int));

// 真 WsClient 的 IGatewayTransport 适配。
class WsGatewayTransport final : public IGatewayTransport {
public:
    WsGatewayTransport(std::string ca_pem, TlsTrustMode trust_mode)
        : ca_pem_(std::move(ca_pem)), trust_mode_(trust_mode) {}

    std::expected<void, GatewayConnectError> Connect(const std::string& url) override {
        WsConnectOptions options;
        options.url = url;
        options.ca_pem = ca_pem_;
        options.trust_mode = trust_mode_;
        auto client = WsClient::Connect(options);
        if (!client.has_value()) {
            return std::unexpected(GatewayConnectError{
                kStageConnecting,
                client.error().error_code.empty() ? WsErrorConnectCode(client.error())
                                                  : client.error().error_code,
                "ws connect: " + client.error().detail});
        }
        client_ = std::move(*client);
        return {};
    }

    std::expected<void, std::string> SendText(const std::string& text) override {
        const auto sent = client_.SendText(text);
        if (!sent.has_value()) {
            return std::unexpected("ws send: " + sent.error().detail);
        }
        return {};
    }

    std::expected<std::string, WsError> ReadMessage(int timeout_ms) override {
        return client_.ReadMessage(timeout_ms);
    }

    void Cancel() override { client_.Cancel(); }

    void Close(std::uint16_t code, const std::string& reason) override {
        (void)client_.Close(code, reason);
    }

private:
    // WsError(细码缺位时)-> 连接段稳定码。TCP 连不上/超时与 WS 升级握手
    // 失败在 WsClient::Connect 一口锅;TLS 细码已透传,其余按 kind 分。
    static std::string WsErrorConnectCode(const WsError& error) {
        switch (error.kind) {
            case WsError::Kind::Timeout:
                return "connect_timeout";
            case WsError::Kind::Protocol:
                return "ws_handshake_failed";
            case WsError::Kind::Closed:
                return "ws_handshake_closed";
            case WsError::Kind::Failed:
                return error.detail.rfind("tls: ", 0) == 0
                           ? std::string(kTlsCodeHandshakeFailed)
                           : std::string("connect_failed");
        }
        return "connect_failed";
    }

    std::string ca_pem_;
    TlsTrustMode trust_mode_ = TlsTrustMode::ExplicitCa;
    WsClient client_;
};

// 组装带阶段/稳定码的连接事件(StageChanged/ConnectFailed/Disconnected 共用)。
GatewayEvent GatewayConnectEvent(GatewayEvent::Kind kind, const std::string& stage,
                                 const std::string& code, const std::string& detail) {
    GatewayEvent event;
    event.kind = kind;
    event.stage = stage;
    event.error_code = code;
    event.detail = detail;
    return event;
}

// 运行期读错误的稳定码(read: 服务端断流/超时/协议错)。
std::string ReadErrorCode(const WsError& error) {
    switch (error.kind) {
        case WsError::Kind::Timeout:
            return "read_timeout";
        case WsError::Kind::Closed:
            return "read_closed";
        case WsError::Kind::Protocol:
            return "read_protocol";
        case WsError::Kind::Failed:
            break;
    }
    return "read_failed";
}

}  // namespace

std::function<std::unique_ptr<IGatewayTransport>()> MakeWsTransportFactory(
    std::string ca_pem, TlsTrustMode trust_mode) {
    return [ca_pem = std::move(ca_pem), trust_mode]() -> std::unique_ptr<IGatewayTransport> {
        return std::make_unique<WsGatewayTransport>(ca_pem, trust_mode);
    };
}

QqGatewaySession::~QqGatewaySession() {
    CancelInFlight();
}

std::string QqGatewaySession::state_name() const {
    switch (state_.load()) {
        case State::Idle:
            return "idle";
        case State::Connecting:
            return "connecting";
        case State::Authenticating:
            return "authenticating";
        case State::Running:
            return "running";
        case State::Backoff:
            return "backoff";
        case State::Stopped:
            return "stopped";
    }
    return "unknown";
}

std::string QqGatewaySession::session_id() const {
    return session_id_;
}

void QqGatewaySession::CancelInFlight() {
    IGatewayTransport* transport = in_flight_.load();
    if (transport != nullptr) {
        transport->Cancel();
    }
}

void QqGatewaySession::SleepInterruptible(std::atomic<bool>* stop, std::int64_t ms) {
    const std::int64_t step = 100;
    for (std::int64_t slept = 0; slept < ms && !stop->load(); slept += step) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<long long>(std::min<std::int64_t>(step, ms - slept))));
    }
}

void QqGatewaySession::RunLoop(std::atomic<bool>* stop) {
    int attempt = 0;
    while (!stop->load()) {
        bool session_invalidated = false;
        state_.store(State::Connecting);
        const bool stable = RunOneConnection(stop, &session_invalidated);
        if (session_invalidated) {
            session_id_.clear();  // op9 不可恢复:下一轮重新 Identify
            GatewayEvent event;
            event.kind = GatewayEvent::Kind::SessionInvalidated;
            event.detail = "invalid session";
            event.seq = last_seq_.load();
            options_.on_event(event);
        }
        if (stop->load()) {
            break;
        }
        // 连接稳定过(收到过 ACK)则退避归零;否则沿阶梯走。
        if (stable) {
            attempt = 0;
        } else {
            attempt = std::min(attempt + 1, kBackoffSteps);
        }
        connect_attempts_.store(attempt);
        const int base_ms = kBackoffSeconds[attempt == 0 ? 0 : attempt - 1] * 1000;
        std::mt19937 rng(static_cast<unsigned>(options_.now_ms() & 0xFFFFFFFF));
        std::uniform_int_distribution<int> jitter(0, base_ms / 10);  // 10% jitter
        const int backoff_ms = static_cast<int>(
            (base_ms + jitter(rng)) * options_.backoff_scale);
        state_.store(State::Backoff);
        // 退避排程单独成事件:只带 attempt 与下次尝试时刻,不带"原因"——
        // 根因归 ConnectFailed/Disconnected,退避不许覆盖它(§三)。
        GatewayEvent backoff;
        backoff.kind = GatewayEvent::Kind::BackoffScheduled;
        backoff.attempt = attempt;
        backoff.next_retry_at_ms = options_.now_ms() + backoff_ms;
        options_.on_event(backoff);
        SleepInterruptible(stop, backoff_ms);
    }
    state_.store(State::Stopped);
    options_.on_event(GatewayEvent{GatewayEvent::Kind::Stopped});
}

bool QqGatewaySession::RunOneConnection(std::atomic<bool>* stop,
                                        bool* session_was_invalidated) {
    auto transport = options_.transport_factory();
    if (!transport) {
        options_.on_event(GatewayConnectEvent(GatewayEvent::Kind::ConnectFailed,
                                              kStageConnecting, "no_transport",
                                              "no transport factory"));
        return false;
    }
    in_flight_.store(transport.get());
    const auto clear_in_flight = [this]() { in_flight_.store(nullptr); };
    bool reached_ready = false;  // 区分 ConnectFailed(没到 READY)与 Disconnected

    // 一轮失败的收口:发 ConnectFailed(未到 READY)或 Disconnected(在线
    // 过才断)——两者都带阶段/稳定码/根因 detail(§三:Disconnected 不丢
    // detail)。
    const auto fail = [&](const std::string& stage, const std::string& code,
                          const std::string& reason) {
        clear_in_flight();
        transport->Close(1000, "session end");
        options_.on_event(GatewayConnectEvent(reached_ready
                                                  ? GatewayEvent::Kind::Disconnected
                                                  : GatewayEvent::Kind::ConnectFailed,
                                              stage, code, reason));
        return false;
    };
    const auto fail_error = [&](const GatewayConnectError& error) {
        return fail(error.stage, error.error_code, error.detail);
    };

    // 取令牌/查地址在 provider 里(适配器已发 fetching_token/
    // fetching_gateway_url 阶段事件);失败账由 provider 带上来。
    const auto url = options_.gateway_url_provider();
    if (!url.has_value()) {
        return fail_error(url.error());
    }
    options_.on_event(GatewayConnectEvent(GatewayEvent::Kind::StageChanged,
                                          kStageConnecting, std::string(),
                                          std::string()));
    const auto connected = transport->Connect(*url);
    if (!connected.has_value()) {
        return fail_error(connected.error());
    }

    // 1) Hello(op=10,带心跳间隔)——WS 已升级,首条即 Hello。
    std::int64_t heartbeat_interval_ms = 0;
    {
        const auto message = transport->ReadMessage(options_.hello_timeout_ms);
        if (!message.has_value()) {
            return fail(kStageConnecting,
                        message.error().kind == WsError::Kind::Timeout ? "hello_timeout"
                                                                        : "hello_failed",
                        "waiting hello: " + message.error().detail);
        }
        const auto payload_json =
            nlohmann::json::parse(*message, nullptr, /*allow_exceptions=*/false);
        if (payload_json.is_discarded()) {
            return fail(kStageConnecting, "hello_bad_payload", "hello payload not json");
        }
        std::string parse_error;
        const auto payload = ParseGatewayPayload(payload_json, &parse_error);
        if (!payload.has_value() || payload->op != GatewayOp::Hello) {
            return fail(kStageConnecting, "hello_bad_payload", "first message is not HELLO");
        }
        const auto interval = ParseHelloInterval(payload->d);
        if (!interval.has_value()) {
            return fail(kStageConnecting, "hello_bad_payload",
                        "HELLO missing heartbeat_interval_ms");
        }
        heartbeat_interval_ms = *interval;
    }

    // 2) Identify / Resume。
    state_.store(State::Authenticating);
    options_.on_event(GatewayConnectEvent(GatewayEvent::Kind::StageChanged,
                                          kStageIdentifying, std::string(),
                                          std::string()));
    const auto token = options_.token_provider();
    if (!token.has_value()) {
        return fail_error(token.error());
    }
    if (session_id_.empty()) {
        const auto sent = transport->SendText(BuildIdentify(*token, options_.intents).dump());
        if (!sent.has_value()) {
            return fail(kStageIdentifying, "identify_send_failed",
                        "send identify: " + sent.error());
        }
    } else {
        const auto sent = transport->SendText(
            BuildResume(*token, session_id_, last_seq_.load()).dump());
        if (!sent.has_value()) {
            return fail(kStageIdentifying, "identify_send_failed",
                        "send resume: " + sent.error());
        }
    }

    // 3) 鉴权结果:READY(Identify)/RESUMED(Resume)/Invalid Session/Reconnect。
    bool resumed = false;
    {
        const auto message = transport->ReadMessage(options_.ready_timeout_ms);
        if (!message.has_value()) {
            return fail(kStageIdentifying,
                        message.error().kind == WsError::Kind::Timeout ? "ready_timeout"
                                                                        : "ready_failed",
                        "waiting ready: " + message.error().detail);
        }
        const auto payload_json =
            nlohmann::json::parse(*message, nullptr, /*allow_exceptions=*/false);
        if (payload_json.is_discarded()) {
            return fail(kStageIdentifying, "ready_bad_payload", "ready payload not json");
        }
        std::string parse_error;
        const auto payload = ParseGatewayPayload(payload_json, &parse_error);
        if (!payload.has_value()) {
            return fail(kStageIdentifying, "ready_bad_payload", "ready payload: " + parse_error);
        }
        if (payload->op == GatewayOp::InvalidSession) {
            const auto resumable = ParseInvalidSessionResumable(payload_json);
            if (resumable.has_value() && !*resumable) {
                *session_was_invalidated = true;
            }
            return fail(kStageIdentifying, "invalid_session", "invalid session");
        }
        if (payload->op == GatewayOp::Reconnect) {
            return fail(kStageIdentifying, "server_reconnect_requested",
                        "server requested reconnect");
        }
        if (payload->op != GatewayOp::Dispatch) {
            return fail(kStageIdentifying, "ready_bad_payload",
                        "unexpected op " + std::to_string(payload->op_raw) +
                            " while authenticating");
        }
        if (payload->t == "READY") {
            const auto ready = ParseReady(payload->d, &parse_error);
            if (!ready.has_value()) {
                return fail(kStageIdentifying, "ready_bad_payload", "READY: " + parse_error);
            }
            session_id_ = ready->session_id;
            {
                GatewayEvent event;
                event.kind = GatewayEvent::Kind::SessionReady;
                event.detail = ready->user_id;
                event.seq = payload->s;
                options_.on_event(event);
            }
        } else if (payload->t == "RESUMED") {
            resumed = true;
            {
                GatewayEvent event;
                event.kind = GatewayEvent::Kind::SessionResumed;
                event.detail = session_id_;
                event.seq = payload->s;
                options_.on_event(event);
            }
        } else {
            // 鉴权窗内来了业务事件(网关通常先回 READY 才推,但不赌):
            // 按序记账并照常派发,不静默吞。
            if (payload->t == "C2C_MESSAGE_CREATE") {
                GatewayEvent event;
                event.kind = GatewayEvent::Kind::C2cMessageCreate;
                event.c2c_d = payload->d;
                event.seq = payload->s;
                options_.on_event(event);
            } else if (payload->t == "INTERACTION_CREATE") {
                GatewayEvent event;
                event.kind = GatewayEvent::Kind::InteractionCreate;
                event.interaction_d = payload->d;
                event.seq = payload->s;
                options_.on_event(event);
            }
        }
        if (payload->s >= 0) {
            last_seq_.store(payload->s);
        }
    }

    // READY/RESUMED 过:connected 成立(§三)。阶段推进放这里,SessionReady/
    // SessionResumed 事件在前——适配器先把 connected 记上,阶段再跟着变。
    reached_ready = true;
    options_.on_event(GatewayConnectEvent(GatewayEvent::Kind::StageChanged,
                                          kStageConnected, std::string(),
                                          std::string()));

    // 4) 运行循环:读分发 + 心跳 + ACK 监视。心跳节拍锚定绝对时刻
    //    (next_beat),事件密集也不重置心跳窗——重置会饿死心跳,服务端掐线。
    state_.store(State::Running);
    int missed_acks = 0;
    bool ever_acked = resumed;  // Resume 成功本身证明服务端认了会话
    std::int64_t next_beat = options_.now_ms() + heartbeat_interval_ms;
    while (!stop->load()) {
        const std::int64_t now = options_.now_ms();
        std::int64_t remaining = next_beat - now;
        if (remaining <= 0) {
            // 心跳到期:发 op1,携带最新 s(官方 opcode 表)。
            const auto sent =
                transport->SendText(BuildHeartbeat(last_seq_.load() >= 0
                                                       ? std::optional<std::int64_t>(
                                                             last_seq_.load())
                                                       : std::nullopt)
                                        .dump());
            if (!sent.has_value()) {
                return fail(kStageConnected, "heartbeat_send_failed",
                            "send heartbeat: " + sent.error());
            }
            ++missed_acks;
            if (missed_acks > options_.missed_ack_limit) {
                return fail(kStageConnected, "heartbeat_ack_missed",
                            "heartbeat ack missed " + std::to_string(missed_acks) +
                                " times");
            }
            next_beat = options_.now_ms() + heartbeat_interval_ms;
            remaining = heartbeat_interval_ms;
        }
        const auto message =
            transport->ReadMessage(static_cast<int>(std::max<std::int64_t>(remaining, 1)));
        if (!message.has_value()) {
            if (message.error().kind == WsError::Kind::Timeout) {
                continue;  // 到点,回循环顶发心跳
            }
            return fail(kStageConnected, ReadErrorCode(message.error()),
                        "read: " + message.error().detail);
        }
        const auto payload_json =
            nlohmann::json::parse(*message, nullptr, /*allow_exceptions=*/false);
        if (payload_json.is_discarded()) {
            return fail(kStageConnected, "dispatch_bad_payload", "dispatch payload not json");
        }
        std::string parse_error;
        const auto payload = ParseGatewayPayload(payload_json, &parse_error);
        if (!payload.has_value()) {
            return fail(kStageConnected, "dispatch_bad_payload",
                        "dispatch payload: " + parse_error);
        }
        switch (payload->op.value_or(GatewayOp::Heartbeat)) {
            case GatewayOp::Dispatch:
                if (payload->s >= 0) {
                    last_seq_.store(payload->s);
                }
                if (payload->t == "C2C_MESSAGE_CREATE") {
                    GatewayEvent event;
                    event.kind = GatewayEvent::Kind::C2cMessageCreate;
                    event.c2c_d = payload->d;
                    event.seq = payload->s;
                    options_.on_event(event);
                } else if (payload->t == "INTERACTION_CREATE") {
                    // Q6 按钮回调:按序记账并派发;宿主裁决后回 PUT
                    // /interactions/{id}。事件形状校验在适配器(纯函数
                    // MapInteractionCreate),这里只转手。
                    GatewayEvent event;
                    event.kind = GatewayEvent::Kind::InteractionCreate;
                    event.interaction_d = payload->d;
                    event.seq = payload->s;
                    options_.on_event(event);
                } else if (payload->t == "READY" || payload->t == "RESUMED") {
                    // 鉴权窗已处理过;重复出现按序记账即可。
                } else {
                    // intents 只订 C2C/互动:兄弟事件(FRIEND_ADD/
                    // C2C_MSG_RECEIVE 等)按序记账,不进模型。
                }
                break;
            case GatewayOp::HeartbeatAck:
                missed_acks = 0;
                ever_acked = true;
                break;
            case GatewayOp::Reconnect:
                return fail(kStageConnected, "server_reconnect_requested",
                            "server requested reconnect");
            case GatewayOp::InvalidSession: {
                const auto resumable = ParseInvalidSessionResumable(payload_json);
                if (resumable.has_value() && !*resumable) {
                    *session_was_invalidated = true;
                }
                return fail(kStageConnected, "invalid_session", "invalid session");
            }
            case GatewayOp::Hello:
            case GatewayOp::Heartbeat:
            case GatewayOp::Identify:
            case GatewayOp::Resume:
                break;  // 服务器不该发;按序忽略,不断连(宽容读)
        }
    }
    // stop 置位:干净收场(也算"稳定结束",不涨退避)。
    clear_in_flight();
    transport->Close(1000, "stop");
    return ever_acked;
}

}  // namespace lubancode::channel::qq
