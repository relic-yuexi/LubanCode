#include "channel/feishu/feishu_gateway.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>
#include <thread>
#include <utility>

namespace lubancode::channel::feishu {

namespace {

// configuration.md §10 的退避阶梯(秒)——与 QqGatewaySession 同一条。
constexpr int kBackoffSeconds[] = {1, 2, 4, 8, 16, 30, 60};
constexpr int kBackoffSteps = static_cast<int>(sizeof(kBackoffSeconds) / sizeof(int));

// 读超时余量(§5.4:2×pingInterval + 5s)。
constexpr std::int64_t kReadTimeoutMarginMs = 5'000;

std::string ToLowerCopy(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// 从头文本里取首个指定头(大小写不敏感),找不到空串。
std::string HeaderValueOf(const std::string& headers, const std::string& name) {
    const std::string prefix = ToLowerCopy(name) + ":";
    std::size_t pos = 0;
    while (pos < headers.size()) {
        const std::size_t eol = headers.find("\r\n", pos);
        const std::size_t line_end = eol == std::string::npos ? headers.size() : eol;
        const std::string line =
            ToLowerCopy(headers.substr(pos, line_end - pos));
        if (line.rfind(prefix, 0) == 0) {
            std::size_t start = pos + prefix.size();
            while (start < headers.size() &&
                   (headers[start] == ' ' || headers[start] == '\t')) {
                ++start;
            }
            std::size_t end = line_end;
            while (end > start &&
                   (headers[end - 1] == ' ' || headers[end - 1] == '\t')) {
                --end;
            }
            return headers.substr(start, end - start);
        }
        if (eol == std::string::npos) {
            break;
        }
        pos = eol + 2;
    }
    return std::string();
}

// 头文本首行 "HTTP/1.1 NNN" 的状态码;解析不出 0。
int StatusCodeOf(const std::string& headers) {
    const std::size_t sp1 = headers.find(' ');
    if (sp1 == std::string::npos) {
        return 0;
    }
    const std::size_t sp2 = headers.find(' ', sp1 + 1);
    const std::string code_text =
        headers.substr(sp1 + 1, (sp2 == std::string::npos ? headers.find("\r\n")
                                                          : sp2) -
                                    sp1 - 1);
    if (code_text.find_first_not_of("0123456789") != std::string::npos) {
        return 0;
    }
    return static_cast<int>(std::strtoll(code_text.c_str(), nullptr, 10));
}

std::string Capped(const std::string& text, std::size_t cap) {
    if (text.size() <= cap) {
        return text;
    }
    return text.substr(0, cap) + "...";
}

// URL query 里取整型参数(引导 URL 已拼好 device_id/service_id,§5.1);
// 解不出 0。
std::int64_t QueryIntOf(const std::string& url, const std::string& key) {
    const std::size_t query_start = url.find('?');
    if (query_start == std::string::npos) {
        return 0;
    }
    const std::string key_prefix = key + "=";
    std::size_t pos = query_start + 1;
    while (pos < url.size()) {
        const std::size_t amp = url.find('&', pos);
        const std::string piece =
            url.substr(pos, (amp == std::string::npos ? url.size() : amp) - pos);
        if (piece.rfind(key_prefix, 0) == 0) {
            const std::string value = piece.substr(key_prefix.size());
            if (!value.empty() &&
                value.find_first_not_of("0123456789") == std::string::npos) {
                return std::strtoll(value.c_str(), nullptr, 10);
            }
            return 0;
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return 0;
}

// 运行期读错误的稳定码(照 QQ ReadErrorCode 的口径)。
std::string ReadErrorCode(const transport::WsError& error) {
    switch (error.kind) {
        case transport::WsError::Kind::Timeout:
            return "read_timeout";
        case transport::WsError::Kind::Closed:
            return "read_closed";
        case transport::WsError::Kind::Protocol:
            return "read_protocol";
        case transport::WsError::Kind::Failed:
            break;
    }
    return "read_failed";
}

// ACK 帧构造(§5.6):复用原帧——method/service/SeqID/LogID/headers 原样,
// headers 追加 biz_rt(处理毫秒差),payload 换成 {"code":N,"headers":{},
// "data":null}。
std::string EncodeAckFrame(const FeishuFrame& original, int code,
                           std::int64_t elapsed_ms) {
    FeishuFrame ack = original;
    ack.headers.push_back(FeishuFrameHeader{kHeaderBizRt, std::to_string(elapsed_ms)});
    nlohmann::json payload = nlohmann::json::object();
    payload["code"] = code;
    payload["headers"] = nlohmann::json::object();
    payload["data"] = nullptr;
    ack.payload = payload.dump();
    return EncodeFeishuFrame(ack);
}

// 真 transport::WsClient 的 IFeishuGatewayTransport 适配(照 QQ 的
// WsGatewayTransport;发送走 Binary)。
class FeishuWsTransport final : public IFeishuGatewayTransport {
public:
    FeishuWsTransport(std::string ca_pem, transport::TlsTrustMode trust_mode)
        : ca_pem_(std::move(ca_pem)), trust_mode_(trust_mode) {}

    std::expected<void, FeishuConnectError> Connect(const std::string& url) override {
        transport::WsConnectOptions options;
        options.url = url;
        options.ca_pem = ca_pem_;
        options.trust_mode = trust_mode_;
        // 建立期取消(A08 口径同 QQ)。
        auto cancel_state = std::make_shared<transport::WsConnectCancelState>();
        options.cancel = cancel_state;
        auto client = transport::WsClient::Connect(options);
        if (!client.has_value()) {
            return std::unexpected(ClassifyFeishuHandshakeFailure(client.error()));
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            client_ = std::move(*client);
            connect_cancel_ = std::move(cancel_state);
            connect_cancel_->DeregisterFd();
        }
        return {};
    }

    std::expected<void, std::string> SendBinary(const std::string& bytes) override {
        const auto sent = client_.SendBinary(bytes);
        if (!sent.has_value()) {
            return std::unexpected("ws send: " + sent.error().detail);
        }
        return {};
    }

    std::expected<std::string, transport::WsError> ReadMessage(int timeout_ms) override {
        return client_.ReadMessage(timeout_ms);
    }

    void Cancel() override {
        std::shared_ptr<transport::WsConnectCancelState> cancel_state;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            cancel_state = connect_cancel_;
            connect_cancel_.reset();
            client_.Cancel();
        }
        if (cancel_state != nullptr) {
            cancel_state->Cancel();  // 锁外:只碰自家的原子与登记句柄
        }
    }

    void Close(std::uint16_t code, const std::string& reason) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        (void)client_.Close(code, reason);
    }

private:
    std::string ca_pem_;
    transport::TlsTrustMode trust_mode_ = transport::TlsTrustMode::ExplicitCa;
    // client_ 归网关线程独占;Cancel 只经 Cancel() 摸连接。发布/清理过
    // mutex_(与 QQ 的 WsGatewayTransport 同一套账)。
    std::mutex mutex_;
    std::shared_ptr<transport::WsConnectCancelState> connect_cancel_;
    transport::WsClient client_;
};

}  // namespace

FeishuConnectError ClassifyFeishuHandshakeFailure(const transport::WsError& error) {
    // 无握手响应头(网络/TLS/超时/取消):按 kind 分,可重试。
    if (error.handshake_headers.empty()) {
        std::string code = "connect_failed";
        switch (error.kind) {
            case transport::WsError::Kind::Timeout:
                code = "connect_timeout";
                break;
            case transport::WsError::Kind::Protocol:
                code = error.error_code.empty() ? std::string("ws_handshake_failed")
                                                : error.error_code;
                break;
            case transport::WsError::Kind::Closed:
                code = error.detail == "connect cancelled"
                           ? std::string("connect_cancelled")
                           : std::string("ws_handshake_closed");
                break;
            case transport::WsError::Kind::Failed:
                code = error.detail.rfind("tls: ", 0) == 0
                           ? std::string(transport::kTlsCodeHandshakeFailed)
                           : std::string("connect_failed");
                break;
        }
        return FeishuConnectError{kStageConnecting, code,
                                  "ws connect: " + error.detail};
    }
    // 升级被拒:§5.2 的裁决表。Handshake-Autherrcode 1000040350 =
    // ExceedConnLimit(超连);Handshake-Status 514=AuthFailed、403=
    // Forbidden、缺省回退 HTTP 码。
    const std::string& headers = error.handshake_headers;
    const std::string autherrcode = HeaderValueOf(headers, "Handshake-Autherrcode");
    if (autherrcode == "1000040350") {
        return FeishuConnectError{kStageConnecting, "feishu_exceed_conn_limit",
                                  "handshake exceed conn limit(1000040350)",
                                  /*non_retryable=*/true};
    }
    int status = 0;
    const std::string handshake_status = HeaderValueOf(headers, "Handshake-Status");
    if (!handshake_status.empty() &&
        handshake_status.find_first_not_of("0123456789") == std::string::npos) {
        status = static_cast<int>(
            std::strtoll(handshake_status.c_str(), nullptr, 10));
    }
    if (status == 0) {
        status = StatusCodeOf(headers);  // 缺省回退 HTTP 码(§5.2)
    }
    const std::string msg = Capped(HeaderValueOf(headers, "Handshake-Msg"), 128);
    const std::string status_note =
        " HTTP=" + std::to_string(StatusCodeOf(headers)) +
        (msg.empty() ? std::string() : " msg=" + msg);
    if (status == 514) {
        return FeishuConnectError{kStageConnecting, "feishu_handshake_auth_failed",
                                  "handshake auth failed(514)" + status_note,
                                  /*non_retryable=*/true};
    }
    if (status == 403) {
        return FeishuConnectError{kStageConnecting, "feishu_handshake_forbidden",
                                  "handshake forbidden(403)" + status_note,
                                  /*non_retryable=*/true};
    }
    if (status >= 500) {
        return FeishuConnectError{kStageConnecting, "feishu_handshake_server_error",
                                  "handshake server error" + status_note};
    }
    return FeishuConnectError{kStageConnecting, "feishu_handshake_rejected",
                              "upgrade not accepted" + status_note};
}

std::function<std::unique_ptr<IFeishuGatewayTransport>()> MakeFeishuWsTransportFactory(
    std::string ca_pem, transport::TlsTrustMode trust_mode) {
    return [ca_pem = std::move(ca_pem), trust_mode]()
               -> std::unique_ptr<IFeishuGatewayTransport> {
        return std::make_unique<FeishuWsTransport>(ca_pem, trust_mode);
    };
}

FeishuGatewaySession::~FeishuGatewaySession() {
    CancelInFlight();
}

std::string FeishuGatewaySession::state_name() const {
    switch (state_.load()) {
        case State::Idle:
            return "idle";
        case State::Connecting:
            return "connecting";
        case State::Running:
            return "running";
        case State::Backoff:
            return "backoff";
        case State::Stopped:
            return "stopped";
    }
    return "unknown";
}

void FeishuGatewaySession::CancelInFlight() {
    // A08 共享所有权:锁内拷 shared_ptr,锁外调 Cancel。
    std::shared_ptr<IFeishuGatewayTransport> transport;
    {
        const std::lock_guard<std::mutex> lock(in_flight_mutex_);
        transport = in_flight_;
    }
    if (transport != nullptr) {
        transport->Cancel();
    }
}

FeishuGatewayAck FeishuGatewaySession::EmitEvent(const FeishuGatewayEvent& event) {
    if (!options_.on_event) {
        return FeishuGatewayAck::Ok;  // 观测型装配:无宿主即无落盘语义
    }
    return options_.on_event(event);
}

void FeishuGatewaySession::SleepInterruptible(std::atomic<bool>* stop,
                                               std::int64_t ms) {
    const std::int64_t step = 100;
    for (std::int64_t slept = 0; slept < ms && !stop->load(); slept += step) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<long long>(std::min<std::int64_t>(step, ms - slept))));
    }
}

std::int64_t FeishuGatewaySession::PingIntervalMs() const {
    const std::int64_t secs = ping_interval_ms_ / 1000;
    const std::int64_t clamped =
        std::clamp<std::int64_t>(secs, options_.min_ping_interval_secs,
                                 options_.max_ping_interval_secs);
    return clamped * 1000;
}

std::int64_t FeishuGatewaySession::ReadTimeoutMs() const {
    // §5.4:读超时 = 2×pingInterval + 5s,每收一帧重置——半开连接靠它拆。
    return 2 * PingIntervalMs() + kReadTimeoutMarginMs;
}

std::string FeishuGatewaySession::SendPing(std::int32_t service) {
    FeishuFrame ping;
    const std::uint64_t seq = next_seq_id_++;
    ping.seq_id = seq;
    ping.log_id = seq;
    ping.service = service;
    ping.method = kFrameMethodControl;
    ping.headers.push_back(FeishuFrameHeader{kHeaderType, kHeaderValuePing});
    return EncodeFeishuFrame(ping);
}

void FeishuGatewaySession::PurgeExpiredFragments(std::int64_t now_ms) {
    for (auto it = fragments_.begin(); it != fragments_.end();) {
        if (now_ms - it->second.first_seen_ms >= options_.fragment_window_ms) {
            // §5.5:缺片到期静默弃(平台等不到 ACK 会重推,不丢信)。
            it = fragments_.erase(it);
            dropped_fragments_.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++it;
        }
    }
}

std::optional<std::int64_t> FeishuGatewaySession::NextFragmentExpiryMs(
    std::int64_t now_ms) const {
    std::optional<std::int64_t> next;
    for (const auto& [message_id, group] : fragments_) {
        (void)message_id;
        const std::int64_t expiry = group.first_seen_ms + options_.fragment_window_ms;
        if (expiry > now_ms && (!next.has_value() || expiry < *next)) {
            next = expiry;
        }
    }
    return next;
}

std::optional<std::string> FeishuGatewaySession::HandleDataFrame(const FeishuFrame& frame) {
    const std::string type = FeishuFrameHeaderValue(frame, kHeaderType);
    if (type != kHeaderValueEvent) {
        // card 与陌生 type:静默丢弃记账(§5.5"首版只认 event"),回 ACK
        // 200 防重推——已明确终结,不冒充需要重推的失败。
        unsupported_events_.fetch_add(1, std::memory_order_relaxed);
        FeishuGatewayEvent event;
        event.kind = FeishuGatewayEvent::Kind::UnsupportedEvent;
        event.detail = type.empty() ? std::string("data frame without type header")
                                    : "frame type " + type;
        (void)EmitEvent(event);
        const auto sent = transport_->SendBinary(EncodeAckFrame(frame, 200, 0));
        if (!sent.has_value()) {
            return "ack send: " + sent.error();
        }
        return std::nullopt;
    }
    // sum/seq 头:字符串数值(§5.5);缺 sum 按单片。
    const std::string sum_text = FeishuFrameHeaderValue(frame, kHeaderSum);
    const std::string seq_text = FeishuFrameHeaderValue(frame, kHeaderSeq);
    const std::string message_id = FeishuFrameHeaderValue(frame, kHeaderMessageId);
    std::int64_t sum = 1;
    std::int64_t seq = 1;
    if (!sum_text.empty() && sum_text.find_first_not_of("0123456789") == std::string::npos) {
        sum = std::strtoll(sum_text.c_str(), nullptr, 10);
    }
    if (!seq_text.empty() && seq_text.find_first_not_of("0123456789") == std::string::npos) {
        seq = std::strtoll(seq_text.c_str(), nullptr, 10);
    }
    const std::int64_t received_at = options_.now_ms();
    const std::string payload =
        frame.payload.has_value() ? *frame.payload : std::string();
    if (sum <= 1) {
        return DeliverEvent(payload, frame, received_at);
    }
    if (message_id.empty()) {
        // sum>1 却没有 message_id:拼装键缺失,明确终结留痕(不赌)。
        unsupported_events_.fetch_add(1, std::memory_order_relaxed);
        FeishuGatewayEvent event;
        event.kind = FeishuGatewayEvent::Kind::UnsupportedEvent;
        event.detail = "fragmented frame without message_id";
        (void)EmitEvent(event);
        const auto sent = transport_->SendBinary(EncodeAckFrame(frame, 200, 0));
        if (!sent.has_value()) {
            return "ack send: " + sent.error();
        }
        return std::nullopt;
    }
    FragmentGroup& group = fragments_[message_id];
    if (group.parts.empty()) {
        group.sum = sum;
        group.first_seen_ms = received_at;
    }
    group.parts[seq] = payload;
    if (static_cast<std::int64_t>(group.parts.size()) < group.sum) {
        return std::nullopt;  // 等齐片;窗内没齐由 PurgeExpiredFragments 弃
    }
    // 拼齐:按 seq 序拼接(§5.5"按 message_id 缓存分片 5 秒拼装")。先取
    // first_seen 再 erase——group 是 map 节点引用,erase 后即悬空。
    std::string assembled;
    for (const auto& [part_seq, part] : group.parts) {
        (void)part_seq;
        assembled += part;
    }
    const std::int64_t first_seen_ms = group.first_seen_ms;
    fragments_.erase(message_id);
    return DeliverEvent(assembled, frame, first_seen_ms);
}

std::optional<std::string> FeishuGatewaySession::DeliverEvent(
    const std::string& payload, const FeishuFrame& original_frame,
    std::int64_t received_at_ms) {
    const auto parsed = nlohmann::json::parse(payload, nullptr,
                                              /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        // 载荷解不开:明确终结(重推同形状也无解),留痕 + ACK 200。
        unsupported_events_.fetch_add(1, std::memory_order_relaxed);
        FeishuGatewayEvent event;
        event.kind = FeishuGatewayEvent::Kind::UnsupportedEvent;
        event.detail = "event payload not a json object";
        (void)EmitEvent(event);
        const auto sent = transport_->SendBinary(
            EncodeAckFrame(original_frame, 200, options_.now_ms() - received_at_ms));
        if (!sent.has_value()) {
            return "ack send: " + sent.error();
        }
        return std::nullopt;
    }
    FeishuGatewayEvent event;
    event.kind = FeishuGatewayEvent::Kind::MessageReceive;
    event.event_payload = parsed;
    const FeishuGatewayAck ack = EmitEvent(event);
    // §5.6:handler 接住(spool 落盘)回 200;没接住回 500 让平台重推
    //(宿主 ingress 按 provider_event_id 去重兜底)。biz_rt = 处理毫秒差。
    const int code = ack == FeishuGatewayAck::Ok ? 200 : 500;
    const auto sent = transport_->SendBinary(
        EncodeAckFrame(original_frame, code, options_.now_ms() - received_at_ms));
    if (!sent.has_value()) {
        return "ack send: " + sent.error();
    }
    return std::nullopt;
}

void FeishuGatewaySession::RunLoop(std::atomic<bool>* stop) {
    int attempt = 0;
    while (!stop->load()) {
        state_.store(State::Connecting);
        const RunOutcome outcome = RunOneConnection(stop, /*attempt_number=*/attempt + 1);
        if (outcome.fatal) {
            // 凭据类客户端错(§5.9):不重试,账面已报(ConnectFailed 带
            // non_retryable),收口退出。
            break;
        }
        if (stop->load()) {
            break;
        }
        if (outcome.stable) {
            attempt = 0;
        } else {
            attempt = std::min(attempt + 1, kBackoffSteps);
        }
        connect_attempts_.store(attempt);
        const int base_ms = kBackoffSeconds[attempt == 0 ? 0 : attempt - 1] * 1000;
        std::mt19937 rng(static_cast<unsigned>(options_.now_ms() & 0xFFFFFFFF));
        std::uniform_int_distribution<int> jitter(0, base_ms / 10);  // 10% jitter
        const int backoff_ms =
            static_cast<int>((base_ms + jitter(rng)) * options_.backoff_scale);
        state_.store(State::Backoff);
        // 退避排程单独成事件:只带 attempt 与下次尝试时刻,不带"原因"
        //(根因归 ConnectFailed/Disconnected,退避不许覆盖它——§三纪律)。
        FeishuGatewayEvent backoff;
        backoff.kind = FeishuGatewayEvent::Kind::BackoffScheduled;
        backoff.attempt = attempt;
        backoff.next_retry_at_ms = options_.now_ms() + backoff_ms;
        (void)EmitEvent(backoff);
        SleepInterruptible(stop, backoff_ms);
    }
    state_.store(State::Stopped);
    (void)EmitEvent(FeishuGatewayEvent{FeishuGatewayEvent::Kind::Stopped});
}

FeishuGatewaySession::RunOutcome FeishuGatewaySession::RunOneConnection(
    std::atomic<bool>* stop, int attempt_number) {
    fragments_.clear();  // 分片账不跨连接(重连后服务端整条重推)
    std::shared_ptr<IFeishuGatewayTransport> transport = options_.transport_factory();
    if (!transport) {
        FeishuGatewayEvent event;
        event.kind = FeishuGatewayEvent::Kind::ConnectFailed;
        event.stage = kStageConnecting;
        event.error_code = "no_transport";
        event.detail = "no transport factory";
        event.attempt = attempt_number;
        (void)EmitEvent(event);
        return RunOutcome{};
    }
    {
        const std::lock_guard<std::mutex> lock(in_flight_mutex_);
        in_flight_ = transport;
    }
    transport_ = transport.get();
    const auto clear_in_flight = [this]() {
        const std::lock_guard<std::mutex> lock(in_flight_mutex_);
        in_flight_.reset();
    };
    struct InFlightGuard {
        FeishuGatewaySession* session;
        ~InFlightGuard() {
            const std::lock_guard<std::mutex> lock(session->in_flight_mutex_);
            session->in_flight_.reset();
        }
    } in_flight_guard{this};
    bool reached_connected = false;  // 区分 ConnectFailed 与 Disconnected

    // 一轮失败的收口:ConnectFailed(没到在线)或 Disconnected(在线过才断)。
    const auto fail = [&](const std::string& stage, const std::string& code,
                          const std::string& reason, bool non_retryable = false) {
        clear_in_flight();
        transport->Close(1000, "session end");
        FeishuGatewayEvent event;
        event.kind = reached_connected ? FeishuGatewayEvent::Kind::Disconnected
                                       : FeishuGatewayEvent::Kind::ConnectFailed;
        event.stage = stage;
        event.error_code = code;
        event.detail = reason;
        event.non_retryable = non_retryable;
        event.attempt = attempt_number;
        (void)EmitEvent(event);
        return RunOutcome{/*stable=*/false, /*fatal=*/non_retryable};
    };
    const auto fail_error = [&](const FeishuConnectError& error) {
        return fail(error.stage, error.error_code, error.detail, error.non_retryable);
    };

    // 1) 引导(每轮重连重新引导,§5.1):HTTP 归适配器的 provider,失败
    //    账由 provider 带上来(阶段 + 稳定码 + non_retryable)。
    const auto endpoint = options_.endpoint_provider();
    if (!endpoint.has_value()) {
        return fail_error(endpoint.error());
    }
    ping_interval_ms_ =
        std::clamp<std::int64_t>(endpoint->ping_interval_secs,
                                 options_.min_ping_interval_secs,
                                 options_.max_ping_interval_secs) *
        1000;
    service_id_ = QueryIntOf(endpoint->url, "service_id");

    // 2) WS 拨号。
    (void)EmitEvent(FeishuGatewayEvent{FeishuGatewayEvent::Kind::StageChanged,
                                       {}, {}, kStageConnecting});
    const auto connected = transport->Connect(endpoint->url);
    if (!connected.has_value()) {
        return fail_error(connected.error());
    }

    // 3) 激活即发 ping(§5.4)。此后心跳按 PingInterval 节拍锚定绝对时刻。
    const auto sent_ping = transport->SendBinary(SendPing(static_cast<std::int32_t>(service_id_)));
    if (!sent_ping.has_value()) {
        return fail(kStageConnecting, "activation_ping_failed",
                    "send activation ping: " + sent_ping.error());
    }
    reached_connected = true;
    state_.store(State::Running);
    (void)EmitEvent(FeishuGatewayEvent{FeishuGatewayEvent::Kind::StageChanged,
                                       {}, {}, kStageConnected});
    (void)EmitEvent(FeishuGatewayEvent{FeishuGatewayEvent::Kind::Connected});

    // 4) 运行循环:心跳 + 读超时监视 + 事件派发/ACK + 分片到期清扫。
    std::int64_t next_ping_at = options_.now_ms() + PingIntervalMs();
    std::int64_t read_deadline = options_.now_ms() + ReadTimeoutMs();
    bool stable = false;  // 收到过任一完整帧(含 pong)= 这轮稳定过
    while (!stop->load()) {
        const std::int64_t now = options_.now_ms();
        PurgeExpiredFragments(now);
        if (now >= read_deadline) {
            // §5.4:半开连接的死刑——2×interval+5s 没有任何帧。
            return fail(kStageConnected, "read_timeout",
                        "no frame within 2*pingInterval+5s");
        }
        if (now >= next_ping_at) {
            const auto sent = transport->SendBinary(
                SendPing(static_cast<std::int32_t>(service_id_)));
            if (!sent.has_value()) {
                return fail(kStageConnected, "heartbeat_send_failed",
                            "send ping: " + sent.error());
            }
            next_ping_at = options_.now_ms() + PingIntervalMs();
        }
        // 读窗:取心跳、读超时、分片到期三者最近的时限。
        std::int64_t budget = std::min(next_ping_at - now, read_deadline - now);
        if (const auto fragment_expiry = NextFragmentExpiryMs(now);
            fragment_expiry.has_value()) {
            budget = std::min(budget, *fragment_expiry - now);
        }
        if (budget <= 0) {
            continue;
        }
        const auto message =
            transport->ReadMessage(static_cast<int>(std::max<std::int64_t>(budget, 1)));
        if (!message.has_value()) {
            if (message.error().kind == transport::WsError::Kind::Timeout) {
                continue;  // 读窗到点:回循环顶(分片清扫/心跳/超时判定)
            }
            return fail(kStageConnected, ReadErrorCode(message.error()),
                        "read: " + message.error().detail);
        }
        // 每收一帧重置读超时(§5.4)。
        read_deadline = options_.now_ms() + ReadTimeoutMs();
        stable = true;
        std::string parse_error;
        const auto frame = DecodeFeishuFrame(*message, &parse_error);
        if (!frame.has_value()) {
            return fail(kStageConnected, "read_bad_frame", "frame: " + parse_error);
        }
        if (frame->method == kFrameMethodControl) {
            // 控制帧:pong(§5.4)。载荷空 = 纯保活;非空 = ClientConfig
            // 配置更新(可覆盖 PingInterval)。
            const std::string type = FeishuFrameHeaderValue(*frame, kHeaderType);
            if (type == kHeaderValuePong && frame->payload.has_value() &&
                !frame->payload->empty()) {
                const auto config = nlohmann::json::parse(*frame->payload, nullptr,
                                                          /*allow_exceptions=*/false);
                if (!config.is_discarded() && config.is_object() &&
                    config.contains("PingInterval") &&
                    config.at("PingInterval").is_number_integer()) {
                    const std::int64_t interval =
                        config.at("PingInterval").get<std::int64_t>();
                    if (interval > 0) {
                        ping_interval_ms_ =
                            std::clamp<std::int64_t>(
                                interval, options_.min_ping_interval_secs,
                                options_.max_ping_interval_secs) *
                            1000;
                        next_ping_at = options_.now_ms() + PingIntervalMs();
                        read_deadline = options_.now_ms() + ReadTimeoutMs();
                    }
                }
            }
            continue;
        }
        if (frame->method == kFrameMethodData) {
            if (const auto fatal = HandleDataFrame(*frame); fatal.has_value()) {
                return fail(kStageConnected, "ack_send_failed", *fatal);
            }
            continue;
        }
        // 陌生 method(协议外):记账不崩(宽容读,照 QQ A09 的口径)。
        unsupported_events_.fetch_add(1, std::memory_order_relaxed);
    }
    // stop 置位:干净收场(不算失败,不涨退避)。
    clear_in_flight();
    transport->Close(1000, "stop");
    return RunOutcome{/*stable=*/stable, /*fatal=*/false};
}

}  // namespace lubancode::channel::feishu
