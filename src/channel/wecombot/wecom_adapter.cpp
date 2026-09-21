// wecom_adapter.hpp 的实现(见头注:线程模型与发送面)。
#include "channel/wecombot/wecom_adapter.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

#include "channel/bridge_protocol.hpp"
#include "channel/wecombot/wecom_proto.hpp"

namespace lubancode::channel::wecombot {

namespace {

// 一段的递交重试上限(1s/2s/4s 三退避,照 QQ DeferredRetry 口径)。
constexpr int kMaxSendAttempts = 3;
constexpr std::int64_t kRateMinuteWindowMs = 60'000;
constexpr std::int64_t kRateHourWindowMs = 3'600'000;

}  // namespace

nlohmann::json WecombotCapabilities() {
    nlohmann::json capabilities = nlohmann::json::object();
    capabilities["transports"] = nlohmann::json::array({"websocket", "direct"});
    capabilities["delivery"] = nlohmann::json::array({"send"});
    // W1 无媒体管道:两列空名单,如实报"无",不虚报可解析。
    capabilities["media"] = nlohmann::json{{"inbound", nlohmann::json::array()},
                                           {"outbound", nlohmann::json::array()}};
    capabilities["streaming"] = false;
    capabilities["credentials"] = true;
    capabilities["interactions"] = false;
    return capabilities;
}

// ---------------------------------------------------------------------------
// WecomRateLedger
// ---------------------------------------------------------------------------

std::int64_t WecomRateLedger::WaitMs(const std::string& conversation_id,
                                    std::int64_t now_ms) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sends_.find(conversation_id);
    if (it == sends_.end()) {
        return 0;
    }
    std::deque<std::int64_t>& sends = it->second;
    // 出窗清理(小时窗覆盖分钟窗)。
    while (!sends.empty() && sends.front() <= now_ms - kRateHourWindowMs) {
        sends.pop_front();
    }
    std::int64_t wait_ms = 0;
    const auto minute_boundary = now_ms - kRateMinuteWindowMs;
    std::size_t in_minute = 0;
    std::int64_t oldest_in_minute = 0;
    for (const std::int64_t at : sends) {
        if (at > minute_boundary) {
            if (in_minute == 0) {
                oldest_in_minute = at;
            }
            ++in_minute;
        }
    }
    if (in_minute >= limits_.per_minute) {
        // 分钟窗满:等最老一笔出窗(那一拍就腾出一个名额)。
        wait_ms = std::max<std::int64_t>(wait_ms, oldest_in_minute + kRateMinuteWindowMs - now_ms);
    }
    if (sends.size() >= limits_.per_hour) {
        wait_ms = std::max<std::int64_t>(wait_ms, sends.front() + kRateHourWindowMs - now_ms);
    }
    return wait_ms;
}

void WecomRateLedger::Record(const std::string& conversation_id, std::int64_t now_ms) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto& sends = sends_[conversation_id];
    sends.push_back(now_ms);
    while (!sends.empty() && sends.front() <= now_ms - kRateHourWindowMs) {
        sends.pop_front();
    }
}

// ---------------------------------------------------------------------------
// WecombotAdapter
// ---------------------------------------------------------------------------

WecombotAdapter::WecombotAdapter(Options options)
    : options_(std::move(options)), rate_(options_.rate_limits) {}

WecombotAdapter::~WecombotAdapter() {
    StopGatewayLocked("adapter destruct");
    if (gateway_thread_ != nullptr && gateway_thread_->joinable()) {
        gateway_thread_->join();
    }
    if (sender_thread_ != nullptr && sender_thread_->joinable()) {
        sender_thread_->join();
    }
}

std::size_t WecombotAdapter::spool_pending_count() const {
    return spool_.has_value() ? spool_->pending_count() : 0;
}

void WecombotAdapter::SetSpoolAppendFaultForTest(bool fail) {
    if (spool_.has_value()) {
        spool_->SetAppendFaultForTest(fail);
    }
}

void WecombotAdapter::WriteToSidecar(const std::byte* data, std::size_t size) {
    // 帧收发机械在共用件(SV-08):坏帧的 Fatal 通知、半帧等待、整帧分派
    // 全由 bridge_ 处置,这里只递业务分派口。
    bridge_.Feed(data, size,
                 [this](const nlohmann::json& frame) { HandleHostFrame(frame); });
}

std::vector<std::byte> WecombotAdapter::DrainFromSidecar() { return bridge_.Drain(); }

void WecombotAdapter::ReplyResult(std::int64_t id, const nlohmann::json& result) {
    bridge_.ReplyResult(id, result);
}

void WecombotAdapter::ReplyDomainError(std::int64_t id, DomainErrorName name,
                                       const std::string& detail) {
    bridge_.ReplyDomainError(id, name, detail);
}

void WecombotAdapter::EmitNotification(BridgeMethod method, const nlohmann::json& params) {
    bridge_.Notify(method, params);
}

std::string WecombotAdapter::NextDeliveryId() {
    std::ostringstream out;
    out << "wecom-del-" << delivery_counter_.fetch_add(1) << "-"
        << std::hex << options_.now_ms();
    return out.str();
}

void WecombotAdapter::RecordAnchor(const std::string& msgid, const std::string& req_id) {
    if (msgid.empty() || req_id.empty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(anchor_mutex_);
    if (anchors_.size() >= options_.max_req_id_anchors && !anchors_.contains(msgid)) {
        // 容量帽:按 FIFO 淘汰最老锚(24h 窗内高流量会话先丢最老——被淘
        // 锚的回复按"锚丢失"如实拒,不静默乱投)。
        while (!anchor_order_.empty() && anchors_.size() >= options_.max_req_id_anchors) {
            anchors_.erase(anchor_order_.front());
            anchor_order_.pop_front();
        }
    }
    const auto inserted = anchors_.emplace(msgid, req_id);
    if (inserted.second) {
        anchor_order_.push_back(msgid);
    } else {
        inserted.first->second = req_id;  // 平台重推同 msgid:锚以最新回调为准
    }
}

std::string WecombotAdapter::LookupAnchor(const std::string& msgid) {
    const std::lock_guard<std::mutex> lock(anchor_mutex_);
    const auto it = anchors_.find(msgid);
    return it == anchors_.end() ? std::string() : it->second;
}

void WecombotAdapter::HandleHostFrame(const nlohmann::json& frame_json) {
    const IncomingMessage message = ParseIncomingMessage(frame_json);
    if (message.kind == IncomingMessageKind::Malformed) {
        EmitNotification(BridgeMethod::Fatal,
                         nlohmann::json{{"reason", "invalid_frame"},
                                        {"detail", message.malformed_reason}});
        return;
    }
    if (message.kind == IncomingMessageKind::ResultResponse ||
        message.kind == IncomingMessageKind::ErrorResponse) {
        return;  // v1 协议 sidecar 不发 request,宿主不会回 response
    }
    if (!message.method.has_value()) {
        if (message.kind == IncomingMessageKind::Request && message.id.has_value()) {
            ReplyDomainError(*message.id, DomainErrorName::NotCapable,
                             "unknown method: " + message.method_name);
        }
        return;
    }
    if (message.kind == IncomingMessageKind::Notification) {
        return;  // channel.typing:企微长连接无此能力,W1 不宣称
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
                    "wecombot adapter speaks " + std::string(kBridgeHandshakeProtocolVersion) +
                        ", host asked " + protocol_version);
                return;
            }
            nlohmann::json result = nlohmann::json::object();
            result["protocol_version"] = std::string(kBridgeHandshakeProtocolVersion);
            result["adapter"] = {{"name", "wecombot-inproc"}, {"version", "1.0.0"}};
            result["capabilities"] = WecombotCapabilities();
            result["account_state_version"] = 1;
            ReplyResult(id, result);
            return;
        }
        case BridgeMethod::Start: {
            if (!StartGatewayLocked()) {
                ReplyDomainError(id, DomainErrorName::SpawnFailed, "spool store open failed");
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
            // W1 只做文本:text part 拼正文;file part 明拒(媒体归 W2)。
            PendingSend pending;
            pending.request_id = id;
            if (message.params.contains("conversation") &&
                message.params.at("conversation").is_object() &&
                message.params.at("conversation").contains("id") &&
                message.params.at("conversation").at("id").is_string()) {
                pending.conversation_id =
                    message.params.at("conversation").at("id").get<std::string>();
            }
            std::string content;
            bool has_file_part = false;
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
                    if (part.contains("type") &&
                        (part.at("type") == "file" || part.at("type") == "image" ||
                         part.at("type") == "audio" || part.at("type") == "video")) {
                        has_file_part = true;
                    }
                }
            }
            pending.content = std::move(content);
            if (message.params.contains("reply_to_message_id") &&
                message.params.at("reply_to_message_id").is_string()) {
                pending.anchor_msgid =
                    message.params.at("reply_to_message_id").get<std::string>();
            }
            if (message.params.contains("client_id") && message.params.at("client_id").is_string()) {
                pending.outbound_delivery_id =
                    message.params.at("client_id").get<std::string>();
            }
            if (pending.conversation_id.empty() || pending.content.empty()) {
                ReplyDomainError(id, DomainErrorName::NotCapable,
                                 "channel.send needs direct conversation id and text");
                return;
            }
            if (has_file_part) {
                ReplyDomainError(id, DomainErrorName::NotCapable,
                                 "wecombot W1 只支持文本回复,附件收发归 W2");
                return;
            }
            if (pending.anchor_msgid.empty()) {
                // 被动回复要透传回调 req_id(§六 6.5);无锚 = 主动推送
                // (aibot_send_msg)——W1 不做,明拒不冒充。
                ReplyDomainError(id, DomainErrorName::NotCapable,
                                 "wecombot W1 只做被动回复:channel.send 需要 "
                                 "reply_to_message_id(主动推送归 W2)");
                return;
            }
            if (pending.outbound_delivery_id.empty()) {
                pending.outbound_delivery_id = NextDeliveryId();
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
            const ConnectionSnapshot snapshot = ConnectionState();
            nlohmann::json result = nlohmann::json::object();
            result["state"] = session_ ? session_->state_name() : std::string("stopped");
            result["connected"] = snapshot.connected;
            result["thread_alive"] = snapshot.thread_alive;
            result["stage"] = snapshot.stage;
            result["cursor"] = nullptr;
            result["backlog"] = spool_pending_count();
            result["unsupported_events"] = unsupported_frame_count();
            result["ignored_events"] = ignored_event_count();
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
            ReplyDomainError(
                id, DomainErrorName::NotCapable,
                std::string("wecombot adapter does not implement ") +
                    BridgeMethodName(*message.method));
            return;
    }
}

bool WecombotAdapter::StartGatewayLocked() {
    if (gateway_thread_ != nullptr) {
        return true;  // 幂等
    }
    // spool:随 start 开(独立于 stop 关闭——重启重投在下轮连接里做)。
    if (!spool_.has_value()) {
        const auto spool_dir = options_.state_root / options_.channel_id / options_.account_id /
                               "spool" / "pending";
        if (auto spool = SpoolStore::Open(spool_dir); spool.has_value()) {
            spool_ = std::move(*spool);
        } else {
            return false;  // 账开不了:如实失败,不虚报 started
        }
        // 重启重投:历史 pending 全部重新上报(宿主 ingress 按 msgid 去重)。
        // 注意重投事件拿不到回调 req_id(锚不落盘)——对它们的回复按
        // "锚丢失"明拒;崩溃前已进模型的对话不受影响。
        for (const auto& [delivery_id, event_json] : spool_->ListPending()) {
            EmitNotification(BridgeMethod::Inbound, event_json);
        }
    }
    stop_.store(false);
    {
        const std::lock_guard<std::mutex> lock(connection_mutex_);
        connection_.connected = false;
        connection_.connected_since_ms = 0;
        connection_.stage = kStageConnecting;
        connection_.retry_count = 0;
        connection_.next_retry_at_ms = 0;
        connection_.updated_at_ms = options_.now_ms();
    }

    WecomGatewaySession::Options gateway_options;
    gateway_options.transport_factory = options_.transport_factory;
    gateway_options.endpoint = options_.endpoint;
    gateway_options.bot_id = options_.config.app_id;      // app_id 字段存 BotID
    gateway_options.secret = options_.credential.secret;  // 长连接专用 Secret
    gateway_options.on_event = [this](const WecomGatewayEvent& event) {
        HandleGatewayEvent(event);
    };
    gateway_options.now_ms = options_.now_ms;
    gateway_options.ping_interval_ms = options_.ping_interval_ms;
    gateway_options.subscribe_timeout_ms = options_.subscribe_timeout_ms;
    gateway_options.missed_ack_limit = options_.missed_ack_limit;
    gateway_options.max_backoff_ms = options_.max_backoff_ms;
    gateway_options.backoff_scale = options_.backoff_scale;
    gateway_options.drain_poll_ms = options_.drain_poll_ms;
    gateway_options.trust_load_block_code = options_.trust_load_block_code;
    gateway_options.trust_load_block_detail = options_.trust_load_block_detail;
    session_ = std::make_unique<WecomGatewaySession>(std::move(gateway_options));
    gateway_thread_ = std::make_unique<std::thread>([this]() { session_->RunLoop(&stop_); });
    if (sender_thread_ == nullptr) {
        sender_thread_ = std::make_unique<std::thread>([this]() { SenderLoop(); });
    }
    return true;
}

void WecombotAdapter::StopGatewayLocked(const std::string& reason) {
    (void)reason;
    // 停止旗先行:RunLoop 出口 FailAllPendings,发送线程的 SubmitFrame 即刻
    // 回 Stopped/失败,不再新起网络;WS 经 CancelInFlight 打断在途连接。
    stop_.store(true);
    if (session_ != nullptr) {
        session_->CancelInFlight();
    }
    if (gateway_thread_ != nullptr && gateway_thread_->joinable()) {
        gateway_thread_->join();
    }
    gateway_thread_.reset();
    // 发送线程先收口再清 session_(它还要读指针递交;stop 后只排空回错)。
    sender_wake_.notify_all();
    if (sender_thread_ != nullptr && sender_thread_->joinable()) {
        sender_thread_->join();
    }
    sender_thread_.reset();
    session_.reset();
}

void WecombotAdapter::HandleGatewayEvent(const WecomGatewayEvent& event) {
    switch (event.kind) {
        case WecomGatewayEvent::Kind::MessageCallback: {
            if (!spool_.has_value()) {
                // 没开账(防御,理论不可达):Fatal 留痕,事件照报(spool 缺
                // 席只影响崩溃重放,内存路不必陪葬)。
                EmitNotification(BridgeMethod::Fatal,
                                 nlohmann::json{{"reason", "spool_write_failed"},
                                                {"detail", "spool not open"}});
            }
            const std::string delivery_id = NextDeliveryId();
            std::string map_error;
            const auto mapping = MapMsgCallback(event.body, options_.channel_id,
                                                 options_.account_id, options_.config.app_id,
                                                 delivery_id, options_.now_ms(), &map_error);
            if (!mapping.has_value()) {
                // 事件解不开:不进 spool 不上报,Fatal 留痕(判无效 = 明确
                // 终结,平台重推同 msgid 只会再判一次无效)。
                EmitNotification(BridgeMethod::Fatal,
                                 nlohmann::json{{"reason", "invalid_frame"},
                                                {"detail", "wecom msg map: " + map_error}});
                return;
            }
            // 回话锚:respond 须透传本回调的 req_id(§六 6.5)。
            RecordAnchor(mapping->event.message_id, event.req_id);
            const nlohmann::json event_json = mapping->event.ToJson();
            if (spool_.has_value()) {
                if (const auto spool_error = spool_->AppendPending(delivery_id, event_json)) {
                    // 落盘失败(磁盘满/权限拒):QQ 靠断线+Resume 补发,企微
                    // 无补发路——断线拿不回这条。Fatal 留痕(宿主 Degraded
                    // 可见),照常上报:崩溃重放丢了,内存处理不丢。
                    EmitNotification(BridgeMethod::Fatal,
                                     nlohmann::json{{"reason", "spool_write_failed"},
                                                    {"detail", *spool_error}});
                }
            }
            EmitNotification(BridgeMethod::Inbound, event_json);
            return;
        }
        case WecomGatewayEvent::Kind::EventCallback: {
            // §六 6.4:enter_chat/卡片/feedback 只记日志不入模型;disconnected
            // 网关已处置(让位)。
            ignored_event_count_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        case WecomGatewayEvent::Kind::UnsupportedFrame: {
            unsupported_frame_count_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        case WecomGatewayEvent::Kind::SessionReady: {
            {
                const std::lock_guard<std::mutex> lock(connection_mutex_);
                connection_.connected = true;
                connection_.connected_since_ms = options_.now_ms();
                connection_.stage = kStageConnected;
                if (connection_.last_failure.has_value()) {
                    connection_.failure_history.push_back(*connection_.last_failure);
                    if (connection_.failure_history.size() > 8) {
                        connection_.failure_history.erase(connection_.failure_history.begin());
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
        case WecomGatewayEvent::Kind::StageChanged: {
            const std::lock_guard<std::mutex> lock(connection_mutex_);
            connection_.stage = event.stage.empty() ? kStageConnecting : event.stage;
            connection_.updated_at_ms = options_.now_ms();
            return;
        }
        case WecomGatewayEvent::Kind::ConnectFailed:
        case WecomGatewayEvent::Kind::Disconnected: {
            {
                const std::lock_guard<std::mutex> lock(connection_mutex_);
                connection_.connected = false;
                connection_.connected_since_ms = 0;
                connection_.last_failure = ConnectionFailure{
                    event.stage, event.error_code, event.detail, options_.now_ms(),
                    event.attempt};
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
        case WecomGatewayEvent::Kind::BackoffScheduled: {
            const std::lock_guard<std::mutex> lock(connection_mutex_);
            connection_.retry_count = event.attempt;
            connection_.next_retry_at_ms = event.next_retry_at_ms;
            connection_.updated_at_ms = options_.now_ms();
            return;
        }
        case WecomGatewayEvent::Kind::Stopped: {
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

ConnectionSnapshot WecombotAdapter::ConnectionState() const {
    const std::lock_guard<std::mutex> lock(connection_mutex_);
    ConnectionSnapshot snapshot = connection_;
    snapshot.thread_alive = gateway_thread_ != nullptr;
    if (snapshot.stage.empty()) {
        snapshot.stage = snapshot.thread_alive ? kStageConnecting : std::string("idle");
    }
    return snapshot;
}

bool WecombotAdapter::SleepSendInterruptible(std::int64_t ms) {
    const std::int64_t step = 50;
    for (std::int64_t slept = 0; slept < ms; slept += step) {
        if (stop_.load()) {
            return false;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<long long>(
                std::min<std::int64_t>(step, ms - slept))));
    }
    return !stop_.load();
}

void WecombotAdapter::SenderLoop() {
    while (true) {
        std::unique_lock<std::mutex> lock(send_mutex_);
        sender_wake_.wait(lock, [this]() { return !send_queue_.empty() || stop_.load(); });
        if (stop_.load()) {
            // 停止收口:队列整批排空——未发的如实回 domain 错,不冒充送达。
            std::vector<PendingSend> sends = std::move(send_queue_);
            send_queue_.clear();
            lock.unlock();
            for (const PendingSend& pending : sends) {
                ReplyDomainError(pending.request_id, DomainErrorName::TransportFailed,
                                 "adapter stopped before send");
            }
            return;
        }
        PendingSend pending = std::move(send_queue_.front());
        send_queue_.erase(send_queue_.begin());
        lock.unlock();
        DeliverPending(std::move(pending));
    }
}

void WecombotAdapter::DeliverPending(PendingSend pending) {
    // 1) 回话锚:respond 透传回调 req_id(§六 6.5)。锚不在(重启丢锚/
    //    容量帽淘汰/24h 超窗后平台自会拒)按永久拒绝如实报。
    const std::string req_id = LookupAnchor(pending.anchor_msgid);
    if (req_id.empty()) {
        ReplyDomainError(pending.request_id, DomainErrorName::PermanentReject,
                         "no req_id anchor for msgid " + pending.anchor_msgid +
                             "(重启后丢锚或锚被淘汰;主动推送归 W2)");
        return;
    }
    // 2) 分段(markdown ≤20480 字节 UTF-8,码点边界切)。
    const auto chunks = SplitUtf8Chunks(pending.content, kWecomMarkdownMaxBytes);
    while (pending.chunk_cursor < chunks.size()) {
        // 3) 限流:单会话 30/min、1000/hour(回复+推送合计口径),超窗排队。
        std::int64_t rate_wait = rate_.WaitMs(pending.conversation_id, options_.now_ms());
        while (rate_wait > 0) {
            if (!SleepSendInterruptible(std::min<std::int64_t>(rate_wait, 1'000))) {
                ReplyDomainError(pending.request_id, DomainErrorName::TransportFailed,
                                 "adapter stopped before send");
                return;
            }
            rate_wait = rate_.WaitMs(pending.conversation_id, options_.now_ms());
        }
        // 4) 递交:入队网关线程写侧,按 req_id 等平台回执。
        const auto outcome =
            session_->SubmitFrame(BuildRespondFrame(req_id, chunks[pending.chunk_cursor]),
                                  req_id, options_.respond_ack_timeout_ms);
        if (outcome.status == WecomSubmitOutcome::Status::Acked) {
            if (outcome.errcode == 0) {
                rate_.Record(pending.conversation_id, options_.now_ms());
                ++pending.chunk_cursor;
                continue;
            }
            if (ClassifyRespondErrcode(outcome.errcode) == WecomRespondStatus::RateLimited) {
                // 平台限流:重试(同 req_id 重发官方容忍)。
            } else {
                // 窗口过期/内容拒绝/权限:errcode 数值如实透传(errmsg 不带
                // ——平台文案可能回显敏感值)。
                ReplyDomainError(pending.request_id, DomainErrorName::PermanentReject,
                                 "respond rejected, errcode=" +
                                     std::to_string(outcome.errcode));
                return;
            }
        } else if (outcome.status == WecomSubmitOutcome::Status::Stopped) {
            ReplyDomainError(pending.request_id, DomainErrorName::TransportFailed,
                             "adapter stopped before send");
            return;
        }
        // Timeout/SendFailed/NotConnected/限流回执:可重试(退避后整段再来,
        // 已送段不重发)。
        ++pending.attempts;
        if (pending.attempts > kMaxSendAttempts) {
            const bool rate_class =
                outcome.status == WecomSubmitOutcome::Status::Acked;  // 限流回执耗尽
            ReplyDomainError(
                pending.request_id,
                rate_class ? DomainErrorName::RateLimited : DomainErrorName::TransportFailed,
                "deferred after retries: " +
                    (outcome.detail.empty() ? "no stable connection" : outcome.detail));
            return;
        }
        if (!SleepSendInterruptible(
                static_cast<std::int64_t>(options_.sender_backoff_base_ms)
                << (pending.attempts - 1))) {
            ReplyDomainError(pending.request_id, DomainErrorName::TransportFailed,
                             "adapter stopped before send");
            return;
        }
    }
    ReplyResult(pending.request_id,
                nlohmann::json{{"provider_message_id", pending.outbound_delivery_id},
                               {"accepted", true}});
}

}  // namespace lubancode::channel::wecombot
