#include "channel/feishu/feishu_adapter.hpp"

#include <chrono>
#include <sstream>
#include <utility>

#include "channel/bridge_protocol.hpp"
#include "channel/feishu/feishu_proto.hpp"
#include "channel/frame.hpp"

namespace lubancode::channel::feishu {

namespace {

// 引导(§5.1):POST {bootstrap_url} 拿 WSS 地址 + ClientConfig。失败带
// 稳定码与阶段;分型照 ClassifyBootstrapFailure——服务端错可重试,凭据类
// 不重试。AppSecret 只进请求体;错误 detail 不带请求内容与 URL query。
std::expected<FeishuEndpoint, FeishuConnectError> FetchEndpoint(
    const FeishuHttpFunc& http, const std::string& bootstrap_url,
    const std::string& app_id, const std::string& app_secret) {
    FeishuHttpRequest request;
    request.method = "POST";
    request.url = bootstrap_url;
    request.headers.emplace_back("Content-Type", "application/json");
    request.body = BuildBootstrapRequest(app_id, app_secret).dump();
    const auto response = http(request);
    if (!response.has_value()) {
        return std::unexpected(FeishuConnectError{
            kStageBootstrapping, "bootstrap_http_failed", response.error()});
    }
    const auto parsed = nlohmann::json::parse(response->body, nullptr,
                                              /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        return std::unexpected(FeishuConnectError{
            kStageBootstrapping, "bootstrap_bad_response",
            "bootstrap response not json"});
    }
    std::string parse_error;
    const auto endpoint = ParseBootstrapResponse(parsed, &parse_error);
    if (endpoint.has_value()) {
        return *endpoint;
    }
    const FeishuBootstrapErrorKind kind = ClassifyBootstrapFailure(response->status,
                                                                   parsed);
    std::string code;
    bool non_retryable = false;
    switch (kind) {
        case FeishuBootstrapErrorKind::ServerError:
            code = "bootstrap_server_error";
            break;
        case FeishuBootstrapErrorKind::RateLimited:
            code = "bootstrap_rate_limited";
            break;
        case FeishuBootstrapErrorKind::BadResponse:
            code = "bootstrap_bad_response";
            non_retryable = true;
            break;
        case FeishuBootstrapErrorKind::InvalidCredentials:
            code = "bootstrap_invalid_credentials";
            non_retryable = true;
            break;
    }
    return std::unexpected(
        FeishuConnectError{kStageBootstrapping, code, parse_error, non_retryable});
}

// A08 停止章:把停止旗盖进每一笔出站 HTTP(引导/令牌/发送同走一只 seam)。
FeishuHttpFunc StampStopFlag(FeishuHttpFunc func, const std::atomic<bool>* stop) {
    return [func = std::move(func), stop](const FeishuHttpRequest& request)
               -> std::expected<FeishuHttpResponse, std::string> {
        FeishuHttpRequest stamped = request;
        stamped.cancel = stop;
        return func(stamped);
    };
}

}  // namespace

nlohmann::json FeishuBotCapabilities() {
    nlohmann::json capabilities = nlohmann::json::object();
    capabilities["transports"] = nlohmann::json::array({"websocket", "direct"});
    capabilities["delivery"] = nlohmann::json::array({"send"});
    // 范围纪律:首版只做文本进出,不虚报媒体/流式/互动。
    capabilities["streaming"] = false;
    capabilities["credentials"] = true;
    capabilities["interactions"] = false;
    return capabilities;
}

FeishuBotAdapter::FeishuBotAdapter(Options options)
    : options_(std::move(options)),
      stop_(false),
      token_manager_(FeishuTokenManager::Options{
          options_.config.app_id,
          options_.credential.secret,
          StampStopFlag(options_.http, &stop_),
          options_.now_ms,
          options_.open_base + "/open-apis/auth/v3/tenant_access_token/internal",
          /*refresh_margin_secs=*/300}) {
    if (options_.bootstrap_url.empty()) {
        options_.bootstrap_url = options_.open_base + "/callback/ws/endpoint";
    }
    options_.http = StampStopFlag(options_.http, &stop_);
}

FeishuBotAdapter::~FeishuBotAdapter() {
    StopGatewayLocked("adapter destruct");
    if (gateway_thread_ != nullptr && gateway_thread_->joinable()) {
        gateway_thread_->join();
    }
    if (sender_thread_ != nullptr && sender_thread_->joinable()) {
        sender_thread_->join();
    }
}

std::size_t FeishuBotAdapter::spool_pending_count() const {
    return spool_.has_value() ? spool_->pending_count() : 0;
}

void FeishuBotAdapter::SetSpoolAppendFaultForTest(bool fail) {
    if (spool_.has_value()) {
        spool_->SetAppendFaultForTest(fail);
    }
}

void FeishuBotAdapter::WriteToSidecar(const std::byte* data, std::size_t size) {
    // 帧收发机械在共用件(SV-08):坏帧的 Fatal 通知、半帧等待、整帧分派
    // 全由 bridge_ 处置,这里只递业务分派口。
    bridge_.Feed(data, size,
                 [this](const nlohmann::json& frame) { HandleHostFrame(frame); });
}

std::vector<std::byte> FeishuBotAdapter::DrainFromSidecar() { return bridge_.Drain(); }

void FeishuBotAdapter::ReplyResult(std::int64_t id, const nlohmann::json& result) {
    bridge_.ReplyResult(id, result);
}

void FeishuBotAdapter::ReplyDomainError(std::int64_t id, DomainErrorName name,
                                         const std::string& detail) {
    bridge_.ReplyDomainError(id, name, detail);
}

void FeishuBotAdapter::EmitNotification(BridgeMethod method, const nlohmann::json& params) {
    bridge_.Notify(method, params);
}

std::string FeishuBotAdapter::NextDeliveryId() {
    std::ostringstream out;
    out << "feishu-del-" << delivery_counter_.fetch_add(1) << "-" << std::hex
        << delivery_rng_();
    return out.str();
}

void FeishuBotAdapter::HandleHostFrame(const nlohmann::json& frame_json) {
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
        return;  // channel.typing:首版不宣称
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
                    "feishu adapter speaks " + std::string(kBridgeHandshakeProtocolVersion) +
                        ", host asked " + protocol_version);
                return;
            }
            nlohmann::json result = nlohmann::json::object();
            result["protocol_version"] = std::string(kBridgeHandshakeProtocolVersion);
            result["adapter"] = {{"name", "feishu-inproc"}, {"version", "1.0.0"}};
            result["capabilities"] = FeishuBotCapabilities();
            result["account_state_version"] = 1;
            ReplyResult(id, result);
            return;
        }
        case BridgeMethod::Start: {
            if (!StartGatewayLocked()) {
                // spool 开不了账:不虚报 started。
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
            // params: {conversation, parts(text), reply_to_message_id, client_id}
            // 首版只发文本(范围纪律):text part 拼正文;非 text part 明拒
            //(not_capable,不悄悄丢)。reply_to_message_id 是被动回复锚
            //(来信 message_id)——飞书 reply API 必须带锚,主动推送首版不做。
            PendingSend pending;
            pending.request_id = id;
            std::string content;
            if (message.params.contains("parts") && message.params.at("parts").is_array()) {
                for (const auto& part : message.params.at("parts")) {
                    if (!part.is_object()) {
                        continue;
                    }
                    if (part.contains("type") && part.at("type") == "text" &&
                        part.contains("text") && part.at("text").is_string()) {
                        if (!content.empty()) {
                            content += "\n";
                        }
                        content += part.at("text").get<std::string>();
                        continue;
                    }
                    if (part.contains("type") && part.at("type") != "text") {
                        ReplyDomainError(
                            id, DomainErrorName::NotCapable,
                            "feishu adapter first version sends text only");
                        return;
                    }
                }
            }
            if (message.params.contains("reply_to_message_id") &&
                message.params.at("reply_to_message_id").is_string()) {
                pending.request.message_id =
                    message.params.at("reply_to_message_id").get<std::string>();
            }
            if (message.params.contains("client_id") &&
                message.params.at("client_id").is_string()) {
                pending.request.outbound_delivery_id =
                    message.params.at("client_id").get<std::string>();
            }
            if (pending.request.message_id.empty() || content.empty()) {
                ReplyDomainError(
                    id, DomainErrorName::NotCapable,
                    "channel.send needs reply_to_message_id anchor and text "
                    "(feishu proactive push not implemented in first version)");
                return;
            }
            pending.request.text = std::move(content);
            if (pending.request.outbound_delivery_id.empty()) {
                pending.request.outbound_delivery_id = NextDeliveryId();
            }
            {
                const std::lock_guard<std::mutex> lock(send_mutex_);
                send_queue_.push_back(std::move(pending));
            }
            sender_wake_.notify_all();
            return;
        }
        case BridgeMethod::InboundAck: {
            const std::string delivery_id = message.params.value("delivery_id", "");
            if (spool_.has_value()) {
                if (const auto error = spool_->RemoveAcked(delivery_id)) {
                    EmitNotification(BridgeMethod::Fatal,
                                     nlohmann::json{{"reason", "spool_write_failed"},
                                                    {"detail", *error}});
                }
            }
            ReplyResult(id, nlohmann::json{{"acked", true}});
            return;
        }
        case BridgeMethod::Health: {
            const channel::ConnectionSnapshot snapshot = ConnectionState();
            nlohmann::json result = nlohmann::json::object();
            result["state"] = session_ ? session_->state_name() : std::string("stopped");
            result["connected"] = snapshot.connected;
            result["thread_alive"] = snapshot.thread_alive;
            result["stage"] = snapshot.stage;
            result["cursor"] = nullptr;
            result["backlog"] = spool_pending_count();
            result["unsupported_events"] = unsupported_event_count();
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
            // edit/react/interaction/login 族:首版能力表不宣称,明拒。
            ReplyDomainError(
                id, DomainErrorName::NotCapable,
                std::string("feishu adapter does not implement ") +
                    BridgeMethodName(*message.method));
            return;
    }
}

bool FeishuBotAdapter::StartGatewayLocked() {
    if (gateway_thread_ != nullptr) {
        return true;  // 幂等
    }
    // spool:随 start 开(路径 <root>/feishu/<acct>/spool/pending,与 qq
    // 互不相见)。
    if (!spool_.has_value()) {
        const auto spool_dir = options_.state_root / options_.channel_id /
                               options_.account_id / "spool" / "pending";
        if (auto spool = qq::QqSpoolStore::Open(spool_dir); spool.has_value()) {
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
        FeishuMessageSender::Options sender_options;
        sender_options.http = options_.http;
        sender_options.tokens = &token_manager_;
        sender_options.open_base = options_.open_base;
        sender_.emplace(std::move(sender_options));
    }
    stop_.store(false);
    // 重启(桥 stop -> start)时重置在线账;last_failure 保留到下次成功。
    {
        const std::lock_guard<std::mutex> lock(connection_mutex_);
        connection_.connected = false;
        connection_.connected_since_ms = 0;
        connection_.stage = kStageBootstrapping;
        connection_.retry_count = 0;
        connection_.next_retry_at_ms = 0;
        connection_.updated_at_ms = options_.now_ms();
    }

    FeishuGatewaySession::Options gateway_options;
    gateway_options.transport_factory = options_.transport_factory;
    // endpoint provider:每轮连接重新引导(§5.1)。信任根加载失败先短路
    //(本地已知 TLS 不可用就不发引导请求);阶段事件照发(§三口径)。
    gateway_options.endpoint_provider =
        [this]() -> std::expected<FeishuEndpoint, FeishuConnectError> {
        if (!options_.trust_load_block_code.empty()) {
            return std::unexpected(FeishuConnectError{
                kStageBootstrapping, options_.trust_load_block_code,
                options_.trust_load_block_detail.empty()
                    ? "信任根加载失败(装配预检),已阻断联网重试"
                    : options_.trust_load_block_detail});
        }
        {
            FeishuGatewayEvent event;
            event.kind = FeishuGatewayEvent::Kind::StageChanged;
            event.stage = kStageBootstrapping;
            (void)HandleGatewayEvent(event);
        }
        return FetchEndpoint(options_.http, options_.bootstrap_url,
                             options_.config.app_id, options_.credential.secret);
    };
    gateway_options.on_event = [this](const FeishuGatewayEvent& event) {
        return HandleGatewayEvent(event);
    };
    gateway_options.now_ms = options_.now_ms;
    session_ = std::make_unique<FeishuGatewaySession>(std::move(gateway_options));
    gateway_thread_ = std::make_unique<std::thread>([this]() { session_->RunLoop(&stop_); });

    if (sender_thread_ == nullptr) {
        sender_thread_ = std::make_unique<std::thread>([this]() { SenderLoop(); });
    }
    return true;
}

void FeishuBotAdapter::StopGatewayLocked(const std::string& reason) {
    (void)reason;
    // 停止旗先行(A08):HTTP 取消章即刻生效,WS 经 CancelInFlight 打断。
    stop_.store(true);
    if (session_ != nullptr) {
        session_->CancelInFlight();
    }
    if (gateway_thread_ != nullptr && gateway_thread_->joinable()) {
        gateway_thread_->join();
    }
    gateway_thread_.reset();
    session_.reset();
    // 发送线程收口:排空队列(未发的如实报错回宿主)后退场;收口后指针
    // 清零,桥 stop→start 重开新线程。
    sender_wake_.notify_all();
    if (sender_thread_ != nullptr && sender_thread_->joinable()) {
        sender_thread_->join();
    }
    sender_thread_.reset();
}

FeishuGatewayAck FeishuBotAdapter::HandleGatewayEvent(const FeishuGatewayEvent& event) {
    switch (event.kind) {
        case FeishuGatewayEvent::Kind::MessageReceive: {
            if (!spool_.has_value()) {
                // 没开账(防御,理论不可达):这条没接住,回 500 让平台重推。
                return FeishuGatewayAck::Retry;
            }
            const std::string delivery_id = NextDeliveryId();
            const auto mapping = MapFeishuEventPayload(
                event.event_payload, options_.channel_id, options_.account_id,
                options_.config.app_id, delivery_id, options_.now_ms());
            if (mapping.kind == FeishuEventMapping::Kind::UnsupportedEventType ||
                mapping.kind == FeishuEventMapping::Kind::Invalid) {
                // 明确终结(判不支持/判无效):计数留痕,ACK 200——重发一条
                // 解不开的事件只会再判一次。
                unsupported_event_count_.fetch_add(1, std::memory_order_relaxed);
                EmitNotification(BridgeMethod::Fatal,
                                 nlohmann::json{{"reason", "invalid_frame"},
                                                {"detail", "feishu event: " +
                                                               mapping.detail}});
                return FeishuGatewayAck::Ok;
            }
            const nlohmann::json event_json = mapping.event.ToJson();
            if (const auto spool_error = spool_->AppendPending(delivery_id, event_json)) {
                // 落盘失败(磁盘满/权限拒):没接住——ACK 500 让平台重推
                //(宿主 ingress 去重兜底,不丢信)。Fatal 让宿主留痕。
                EmitNotification(BridgeMethod::Fatal,
                                 nlohmann::json{{"reason", "spool_write_failed"},
                                                {"detail", *spool_error}});
                return FeishuGatewayAck::Retry;
            }
            EmitNotification(BridgeMethod::Inbound, event_json);
            return FeishuGatewayAck::Ok;
        }
        case FeishuGatewayEvent::Kind::UnsupportedEvent: {
            // card/未知事件类型/无效载荷:网关侧已计数,这里只同步适配器
            // 的账(Health 投影)。
            unsupported_event_count_.fetch_add(1, std::memory_order_relaxed);
            return FeishuGatewayAck::Ok;
        }
        case FeishuGatewayEvent::Kind::StageChanged: {
            const std::lock_guard<std::mutex> lock(connection_mutex_);
            connection_.stage = event.stage.empty() ? std::string(kStageConnecting)
                                                    : event.stage;
            connection_.updated_at_ms = options_.now_ms();
            return FeishuGatewayAck::Ok;
        }
        case FeishuGatewayEvent::Kind::Connected: {
            // connected 只在拨通 + 激活 ping 发出后成立;连接成功把错误移入
            // 历史、当前清空(§三口径)。
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
            return FeishuGatewayAck::Ok;
        }
        case FeishuGatewayEvent::Kind::ConnectFailed:
        case FeishuGatewayEvent::Kind::Disconnected: {
            {
                const std::lock_guard<std::mutex> lock(connection_mutex_);
                connection_.connected = false;
                connection_.connected_since_ms = 0;
                connection_.last_failure = channel::ConnectionFailure{
                    event.stage, event.error_code, event.detail, options_.now_ms(),
                    event.attempt};
                connection_.updated_at_ms = options_.now_ms();
            }
            EmitNotification(BridgeMethod::Status,
                             nlohmann::json{{"state", "backoff"},
                                            {"connected", false},
                                            {"stage", event.stage},
                                            {"error_code", event.error_code},
                                            {"detail", event.detail},
                                            {"non_retryable", event.non_retryable}});
            return FeishuGatewayAck::Ok;
        }
        case FeishuGatewayEvent::Kind::BackoffScheduled: {
            const std::lock_guard<std::mutex> lock(connection_mutex_);
            connection_.retry_count = event.attempt;
            connection_.next_retry_at_ms = event.next_retry_at_ms;
            connection_.updated_at_ms = options_.now_ms();
            return FeishuGatewayAck::Ok;
        }
        case FeishuGatewayEvent::Kind::Stopped: {
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
            return FeishuGatewayAck::Ok;
        }
    }
    return FeishuGatewayAck::Ok;
}

channel::ConnectionSnapshot FeishuBotAdapter::ConnectionState() const {
    const std::lock_guard<std::mutex> lock(connection_mutex_);
    channel::ConnectionSnapshot snapshot = connection_;
    snapshot.thread_alive = gateway_thread_ != nullptr;
    if (snapshot.stage.empty()) {
        snapshot.stage = snapshot.thread_alive ? std::string(kStageBootstrapping)
                                               : std::string("idle");
    }
    return snapshot;
}

void FeishuBotAdapter::SenderLoop() {
    // 退避重试(DeferredRetry):1s/2s/4s 三次,仍失败按稳定名回宿主。
    constexpr int kMaxAttempts = 3;
    while (true) {
        std::unique_lock<std::mutex> lock(send_mutex_);
        sender_wake_.wait(lock, [this]() { return !send_queue_.empty() || stop_.load(); });
        // 停止(A08 有界收口):不再起网络,队列整批排空——未发的如实回
        // domain 错,随后线程退出。
        if (stop_.load()) {
            std::vector<PendingSend> sends = std::move(send_queue_);
            send_queue_.clear();
            lock.unlock();
            for (const PendingSend& pending : sends) {
                ReplyDomainError(pending.request_id, DomainErrorName::TransportFailed,
                                 "adapter stopped before send");
            }
            return;
        }
        if (send_queue_.empty()) {
            continue;
        }
        PendingSend pending = std::move(send_queue_.front());
        send_queue_.erase(send_queue_.begin());
        lock.unlock();

        FeishuMessageSender::Outcome outcome = sender_->SendReply(pending.request);
        int attempt = pending.attempts;
        while (outcome.status == FeishuMessageSender::Outcome::Status::DeferredRetry &&
               attempt < kMaxAttempts) {
            if (stop_.load()) {
                break;  // 停止路径:不再退避重试
            }
            // 退避睡可打断(A08):每 100ms 查停止旗。
            const int backoff_ms = 1000 * (1 << attempt);
            for (int slept = 0; slept < backoff_ms && !stop_.load(); slept += 100) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::min(100, backoff_ms - slept)));
            }
            if (stop_.load()) {
                break;
            }
            ++attempt;
            outcome = sender_->SendReply(pending.request);
        }

        switch (outcome.status) {
            case FeishuMessageSender::Outcome::Status::Sent:
                ReplyResult(pending.request_id,
                            nlohmann::json{{"provider_message_id",
                                            outcome.provider_message_id},
                                           {"accepted", true}});
                break;
            case FeishuMessageSender::Outcome::Status::DeferredRetry:
                ReplyDomainError(pending.request_id, DomainErrorName::RateLimited,
                                 "deferred after retries: " + outcome.error.detail);
                break;
            case FeishuMessageSender::Outcome::Status::PermanentFail:
                switch (outcome.error.kind) {
                    case FeishuApiErrorKind::Unauthorized:
                        ReplyDomainError(pending.request_id,
                                         DomainErrorName::LoginRequired,
                                         "unauthorized after token refresh");
                        break;
                    case FeishuApiErrorKind::PermissionDenied:
                        ReplyDomainError(pending.request_id,
                                         DomainErrorName::NotCapable,
                                         "permission denied: " + outcome.error.detail);
                        break;
                    default:
                        ReplyDomainError(pending.request_id,
                                         DomainErrorName::PermanentReject,
                                         outcome.error.detail);
                        break;
                }
                break;
        }
    }
}

}  // namespace lubancode::channel::feishu
