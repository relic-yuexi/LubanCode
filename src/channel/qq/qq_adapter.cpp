#include "channel/qq/qq_adapter.hpp"

#include <chrono>
#include <sstream>
#include <utility>

#include "channel/bridge_protocol.hpp"
#include "channel/frame.hpp"
#include "channel/qq/qq_proto.hpp"

namespace lubancode::channel::qq {

namespace {

// GET /gateway 的响应:{"url": "wss://..."}(官方 API)。失败带稳定码与
// 阶段(连接状态单 §三:查询地址是独立阶段,不与取令牌混报)。
std::expected<std::string, GatewayConnectError> FetchGatewayUrl(const QqHttpFunc& http,
                                                                const std::string& api_base,
                                                                const std::string& token) {
    QqHttpRequest request;
    request.method = "GET";
    request.url = api_base + "/gateway";
    request.headers.emplace_back("Authorization", "QQBot " + token);
    const auto response = http(request);
    if (!response.has_value()) {
        return std::unexpected(GatewayConnectError{
            kStageFetchingGatewayUrl, "gateway_url_http_failed", response.error()});
    }
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected(GatewayConnectError{
            kStageFetchingGatewayUrl, "gateway_url_http_failed",
            "gateway url status " + std::to_string(response->status)});
    }
    const auto parsed =
        nlohmann::json::parse(response->body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("url") ||
        !parsed.at("url").is_string()) {
        return std::unexpected(GatewayConnectError{
            kStageFetchingGatewayUrl, "gateway_url_bad_response",
            "gateway url response missing url"});
    }
    return parsed.at("url").get<std::string>();
}

// QqTokenManager 错误分型 -> 稳定码(取令牌阶段)。
std::string TokenErrorCode(QqTokenManager::ErrorKind kind) {
    switch (kind) {
        case QqTokenManager::ErrorKind::InvalidCredentials:
            return "token_invalid_credentials";
        case QqTokenManager::ErrorKind::ServerError:
            return "token_server_error";
        case QqTokenManager::ErrorKind::RateLimited:
            return "token_rate_limited";
        case QqTokenManager::ErrorKind::NetworkError:
            break;
    }
    return "token_network_failed";
}

}  // namespace

nlohmann::json QqBotCapabilities() {
    nlohmann::json capabilities = nlohmann::json::object();
    capabilities["transports"] = nlohmann::json::array({"websocket", "direct"});
    capabilities["delivery"] = nlohmann::json::array({"send"});
    capabilities["media"] = nullptr;  // Q4 之前不虚报(显式空)
    capabilities["streaming"] = false;
    capabilities["credentials"] = true;
    return capabilities;
}

QqBotAdapter::QqBotAdapter(Options options)
    : options_(std::move(options)),
      token_manager_(QqTokenManager::Options{
          options_.config.app_id,
          options_.credential.secret,
          options_.http,
          options_.now_ms,
          options_.bots_base + "/app/getAppAccessToken",
          /*refresh_margin_secs=*/300}) {}

QqBotAdapter::~QqBotAdapter() {
    StopGatewayLocked("adapter destruct");
    if (gateway_thread_ != nullptr && gateway_thread_->joinable()) {
        gateway_thread_->join();
    }
    if (sender_thread_ != nullptr && sender_thread_->joinable()) {
        sender_thread_->join();
    }
}

std::size_t QqBotAdapter::spool_pending_count() const {
    return spool_.has_value() ? spool_->pending_count() : 0;
}

void QqBotAdapter::WriteToSidecar(const std::byte* data, std::size_t size) {
    host_frame_decoder_.Feed(data, size);
    while (true) {
        auto next = host_frame_decoder_.TryDecodeNext();
        if (!next.has_value()) {
            // 宿主来向帧坏:Fatal 通知(协议错由宿主状态机处置)。
            EmitNotification(BridgeMethod::Fatal,
                             nlohmann::json{{"reason", "invalid_frame"},
                                            {"detail", next.error().message}});
            return;
        }
        if (!next->has_value()) {
            return;  // 半帧,等更多字节
        }
        HandleHostFrame(**next);
    }
}

std::vector<std::byte> QqBotAdapter::DrainFromSidecar() {
    const std::lock_guard<std::mutex> lock(host_mutex_);
    std::vector<std::byte> out = std::move(to_host_);
    to_host_.clear();
    return out;
}

void QqBotAdapter::ReplyResult(std::int64_t id, const nlohmann::json& result) {
    const std::lock_guard<std::mutex> lock(host_mutex_);
    if (const auto encoded = EncodeFrame(BuildResultResponseJson(id, result));
        encoded.has_value()) {
        to_host_.insert(to_host_.end(), encoded->begin(), encoded->end());
    }
}

void QqBotAdapter::ReplyDomainError(std::int64_t id, DomainErrorName name,
                                    const std::string& detail) {
    const std::lock_guard<std::mutex> lock(host_mutex_);
    if (const auto encoded = EncodeFrame(BuildDomainErrorResponseJson(id, name, detail));
        encoded.has_value()) {
        to_host_.insert(to_host_.end(), encoded->begin(), encoded->end());
    }
}

void QqBotAdapter::EmitNotification(BridgeMethod method, const nlohmann::json& params) {
    const std::lock_guard<std::mutex> lock(host_mutex_);
    if (const auto encoded = EncodeFrame(BuildNotificationJson(method, params));
        encoded.has_value()) {
        to_host_.insert(to_host_.end(), encoded->begin(), encoded->end());
    }
}

std::string QqBotAdapter::NextDeliveryId() {
    std::ostringstream out;
    out << "qq-del-" << delivery_counter_.fetch_add(1) << "-"
        << std::hex << delivery_rng_();
    return out.str();
}

void QqBotAdapter::HandleHostFrame(const nlohmann::json& frame_json) {
    const IncomingMessage message = ParseIncomingMessage(frame_json);
    if (message.kind == IncomingMessageKind::Malformed) {
        EmitNotification(BridgeMethod::Fatal,
                         nlohmann::json{{"reason", "invalid_frame"},
                                        {"detail", message.malformed_reason}});
        return;
    }
    if (message.kind == IncomingMessageKind::ResultResponse ||
        message.kind == IncomingMessageKind::ErrorResponse) {
        return;  // 宿主不会回 response 给 sidecar(v1 协议 sidecar 不发 request)
    }
    if (!message.method.has_value()) {
        if (message.kind == IncomingMessageKind::Request && message.id.has_value()) {
            ReplyDomainError(*message.id, DomainErrorName::NotCapable,
                             "unknown method: " + message.method_name);
        }
        return;
    }
    if (message.kind == IncomingMessageKind::Notification) {
        return;  // channel.typing:平台有 input_notify 能力才接(Q1 不宣称)
    }
    if (!message.id.has_value()) {
        return;
    }
    const std::int64_t id = *message.id;
    if (const auto shape_error =
            ValidateMethodParamsShape(*message.method, message.params)) {
        ReplyDomainError(id, DomainErrorName::NotCapable, *shape_error);
        return;
    }

    switch (*message.method) {
        case BridgeMethod::Initialize: {
            const std::string protocol_version = message.params.value("protocol_version", "");
            if (protocol_version != kBridgeHandshakeProtocolVersion) {
                ReplyDomainError(
                    id, DomainErrorName::ProtocolIncompatible,
                    "qq adapter speaks " + std::string(kBridgeHandshakeProtocolVersion) +
                        ", host asked " + protocol_version);
                return;
            }
            nlohmann::json result = nlohmann::json::object();
            result["protocol_version"] = std::string(kBridgeHandshakeProtocolVersion);
            result["adapter"] = {{"name", "qqbot-inproc"}, {"version", "1.0.0"}};
            result["capabilities"] = QqBotCapabilities();
            result["account_state_version"] = 1;
            ReplyResult(id, result);
            return;
        }
        case BridgeMethod::Start: {
            if (!StartGatewayLocked()) {
                // spool 开不了账:不虚报 started——按 domain 错回宿主,状态机
                // 进 Degraded 留痕,不冒充运行。
                ReplyDomainError(id, DomainErrorName::SpawnFailed,
                                 "spool store open failed");
                return;
            }
            nlohmann::json result = nlohmann::json::object();
            result["started"] = true;
            result["transport"] = message.params.value("transport", "websocket");
            ReplyResult(id, result);
            return;
        }
        case BridgeMethod::Stop: {
            StopGatewayLocked("host stop");
            ReplyResult(id, nlohmann::json{{"stopped", true}, {"flushed", true}});
            return;
        }
        case BridgeMethod::Send: {
            // params: {conversation, parts, reply_to_message_id?, client_id?}
            PendingSend pending;
            pending.request_id = id;
            if (message.params.contains("conversation") &&
                message.params.at("conversation").is_object() &&
                message.params.at("conversation").contains("id") &&
                message.params.at("conversation").at("id").is_string()) {
                pending.request.openid =
                    message.params.at("conversation").at("id").get<std::string>();
            }
            std::string content;
            if (message.params.contains("parts") && message.params.at("parts").is_array()) {
                for (const auto& part : message.params.at("parts")) {
                    if (part.is_object() && part.contains("type") &&
                        part.at("type") == "text" && part.contains("text") &&
                        part.at("text").is_string()) {
                        if (!content.empty()) {
                            content += "\n";
                        }
                        content += part.at("text").get<std::string>();
                    }
                }
            }
            pending.request.content = std::move(content);
            if (message.params.contains("reply_to_message_id") &&
                message.params.at("reply_to_message_id").is_string()) {
                pending.request.msg_id =
                    message.params.at("reply_to_message_id").get<std::string>();
            }
            if (message.params.contains("client_id") &&
                message.params.at("client_id").is_string()) {
                pending.request.outbound_delivery_id =
                    message.params.at("client_id").get<std::string>();
            }
            if (pending.request.openid.empty() || pending.request.content.empty()) {
                ReplyDomainError(id, DomainErrorName::NotCapable,
                                 "channel.send needs direct conversation id and text");
                return;
            }
            if (pending.request.outbound_delivery_id.empty()) {
                pending.request.outbound_delivery_id = NextDeliveryId();
            }
            {
                const std::lock_guard<std::mutex> lock(host_mutex_);
                send_queue_.push_back(std::move(pending));
            }
            sender_wake_.notify_all();
            return;
        }
        case BridgeMethod::InboundAck: {
            const std::string delivery_id = message.params.value("delivery_id", "");
            if (spool_.has_value()) {
                if (const auto error = spool_->RemoveAcked(delivery_id)) {
                    // 清理失败:留 Fatal 痕(事件不重投——宿主已确认过)。
                    EmitNotification(BridgeMethod::Fatal,
                                     nlohmann::json{{"reason", "spool_write_failed"},
                                                    {"detail", *error}});
                }
            }
            ReplyResult(id, nlohmann::json{{"acked", true}});
            return;
        }
        case BridgeMethod::Health: {
            // 连接状态单 §三:connected 只认 READY/RESUMED(不再拿线程存活
            // 冒充在线);last_error 带最近失败(不再恒空)。
            const ConnectionSnapshot snapshot = ConnectionState();
            nlohmann::json result = nlohmann::json::object();
            result["state"] = session_ ? session_->state_name() : std::string("stopped");
            result["connected"] = snapshot.connected;
            result["thread_alive"] = snapshot.thread_alive;
            result["stage"] = snapshot.stage;
            result["cursor"] = nullptr;
            result["backlog"] = spool_pending_count();
            if (snapshot.last_failure.has_value()) {
                result["last_error"] = nlohmann::json{
                    {"stage", snapshot.last_failure->stage},
                    {"error_code", snapshot.last_failure->error_code},
                    {"detail", snapshot.last_failure->detail},
                    {"at_ms", snapshot.last_failure->at_ms}};
            } else {
                result["last_error"] = nullptr;
            }
            result["retry_count"] = snapshot.retry_count;
            result["next_retry_at_ms"] = snapshot.next_retry_at_ms;
            ReplyResult(id, result);
            return;
        }
        default:
            // edit/react/login 族:首版能力表不宣称,明拒。
            ReplyDomainError(
                id, DomainErrorName::NotCapable,
                std::string("qq adapter does not implement ") + BridgeMethodName(*message.method));
            return;
    }
}

bool QqBotAdapter::StartGatewayLocked() {
    if (gateway_thread_ != nullptr) {
        return true;  // 幂等
    }
    // spool:随 start 开(独立于 stop 关闭——重启重投在下轮连接里做)。
    if (!spool_.has_value()) {
        const auto spool_dir = options_.state_root / options_.channel_id / options_.account_id /
                               "spool" / "pending";
        if (auto spool = QqSpoolStore::Open(spool_dir); spool.has_value()) {
            spool_ = std::move(*spool);
        } else {
            return false;  // 账开不了:如实失败,不虚报 started
        }
        // 重启重投:历史 pending 全部重新上报(宿主 ingress 去重键兜底)。
        for (const auto& [delivery_id, event_json] : spool_->ListPending()) {
            EmitNotification(BridgeMethod::Inbound, event_json);
        }
    }
    if (!sender_.has_value()) {
        QqMessageSender::Options sender_options;
        sender_options.http = options_.http;
        sender_options.tokens = &token_manager_;
        sender_options.api_base = options_.api_base;
        sender_.emplace(std::move(sender_options));
    }
    stop_.store(false);
    // 重启(桥 stop -> start)时重置在线账;last_failure 保留到下次成功——
    // 用户看得到上一次为什么失败(§三)。
    {
        const std::lock_guard<std::mutex> lock(connection_mutex_);
        connection_.connected = false;
        connection_.connected_since_ms = 0;
        connection_.stage = kStageConnecting;
        connection_.retry_count = 0;
        connection_.next_retry_at_ms = 0;
        connection_.updated_at_ms = options_.now_ms();
    }

    QqGatewaySession::Options gateway_options;
    gateway_options.transport_factory = options_.transport_factory;
    // provider 发阶段事件(取令牌/查地址两段独立可见,§三);失败带
    // GatewayConnectError(阶段 + 稳定码 + 脱敏 detail)。
    gateway_options.gateway_url_provider =
        [this]() -> std::expected<std::string, GatewayConnectError> {
        HandleGatewayEvent(GatewayEvent{GatewayEvent::Kind::StageChanged,
                                        nlohmann::json::object(), std::string(), -1,
                                        kStageFetchingToken});
        const auto token = token_manager_.GetValidToken();
        if (!token.has_value()) {
            return std::unexpected(GatewayConnectError{
                kStageFetchingToken, TokenErrorCode(token.error().kind),
                "token: " + token.error().detail});
        }
        HandleGatewayEvent(GatewayEvent{GatewayEvent::Kind::StageChanged,
                                        nlohmann::json::object(), std::string(), -1,
                                        kStageFetchingGatewayUrl});
        return FetchGatewayUrl(options_.http, options_.api_base, *token);
    };
    gateway_options.token_provider =
        [this]() -> std::expected<std::string, GatewayConnectError> {
        // 网关线程在 identifying 阶段再取(鉴权窗);失败同样带阶段与码。
        const auto token = token_manager_.GetValidToken();
        if (!token.has_value()) {
            return std::unexpected(GatewayConnectError{
                kStageIdentifying, TokenErrorCode(token.error().kind),
                "token: " + token.error().detail});
        }
        return *token;
    };
    gateway_options.on_event = [this](const GatewayEvent& event) { HandleGatewayEvent(event); };
    gateway_options.now_ms = options_.now_ms;
    session_ = std::make_unique<QqGatewaySession>(std::move(gateway_options));
    gateway_thread_ = std::make_unique<std::thread>([this]() { session_->RunLoop(&stop_); });

    if (sender_thread_ == nullptr) {
        sender_thread_ =
            std::make_unique<std::thread>([this]() { SenderLoop(); });
    }
    return true;
}

void QqBotAdapter::StopGatewayLocked(const std::string& reason) {
    (void)reason;
    if (gateway_thread_ == nullptr) {
        return;
    }
    stop_.store(true);
    session_->CancelInFlight();
    gateway_thread_->join();
    gateway_thread_.reset();
    session_.reset();
    sender_wake_.notify_all();
    // 发送线程随适配器生命周期存续(队列清空后自然空转;析构收口)。
}

void QqBotAdapter::HandleGatewayEvent(const GatewayEvent& event) {
    switch (event.kind) {
        case GatewayEvent::Kind::C2cMessageCreate: {
            if (!spool_.has_value()) {
                return;  // 没开账不收事件(防御)
            }
            const std::string delivery_id = NextDeliveryId();
            std::string map_error;
            const auto mapping = MapC2cMessageCreate(
                event.c2c_d, std::string(), options_.channel_id, options_.account_id,
                delivery_id, options_.now_ms(), &map_error);
            if (!mapping.has_value()) {
                // 事件解不开:不进 spool 不上报(无效正文不是可投递事实),
                // Fatal 留痕让宿主记账。
                EmitNotification(BridgeMethod::Fatal,
                                 nlohmann::json{{"reason", "invalid_frame"},
                                                {"detail", "c2c map: " + map_error}});
                return;
            }
            const nlohmann::json event_json = mapping->event.ToJson();
            if (const auto spool_error = spool_->AppendPending(delivery_id, event_json)) {
                // 落盘失败:停止上报该事件,退避后网关重连会重收(平台 at
                // least once)。Fatal 让宿主 Degraded 留痕。
                EmitNotification(BridgeMethod::Fatal,
                                 nlohmann::json{{"reason", "spool_write_failed"},
                                                {"detail", *spool_error}});
                return;
            }
            EmitNotification(BridgeMethod::Inbound, event_json);
            return;
        }
        case GatewayEvent::Kind::StageChanged: {
            // 阶段推进只记本地快照账,不刷宿主状态机(宿主只认
            // running/backoff/stopped 粗粒度状态)。
            const std::lock_guard<std::mutex> lock(connection_mutex_);
            connection_.stage = event.stage.empty() ? kStageConnecting : event.stage;
            connection_.updated_at_ms = options_.now_ms();
            return;
        }
        case GatewayEvent::Kind::SessionReady:
        case GatewayEvent::Kind::SessionResumed: {
            // connected 只在 READY/RESUMED 后成立;连接成功把错误移入
            // 历史、当前清空(§三)。
            {
                const std::lock_guard<std::mutex> lock(connection_mutex_);
                connection_.connected = true;
                connection_.connected_since_ms = options_.now_ms();
                connection_.stage = kStageConnected;
                if (connection_.last_failure.has_value()) {
                    connection_.failure_history.push_back(*connection_.last_failure);
                    if (connection_.failure_history.size() > 8) {
                        connection_.failure_history.erase(
                            connection_.failure_history.begin());
                    }
                    connection_.last_failure.reset();
                }
                connection_.retry_count = 0;
                connection_.next_retry_at_ms = 0;
                connection_.updated_at_ms = options_.now_ms();
            }
            EmitNotification(BridgeMethod::Status,
                             nlohmann::json{{"state", "running"}, {"connected", true},
                                            {"stage", kStageConnected}});
            return;
        }
        case GatewayEvent::Kind::ConnectFailed:
        case GatewayEvent::Kind::Disconnected: {
            // 失败保留 stage/稳定码/清洗说明;退避事件不进这里,根因不被
            // 覆盖(§三)。宿主粗粒度状态仍报 backoff(不动宿主状态机语义,
            // 细账走扩展字段)。
            {
                const std::lock_guard<std::mutex> lock(connection_mutex_);
                connection_.connected = false;
                connection_.connected_since_ms = 0;
                connection_.last_failure = ConnectionFailure{
                    event.stage, event.error_code, event.detail, options_.now_ms()};
                connection_.updated_at_ms = options_.now_ms();
            }
            EmitNotification(BridgeMethod::Status,
                             nlohmann::json{{"state", "backoff"},
                                            {"connected", false},
                                            {"stage", event.stage},
                                            {"error_code", event.error_code},
                                            {"detail", event.detail}});
            return;
        }
        case GatewayEvent::Kind::BackoffScheduled: {
            // 只记重试账,不碰 last_failure(退避不许覆盖根因,§三)。
            const std::lock_guard<std::mutex> lock(connection_mutex_);
            connection_.retry_count = event.attempt;
            connection_.next_retry_at_ms = event.next_retry_at_ms;
            connection_.updated_at_ms = options_.now_ms();
            return;
        }
        case GatewayEvent::Kind::SessionInvalidated: {
            {
                const std::lock_guard<std::mutex> lock(connection_mutex_);
                connection_.stage = kStageIdentifying;
                connection_.updated_at_ms = options_.now_ms();
            }
            EmitNotification(BridgeMethod::Status,
                             nlohmann::json{{"state", "backoff"}, {"connected", false},
                                            {"stage", kStageIdentifying}});
            return;
        }
        case GatewayEvent::Kind::Stopped: {
            // 停止:connected 立即 false(§三)。
            {
                const std::lock_guard<std::mutex> lock(connection_mutex_);
                connection_.connected = false;
                connection_.connected_since_ms = 0;
                connection_.stage = kStageStopped;
                connection_.next_retry_at_ms = 0;
                connection_.updated_at_ms = options_.now_ms();
            }
            EmitNotification(BridgeMethod::Status,
                             nlohmann::json{{"state", "stopped"}, {"connected", false},
                                            {"stage", kStageStopped}});
            return;
        }
    }
}

ConnectionSnapshot QqBotAdapter::ConnectionState() const {
    const std::lock_guard<std::mutex> lock(connection_mutex_);
    ConnectionSnapshot snapshot = connection_;
    snapshot.thread_alive = gateway_thread_ != nullptr;
    if (snapshot.stage.empty()) {
        snapshot.stage = snapshot.thread_alive ? kStageConnecting : std::string("idle");
    }
    return snapshot;
}

void QqBotAdapter::SenderLoop() {
    // 退避重试(DeferredRetry):1s/2s/4s 三次,仍失败按稳定名回宿主。
    constexpr int kMaxAttempts = 3;
    while (true) {
        std::unique_lock<std::mutex> lock(host_mutex_);
        sender_wake_.wait(lock, [this]() { return !send_queue_.empty() || stop_.load(); });
        if (send_queue_.empty()) {
            if (stop_.load()) {
                return;  // 停止且队列清空
            }
            continue;
        }
        PendingSend pending = std::move(send_queue_.front());
        send_queue_.erase(send_queue_.begin());
        lock.unlock();

        QqMessageSender::Outcome outcome = sender_->SendC2c(pending.request);
        int attempt = pending.attempts;
        while (outcome.status == QqMessageSender::Outcome::Status::DeferredRetry &&
               attempt < kMaxAttempts) {
            const int backoff_ms = 1000 * (1 << attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            if (stop_.load()) {
                break;  // 停止路径:尽快收口(未完成的发送如实报错)
            }
            ++attempt;
            outcome = sender_->SendC2c(pending.request);
        }

        switch (outcome.status) {
            case QqMessageSender::Outcome::Status::Sent:
            case QqMessageSender::Outcome::Status::Deduped:
                ReplyResult(pending.request_id,
                            nlohmann::json{{"provider_message_id",
                                            outcome.provider_message_id},
                                           {"accepted", true}});
                break;
            case QqMessageSender::Outcome::Status::DeferredRetry:
                ReplyDomainError(pending.request_id, DomainErrorName::RateLimited,
                                 "deferred after retries: " + outcome.error.detail);
                break;
            case QqMessageSender::Outcome::Status::PermanentFail:
                switch (outcome.error.kind) {
                    case QqApiErrorKind::MsgIdExpired:
                        ReplyDomainError(pending.request_id, DomainErrorName::PermanentReject,
                                         "msg_id expired");
                        break;
                    case QqApiErrorKind::Unauthorized:
                        ReplyDomainError(pending.request_id, DomainErrorName::LoginRequired,
                                         "unauthorized after token refresh");
                        break;
                    default:
                        ReplyDomainError(pending.request_id, DomainErrorName::PermanentReject,
                                         outcome.error.detail);
                        break;
                }
                break;
        }
    }
}

}  // namespace lubancode::channel::qq
