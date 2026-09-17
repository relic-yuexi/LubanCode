// wecom_gateway.hpp 的实现(见头注:写侧串行化与两处刻意差异的账)。
#include "channel/wecombot/wecom_gateway.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

namespace lubancode::channel::wecombot {

namespace {

// 退避阶梯(照 QqGatewaySession/configuration.md §10):1s..60s + 10% jitter,
// 在线稳定过归零。
constexpr int kBackoffSeconds[] = {1, 2, 4, 8, 16, 30, 60};
constexpr int kBackoffSteps = static_cast<int>(sizeof(kBackoffSeconds) / sizeof(int));

// 真 transport::WsClient 的连接错误 → 稳定码(照 qq_gateway 的映射;
// TCP/TLS/WS 升级失败在 transport::WsClient::Connect 一口锅)。
std::string WsErrorConnectCode(const channel::transport::WsError& error) {
    if (!error.error_code.empty()) {
        return error.error_code;
    }
    switch (error.kind) {
        case channel::transport::WsError::Kind::Timeout:
            return "connect_timeout";
        case channel::transport::WsError::Kind::Protocol:
            return "ws_handshake_failed";
        case channel::transport::WsError::Kind::Closed:
            return error.detail == "connect cancelled" ? std::string("connect_cancelled")
                                                       : std::string("ws_handshake_closed");
        case channel::transport::WsError::Kind::Failed:
            return error.detail.rfind("tls: ", 0) == 0
                       ? std::string(channel::transport::kTlsCodeHandshakeFailed)
                       : std::string("connect_failed");
    }
    return "connect_failed";
}

// 运行期读错误 → 稳定码。
std::string ReadErrorCode(const channel::transport::WsError& error) {
    switch (error.kind) {
        case channel::transport::WsError::Kind::Timeout:
            return "read_timeout";
        case channel::transport::WsError::Kind::Closed:
            return "read_closed";
        case channel::transport::WsError::Kind::Protocol:
            return "read_protocol";
        case channel::transport::WsError::Kind::Failed:
            break;
    }
    return "read_failed";
}

WecomGatewayEvent MakeConnectEvent(WecomGatewayEvent::Kind kind, const std::string& stage,
                                   const std::string& code, const std::string& detail) {
    WecomGatewayEvent event;
    event.kind = kind;
    event.stage = stage;
    event.error_code = code;
    event.detail = detail;
    return event;
}

}  // namespace

WecomGatewaySession::~WecomGatewaySession() {
    CancelInFlight();
}

std::string WecomGatewaySession::state_name() const {
    switch (state_.load()) {
        case State::Idle:
            return "idle";
        case State::Connecting:
            return "connecting";
        case State::Subscribing:
            return "subscribing";
        case State::Running:
            return "running";
        case State::Backoff:
            return "backoff";
        case State::Stopped:
            return "stopped";
    }
    return "unknown";
}

void WecomGatewaySession::CancelInFlight() {
    // 共享所有权(照 QqGatewaySession):锁内拷 shared_ptr,锁外调 Cancel。
    std::shared_ptr<WecomTransport> transport;
    {
        const std::lock_guard<std::mutex> lock(in_flight_mutex_);
        transport = in_flight_;
    }
    if (transport != nullptr) {
        transport->Cancel();
    }
}

void WecomGatewaySession::EmitEvent(const WecomGatewayEvent& event) {
    if (options_.on_event) {
        options_.on_event(event);
    }
}

std::string WecomGatewaySession::NextReqId() {
    // 进程内唯一即可:序号单调 + 时间戳;不承载语义,不进日志敏感面。
    std::ostringstream out;
    out << "wecomreq-" << std::hex << (options_.now_ms ? options_.now_ms() : 0) << "-"
        << req_id_sequence_.fetch_add(1);
    return out.str();
}

void WecomGatewaySession::SleepInterruptible(std::atomic<bool>* stop, std::int64_t ms) {
    const std::int64_t step = 100;
    for (std::int64_t slept = 0; slept < ms && !stop->load(); slept += step) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<long long>(std::min<std::int64_t>(step, ms - slept))));
    }
}

WecomSubmitOutcome WecomGatewaySession::SubmitFrame(const std::string& frame_text,
                                                    const std::string& req_id,
                                                    int timeout_ms) {
    if (loop_stopped_.load()) {
        return WecomSubmitOutcome{WecomSubmitOutcome::Status::Stopped, 0, "",
                                  "gateway loop stopped"};
    }
    auto receipt = std::make_shared<WecomPendingReceipt>();
    {
        const std::lock_guard<std::mutex> lock(pending_mutex_);
        // 同 req_id 重递交(发送线程重试):新账覆盖旧账,旧 receipt 自然
        // 超时作废——排水侧按 receipt 自身 done 标跳过已作废条目。
        pendings_[req_id] = receipt;
    }
    {
        const std::lock_guard<std::mutex> lock(write_mutex_);
        write_queue_.push_back(WriteEntry{frame_text, receipt});
    }
    std::unique_lock<std::mutex> lock(receipt->mutex);
    const bool done = receipt->cv.wait_for(
        lock, std::chrono::milliseconds(timeout_ms), [&receipt]() { return receipt->done.load(); });
    WecomSubmitOutcome outcome = receipt->outcome;
    lock.unlock();
    // 超时收尾:账上摘除(平台稍后迟到回执找不到账,按 unexpected 计数)。
    if (!done) {
        outcome.status = WecomSubmitOutcome::Status::Timeout;
        outcome.detail = "no ack within " + std::to_string(timeout_ms) + "ms";
        receipt->Complete(outcome);
        const std::lock_guard<std::mutex> erase_lock(pending_mutex_);
        const auto it = pendings_.find(req_id);
        if (it != pendings_.end() && it->second == receipt) {
            pendings_.erase(it);
        }
    }
    return outcome;
}

bool WecomGatewaySession::CompletePending(const std::string& req_id, std::int64_t errcode,
                                          const std::string& errmsg) {
    std::shared_ptr<WecomPendingReceipt> receipt;
    {
        const std::lock_guard<std::mutex> lock(pending_mutex_);
        const auto it = pendings_.find(req_id);
        if (it == pendings_.end()) {
            return false;
        }
        receipt = it->second;
        pendings_.erase(it);
    }
    WecomSubmitOutcome outcome;
    outcome.status = WecomSubmitOutcome::Status::Acked;
    outcome.errcode = errcode;
    outcome.errmsg = errmsg;
    receipt->Complete(std::move(outcome));
    return true;
}

void WecomGatewaySession::FailAllPendings(WecomSubmitOutcome::Status status,
                                          const std::string& detail) {
    std::vector<std::shared_ptr<WecomPendingReceipt>> receipts;
    {
        const std::lock_guard<std::mutex> lock(pending_mutex_);
        receipts.reserve(pendings_.size());
        for (auto& [id, receipt] : pendings_) {
            receipts.push_back(receipt);
        }
        pendings_.clear();
    }
    // 出站队列里的同批条目一并作废(IsDone 跳过写)。
    {
        const std::lock_guard<std::mutex> lock(write_mutex_);
        for (auto& entry : write_queue_) {
            receipts.push_back(entry.receipt);
        }
        write_queue_.clear();
    }
    for (auto& receipt : receipts) {
        WecomSubmitOutcome outcome;
        outcome.status = status;
        outcome.detail = detail;
        receipt->Complete(std::move(outcome));
    }
}

std::optional<std::string> WecomGatewaySession::DrainWrites(
    const std::shared_ptr<WecomTransport>& transport) {
    std::vector<WriteEntry> batch;
    {
        const std::lock_guard<std::mutex> lock(write_mutex_);
        batch.swap(write_queue_);
    }
    for (WriteEntry& entry : batch) {
        if (entry.receipt->IsDone()) {
            continue;  // 发送方已超时/已被全局失败:不再写(重复投递无意义)
        }
        const auto sent = transport->SendText(entry.frame);
        if (!sent.has_value()) {
            WecomSubmitOutcome outcome;
            outcome.status = WecomSubmitOutcome::Status::SendFailed;
            outcome.detail = "ws send: " + sent.error();
            entry.receipt->Complete(std::move(outcome));
            return outcome.detail;
        }
    }
    return std::nullopt;
}

void WecomGatewaySession::RunLoop(std::atomic<bool>* stop) {
    loop_stopped_.store(false);
    int attempt = 0;
    while (!stop->load()) {
        state_.store(State::Connecting);
        const RunOutcome outcome = RunOneConnection(stop, attempt + 1);
        if (outcome.fatal) {
            break;  // 凭据错/被新连顶替:退避重试无意义(换密钥/让位)
        }
        if (stop->load()) {
            break;
        }
        attempt = outcome.stable ? 0 : std::min(attempt + 1, kBackoffSteps);
        connect_attempts_.store(attempt);
        const int base_ms = kBackoffSeconds[attempt == 0 ? 0 : attempt - 1] * 1000;
        std::mt19937 rng(static_cast<unsigned>(
            (options_.now_ms ? options_.now_ms() : 0) & 0xFFFFFFFF));
        std::uniform_int_distribution<int> jitter(0, base_ms / 10);  // 10% jitter
        int backoff_ms = static_cast<int>((base_ms + jitter(rng)) * options_.backoff_scale);
        if (backoff_ms > options_.max_backoff_ms) {
            backoff_ms = options_.max_backoff_ms;
        }
        state_.store(State::Backoff);
        WecomGatewayEvent backoff;
        backoff.kind = WecomGatewayEvent::Kind::BackoffScheduled;
        backoff.attempt = attempt;
        backoff.next_retry_at_ms = (options_.now_ms ? options_.now_ms() : 0) + backoff_ms;
        EmitEvent(backoff);
        SleepInterruptible(stop, backoff_ms);
    }
    state_.store(State::Stopped);
    loop_stopped_.store(true);
    connection_active_.store(false);
    FailAllPendings(WecomSubmitOutcome::Status::Stopped, "gateway loop stopped");
    EmitEvent(WecomGatewayEvent{WecomGatewayEvent::Kind::Stopped});
}

WecomGatewaySession::RunOutcome WecomGatewaySession::RunOneConnection(std::atomic<bool>* stop,
                                                                      int attempt_number) {
    std::shared_ptr<WecomTransport> transport =
        options_.transport_factory ? options_.transport_factory() : nullptr;
    if (transport == nullptr) {
        WecomGatewayEvent event = MakeConnectEvent(WecomGatewayEvent::Kind::ConnectFailed,
                                                   kStageConnecting, "no_transport",
                                                   "no transport factory");
        event.attempt = attempt_number;
        EmitEvent(event);
        return RunOutcome{};
    }
    {
        const std::lock_guard<std::mutex> lock(in_flight_mutex_);
        in_flight_ = transport;
    }
    struct InFlightGuard {
        WecomGatewaySession* session;
        ~InFlightGuard() {
            const std::lock_guard<std::mutex> lock(session->in_flight_mutex_);
            session->in_flight_.reset();
        }
    } in_flight_guard{this};
    connection_active_.store(false);

    // 一轮失败的收口:kind 显式给(没订阅成就断 = ConnectFailed;订阅成
    // 过后断 = Disconnected),都带阶段/稳定码/根因与尝试编号。在途递交
    // 一并快速失败(NotConnected)——连接死了,发送线程不必干等回执超时,
    // 退避后用同一 req_id 重递交(官方容忍)。
    const auto fail = [&](WecomGatewayEvent::Kind kind, const std::string& stage,
                          const std::string& code, const std::string& reason) {
        connection_active_.store(false);
        transport->Close(1000, "session end");
        FailAllPendings(WecomSubmitOutcome::Status::NotConnected,
                        "connection lost: " + code);
        WecomGatewayEvent event = MakeConnectEvent(kind, stage, code, reason);
        event.attempt = attempt_number;
        EmitEvent(event);
    };

    // 装配预检的信任根失败:短路联网(同 QQ §四——本地 TLS 不可用时连接
    // 注定无效);非致命,退避,重启/配置变化后重新装配才会再加载。
    if (!options_.trust_load_block_code.empty()) {
        fail(WecomGatewayEvent::Kind::ConnectFailed, kStageConnecting,
             options_.trust_load_block_code,
             options_.trust_load_block_detail.empty()
                 ? std::string("信任根加载失败(装配预检),已阻断联网重试")
                 : options_.trust_load_block_detail);
        return RunOutcome{};
    }

    EmitEvent(MakeConnectEvent(WecomGatewayEvent::Kind::StageChanged, kStageConnecting,
                               std::string(), std::string()));
    const auto connected = transport->Connect(options_.endpoint);
    if (!connected.has_value()) {
        fail(WecomGatewayEvent::Kind::ConnectFailed, kStageConnecting,
             connected.error().error_code.empty()
                 ? WsErrorConnectCode(connected.error())
                 : connected.error().error_code,
             "ws connect: " + connected.error().detail);
        return RunOutcome{};
    }

    // 1) subscribe:连上即发、勿重发(平台频率保护);等 errcode 回执。
    state_.store(State::Subscribing);
    EmitEvent(MakeConnectEvent(WecomGatewayEvent::Kind::StageChanged, kStageSubscribing,
                               std::string(), std::string()));
    const std::string subscribe_req_id = NextReqId();
    const auto subscribed =
        transport->SendText(BuildSubscribeFrame(options_.bot_id, options_.secret,
                                                subscribe_req_id));
    if (!subscribed.has_value()) {
        fail(WecomGatewayEvent::Kind::ConnectFailed, kStageSubscribing,
             "subscribe_send_failed", "send subscribe: " + subscribed.error());
        return RunOutcome{};
    }
    std::optional<std::int64_t> subscribe_errcode;
    {
        const std::int64_t deadline_ms =
            (options_.now_ms ? options_.now_ms() : 0) + options_.subscribe_timeout_ms;
        while (!stop->load()) {
            const std::int64_t now = options_.now_ms ? options_.now_ms() : 0;
            if (now >= deadline_ms) {
                break;
            }
            const auto message = transport->ReadMessage(
                static_cast<int>(std::min<std::int64_t>(deadline_ms - now, 1'000)));
            if (!message.has_value()) {
                if (message.error().kind == channel::transport::WsError::Kind::Timeout) {
                    continue;
                }
                fail(WecomGatewayEvent::Kind::ConnectFailed, kStageSubscribing,
                     ReadErrorCode(message.error()), "subscribe read: " + message.error().detail);
                return RunOutcome{};
            }
            const auto payload_json =
                nlohmann::json::parse(*message, nullptr, /*allow_exceptions=*/false);
            if (payload_json.is_discarded()) {
                fail(WecomGatewayEvent::Kind::ConnectFailed, kStageSubscribing,
                     "subscribe_bad_payload", "subscribe response not json");
                return RunOutcome{};
            }
            std::string parse_error;
            const auto frame = ParseWecomInboundFrame(payload_json, &parse_error);
            if (!frame.has_value()) {
                fail(WecomGatewayEvent::Kind::ConnectFailed, kStageSubscribing,
                     "subscribe_bad_payload", "subscribe response: " + parse_error);
                return RunOutcome{};
            }
            if (frame->kind == WecomFrameKind::Ack) {
                if (!frame->req_id.empty() && frame->req_id != subscribe_req_id) {
                    // 别家的回执(理论上订阅窗内不该有):记账不崩,继续等。
                    unexpected_frames_.fetch_add(1);
                    continue;
                }
                subscribe_errcode = frame->errcode;
                break;
            }
            if (frame->kind == WecomFrameKind::MsgCallback ||
                frame->kind == WecomFrameKind::EventCallback) {
                // 早到回调:订阅窗内照常转交(平台没承诺次序,不丢信)。
                WecomGatewayEvent event;
                event.kind = frame->kind == WecomFrameKind::MsgCallback
                                 ? WecomGatewayEvent::Kind::MessageCallback
                                 : WecomGatewayEvent::Kind::EventCallback;
                event.body = frame->body;
                event.req_id = frame->req_id;
                EmitEvent(event);
                continue;
            }
            unexpected_frames_.fetch_add(1);
        }
    }
    if (stop->load()) {
        transport->Close(1000, "stop");
        return RunOutcome{};
    }
    if (!subscribe_errcode.has_value()) {
        fail(WecomGatewayEvent::Kind::ConnectFailed, kStageSubscribing, "subscribe_timeout",
             "no subscribe response within window");
        return RunOutcome{};
    }
    if (*subscribe_errcode != 0) {
        const bool credential =
            ClassifySubscribeErrcode(*subscribe_errcode) == WecomSubscribeStatus::CredentialRejected;
        // 平台 errmsg 不透传(可能回显敏感值);errcode 数值如实进 detail。
        fail(WecomGatewayEvent::Kind::ConnectFailed, kStageSubscribing,
             credential ? "subscribe_invalid_credentials" : "subscribe_rejected",
             "subscribe rejected, errcode=" + std::to_string(*subscribe_errcode));
        return RunOutcome{/*stable=*/false, /*fatal=*/credential};
    }

    // 2) 订阅过:连接可用。
    connection_active_.store(true);
    EmitEvent(WecomGatewayEvent{WecomGatewayEvent::Kind::SessionReady});
    EmitEvent(MakeConnectEvent(WecomGatewayEvent::Kind::StageChanged, kStageConnected,
                               std::string(), std::string()));

    // 3) 读循环:心跳节拍 + 出站排水 + 入站分发。任一入站帧(回执/回调)
    //    都证明连接活着——missed 账清零;死线判据是"拍内无任何入站 +
    //    心跳无人应答"。心跳窗内无帧也持续排水(发送线程不等心跳)。
    state_.store(State::Running);
    int missed_acks = 0;
    bool ever_acked = false;
    std::int64_t next_ping_ms =
        (options_.now_ms ? options_.now_ms() : 0) + options_.ping_interval_ms;
    std::string last_ping_req_id;
    while (!stop->load()) {
        const std::int64_t now = options_.now_ms ? options_.now_ms() : 0;
        std::int64_t remaining_ping = next_ping_ms - now;
        if (remaining_ping <= 0) {
            last_ping_req_id = NextReqId();
            const auto pinged = transport->SendText(BuildPingFrame(last_ping_req_id));
            if (!pinged.has_value()) {
                fail(WecomGatewayEvent::Kind::Disconnected, kStageConnected,
                     "ping_send_failed", "send ping: " + pinged.error());
                return RunOutcome{ever_acked, /*fatal=*/false};
            }
            ++missed_acks;
            if (missed_acks > options_.missed_ack_limit) {
                fail(WecomGatewayEvent::Kind::Disconnected, kStageConnected,
                     "heartbeat_ack_missed",
                     "no inbound frame for " + std::to_string(missed_acks) + " ping intervals");
                return RunOutcome{ever_acked, /*fatal=*/false};
            }
            next_ping_ms = now + options_.ping_interval_ms;
            remaining_ping = options_.ping_interval_ms;
        }
        if (const auto write_error = DrainWrites(transport)) {
            fail(WecomGatewayEvent::Kind::Disconnected, kStageConnected, "write_failed",
                 *write_error);
            return RunOutcome{ever_acked, /*fatal=*/false};
        }
        const int read_budget = static_cast<int>(std::max<std::int64_t>(
            1, std::min<std::int64_t>(remaining_ping, options_.drain_poll_ms)));
        const auto message = transport->ReadMessage(read_budget);
        if (!message.has_value()) {
            if (message.error().kind == channel::transport::WsError::Kind::Timeout) {
                continue;  // 到点:回循环顶(心跳判定/排水)
            }
            fail(WecomGatewayEvent::Kind::Disconnected, kStageConnected,
                 ReadErrorCode(message.error()), "read: " + message.error().detail);
            return RunOutcome{ever_acked, /*fatal=*/false};
        }
        const auto payload_json =
            nlohmann::json::parse(*message, nullptr, /*allow_exceptions=*/false);
        if (payload_json.is_discarded()) {
            fail(WecomGatewayEvent::Kind::Disconnected, kStageConnected, "frame_bad_payload",
                 "payload not json");
            return RunOutcome{ever_acked, /*fatal=*/false};
        }
        std::string parse_error;
        const auto frame = ParseWecomInboundFrame(payload_json, &parse_error);
        if (!frame.has_value()) {
            fail(WecomGatewayEvent::Kind::Disconnected, kStageConnected, "frame_bad_payload",
                 "frame parse: " + parse_error);
            return RunOutcome{ever_acked, /*fatal=*/false};
        }
        // 任何入站帧都证明连接活着(回执与回调同一条 TCP)。
        missed_acks = 0;
        switch (frame->kind) {
            case WecomFrameKind::Ack: {
                ever_acked = true;
                if (!frame->req_id.empty() && frame->req_id == last_ping_req_id) {
                    break;  // 本拍心跳的回执
                }
                if (!frame->req_id.empty() &&
                    CompletePending(frame->req_id, *frame->errcode, frame->errmsg)) {
                    break;  // respond 回执:递交账完成
                }
                unexpected_frames_.fetch_add(1);
                break;
            }
            case WecomFrameKind::MsgCallback: {
                WecomGatewayEvent event;
                event.kind = WecomGatewayEvent::Kind::MessageCallback;
                event.body = frame->body;
                event.req_id = frame->req_id;
                EmitEvent(event);
                break;
            }
            case WecomFrameKind::EventCallback: {
                // disconnected_event:新连顶了旧连——断开并让位(重连会
                // 反踢新连,两头打乒乓;账号锁应拦住本机重复装配,跨机
                // 抢线如实报给用户裁决)。
                std::string event_parse_error;
                const auto info = ParseEventCallback(frame->body, &event_parse_error);
                if (info.has_value() && info->eventtype == kWecomEventDisconnected) {
                    fail(WecomGatewayEvent::Kind::Disconnected, kStageConnected,
                         "displaced_by_new_connection",
                         "server opened a newer connection for this bot");
                    return RunOutcome{ever_acked, /*fatal=*/true};
                }
                WecomGatewayEvent event;
                event.kind = WecomGatewayEvent::Kind::EventCallback;
                event.body = frame->body;
                event.req_id = frame->req_id;
                EmitEvent(event);
                break;
            }
            case WecomFrameKind::Unknown: {
                unexpected_frames_.fetch_add(1);
                WecomGatewayEvent event;
                event.kind = WecomGatewayEvent::Kind::UnsupportedFrame;
                event.detail = frame->cmd.empty() ? "frame without cmd" : "cmd: " + frame->cmd;
                EmitEvent(event);
                break;
            }
        }
    }
    // stop 置位:干净收场(在线稳定结束,不涨退避)。
    connection_active_.store(false);
    transport->Close(1000, "stop");
    return RunOutcome{ever_acked, /*fatal=*/false};
}

}  // namespace lubancode::channel::wecombot
