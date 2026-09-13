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
    explicit WsGatewayTransport(std::string ca_pem) : ca_pem_(std::move(ca_pem)) {}

    std::expected<void, std::string> Connect(const std::string& url) override {
        WsConnectOptions options;
        options.url = url;
        options.ca_pem = ca_pem_;
        auto client = WsClient::Connect(options);
        if (!client.has_value()) {
            return std::unexpected("ws connect: " + client.error().detail);
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
    std::string ca_pem_;
    WsClient client_;
};

}  // namespace

std::function<std::unique_ptr<IGatewayTransport>()> MakeWsTransportFactory(std::string ca_pem) {
    return [ca_pem = std::move(ca_pem)]() -> std::unique_ptr<IGatewayTransport> {
        return std::make_unique<WsGatewayTransport>(ca_pem);
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
            options_.on_event(GatewayEvent{GatewayEvent::Kind::SessionInvalidated,
                                           nlohmann::json::object(), "invalid session",
                                           last_seq_.load()});
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
        options_.on_event(GatewayEvent{GatewayEvent::Kind::Disconnected,
                                       nlohmann::json::object(),
                                       "disconnected; backoff " + std::to_string(backoff_ms) +
                                           "ms (attempt " + std::to_string(attempt) + ")",
                                       last_seq_.load()});
        SleepInterruptible(stop, backoff_ms);
    }
    state_.store(State::Stopped);
}

bool QqGatewaySession::RunOneConnection(std::atomic<bool>* stop,
                                        bool* session_was_invalidated) {
    auto transport = options_.transport_factory();
    if (!transport) {
        options_.on_event(GatewayEvent{GatewayEvent::Kind::Disconnected,
                                       nlohmann::json::object(),
                                       "no transport factory", last_seq_.load()});
        return false;
    }
    in_flight_.store(transport.get());
    const auto clear_in_flight = [this]() { in_flight_.store(nullptr); };

    const auto finish = [&](const std::string& reason) {
        clear_in_flight();
        transport->Close(1000, "session end");
        options_.on_event(
            GatewayEvent{GatewayEvent::Kind::Disconnected, nlohmann::json::object(),
                         reason, last_seq_.load()});
        return false;
    };

    const auto url = options_.gateway_url_provider();
    if (!url.has_value()) {
        return finish("gateway url: " + url.error());
    }
    const auto connected = transport->Connect(*url);
    if (!connected.has_value()) {
        return finish(connected.error());
    }

    // 1) Hello(op=10,带心跳间隔)。
    std::int64_t heartbeat_interval_ms = 0;
    {
        const auto message = transport->ReadMessage(options_.hello_timeout_ms);
        if (!message.has_value()) {
            return finish("waiting hello: " + message.error().detail);
        }
        const auto payload_json =
            nlohmann::json::parse(*message, nullptr, /*allow_exceptions=*/false);
        if (payload_json.is_discarded()) {
            return finish("hello payload not json");
        }
        std::string parse_error;
        const auto payload = ParseGatewayPayload(payload_json, &parse_error);
        if (!payload.has_value() || payload->op != GatewayOp::Hello) {
            return finish("first message is not HELLO");
        }
        const auto interval = ParseHelloInterval(payload->d);
        if (!interval.has_value()) {
            return finish("HELLO missing heartbeat_interval_ms");
        }
        heartbeat_interval_ms = *interval;
    }

    // 2) Identify / Resume。
    state_.store(State::Authenticating);
    const auto token = options_.token_provider();
    if (!token.has_value()) {
        return finish("token: " + token.error());
    }
    if (session_id_.empty()) {
        const auto sent = transport->SendText(BuildIdentify(*token, options_.intents).dump());
        if (!sent.has_value()) {
            return finish("send identify: " + sent.error());
        }
    } else {
        const auto sent = transport->SendText(
            BuildResume(*token, session_id_, last_seq_.load()).dump());
        if (!sent.has_value()) {
            return finish("send resume: " + sent.error());
        }
    }

    // 3) 鉴权结果:READY(Identify)/RESUMED(Resume)/Invalid Session/Reconnect。
    bool resumed = false;
    {
        const auto message = transport->ReadMessage(options_.ready_timeout_ms);
        if (!message.has_value()) {
            return finish("waiting ready: " + message.error().detail);
        }
        const auto payload_json =
            nlohmann::json::parse(*message, nullptr, /*allow_exceptions=*/false);
        if (payload_json.is_discarded()) {
            return finish("ready payload not json");
        }
        std::string parse_error;
        const auto payload = ParseGatewayPayload(payload_json, &parse_error);
        if (!payload.has_value()) {
            return finish("ready payload: " + parse_error);
        }
        if (payload->op == GatewayOp::InvalidSession) {
            const auto resumable = ParseInvalidSessionResumable(payload_json);
            if (resumable.has_value() && !*resumable) {
                *session_was_invalidated = true;
            }
            return finish("invalid session");
        }
        if (payload->op == GatewayOp::Reconnect) {
            return finish("server requested reconnect");
        }
        if (payload->op != GatewayOp::Dispatch) {
            return finish("unexpected op " + std::to_string(payload->op_raw) +
                          " while authenticating");
        }
        if (payload->t == "READY") {
            const auto ready = ParseReady(payload->d, &parse_error);
            if (!ready.has_value()) {
                return finish("READY: " + parse_error);
            }
            session_id_ = ready->session_id;
            options_.on_event(GatewayEvent{GatewayEvent::Kind::SessionReady,
                                           nlohmann::json::object(), ready->user_id,
                                           payload->s});
        } else if (payload->t == "RESUMED") {
            resumed = true;
            options_.on_event(GatewayEvent{GatewayEvent::Kind::SessionResumed,
                                           nlohmann::json::object(), session_id_,
                                           payload->s});
        } else {
            // 鉴权窗内来了别的事件:按序记账后继续(不判错)。
        }
        if (payload->s >= 0) {
            last_seq_.store(payload->s);
        }
    }

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
                return finish("send heartbeat: " + sent.error());
            }
            ++missed_acks;
            if (missed_acks > options_.missed_ack_limit) {
                return finish("heartbeat ack missed " + std::to_string(missed_acks) +
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
            return finish("read: " + message.error().detail);
        }
        const auto payload_json =
            nlohmann::json::parse(*message, nullptr, /*allow_exceptions=*/false);
        if (payload_json.is_discarded()) {
            return finish("dispatch payload not json");
        }
        std::string parse_error;
        const auto payload = ParseGatewayPayload(payload_json, &parse_error);
        if (!payload.has_value()) {
            return finish("dispatch payload: " + parse_error);
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
                } else if (payload->t == "READY" || payload->t == "RESUMED") {
                    // 鉴权窗已处理过;重复出现按序记账即可。
                } else {
                    // intents 只订 GROUP_AND_C2C_EVENT:兄弟事件
                    // (FRIEND_ADD/C2C_MSG_RECEIVE 等)按序记账,不进模型。
                }
                break;
            case GatewayOp::HeartbeatAck:
                missed_acks = 0;
                ever_acked = true;
                break;
            case GatewayOp::Reconnect:
                return finish("server requested reconnect");
            case GatewayOp::InvalidSession: {
                const auto resumable = ParseInvalidSessionResumable(payload_json);
                if (resumable.has_value() && !*resumable) {
                    *session_was_invalidated = true;
                }
                return finish("invalid session");
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
