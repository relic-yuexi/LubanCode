#include "channel/manager.hpp"

#include <algorithm>
#include <chrono>
#include <random>

#include "channel/bridge_protocol.hpp"
#include "channel/digest.hpp"

#include "platform/paths.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace lubancode::channel {

namespace {

std::int64_t DefaultNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::filesystem::path AccountDir(const std::filesystem::path& state_root,
                                 const std::string& channel_id, const std::string& account_id) {
    return state_root / channel_id / account_id;
}

std::filesystem::path LockFile(const std::filesystem::path& state_root,
                               const std::string& channel_id, const std::string& account_id) {
    return state_root / "locks" / (channel_id + "-" + account_id + ".lock");
}

constexpr std::size_t kMaxRecentTransitions = 8;

unsigned long CurrentPid() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// 实例令牌:每只 ChannelManager 一枚,进锁账。同进程两只 manager 互不
// 相认(账号锁只认"同一实例重入续锁",见 account_lock.cpp)。
std::string NewInstanceToken() {
    std::random_device device;
    const std::string seed = std::to_string(CurrentPid()) + ":" + std::to_string(device()) +
                             std::to_string(device()) + ":" + std::to_string(DefaultNowMs());
    return Sha256Hex(seed);
}

// PairingStore -> PairingAdmission 的适配器(router 是纯函数件,不持账;
// 账的归属仍在 manager 的 AccountEntry)。
class StorePairingAdmission : public PairingAdmission {
public:
    explicit StorePairingAdmission(PairingStore& store) : store_(store) {}

    bool IsSenderApproved(const std::string& sender_id) const override {
        return store_.IsSenderApproved(sender_id);
    }

    std::optional<std::string> RequestCode(const std::string& sender_id,
                                           std::int64_t now_ms) override {
        return store_.RequestPairing(sender_id, now_ms);
    }

private:
    PairingStore& store_;
};

}  // namespace

ChannelManager::ChannelManager(ChannelManagerOptions options)
    : options_(std::move(options)), instance_token_(NewInstanceToken()) {
    if (!options_.now_ms) {
        options_.now_ms = [] { return DefaultNowMs(); };
    }
    if (!options_.alive_checker) {
        options_.alive_checker = AccountLock::DefaultAliveChecker();
    }
}

ChannelManager::~ChannelManager() {
    // 关闭顺序(bridge-protocol.md §7 的同步版):停收新 turn -> channel.stop
    // -> 关 stdin(同步模型无柄) -> 释放账号锁。析构不抛,失败只留账。
    std::vector<std::pair<std::string, std::string>> to_stop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& account : accounts_) {
            if (account->state != ChannelAccountState::Stopped &&
                account->state != ChannelAccountState::Disabled &&
                !IsUnrecoverableAccountState(account->state)) {
                to_stop.emplace_back(account->channel_id, account->account_id);
            }
        }
    }
    for (const auto& [channel_id, account_id] : to_stop) {
        StopAccount(channel_id, account_id);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& account : accounts_) {
            if (options_.wake != nullptr) {
                options_.wake->ClearAccountWakeSource(account->channel_id, account->account_id);
            }
        }
        accounts_.clear();
    }
}

ChannelManager::AccountEntry* ChannelManager::Find(const std::string& channel_id,
                                                   const std::string& account_id) {
    for (auto& account : accounts_) {
        if (account->channel_id == channel_id && account->account_id == account_id) {
            return account.get();
        }
    }
    return nullptr;
}

const ChannelManager::AccountEntry* ChannelManager::Find(const std::string& channel_id,
                                                         const std::string& account_id) const {
    for (const auto& account : accounts_) {
        if (account->channel_id == channel_id && account->account_id == account_id) {
            return account.get();
        }
    }
    return nullptr;
}

std::optional<std::string> ChannelManager::TransitionLocked(AccountEntry& entry,
                                                            ChannelAccountState to,
                                                            const std::string& reason,
                                                            const std::string& detail) {
    if (!CanTransition(entry.state, to)) {
        return std::string("非法状态迁移 ") + ChannelAccountStateName(entry.state) + " -> " +
               ChannelAccountStateName(to) + " (" + reason + ")";
    }
    AccountStatusTransition transition;
    transition.channel_id = entry.channel_id;
    transition.account_id = entry.account_id;
    transition.timestamp_ms = options_.now_ms();
    transition.from = entry.state;
    transition.to = to;
    transition.reason = reason;
    transition.detail = detail;
    transition.generation = entry.generation;
    if (ShouldAutoRetry(reason) || (!reason.empty() && to == ChannelAccountState::Backoff)) {
        transition.retry_at_ms =
            options_.now_ms() + BackoffDelayMs(entry.backoff_attempt, /*jitter=*/0.0);
        entry.retry_at_ms = transition.retry_at_ms;
        entry.backoff_attempt += 1;
    }
    if (to == ChannelAccountState::Running) {
        entry.running_since_ms = options_.now_ms();
    }
    entry.state = to;
    entry.transitions.push_back(std::move(transition));
    if (entry.transitions.size() > 64) {
        entry.transitions.erase(entry.transitions.begin(),
                                entry.transitions.end() - static_cast<std::ptrdiff_t>(32));
    }
    return std::nullopt;
}

ChannelManager::AddAccountResult ChannelManager::AddAccount(
    const std::string& channel_id, const std::string& account_id,
    const ChannelAccountUserConfig& config, ChannelBridgeTransport* transport) {
    std::lock_guard<std::mutex> lock(mutex_);
    AddAccountResult result;
    if (Find(channel_id, account_id) != nullptr) {
        result.status = AddAccountResult::Status::IoError;
        result.detail = "账号已在册: " + channel_id + "/" + account_id;
        return result;
    }

    // 账号锁(configuration.md §11):锁文件在 state_root/locks 下,先于
    // 任何目录建立与桥活动。
    AccountLockRecord self;
    self.pid = CurrentPid();
    self.start_time_ms = 0;  // 进程启动时刻宿主进程自己知道;channel 库不
                             // 碰 platform 的进程账,0 = 未记(锁只对 pid 核活)
    self.acquired_at_ms = options_.now_ms();
    self.generation = 1;
    self.instance_token = instance_token_;

    AccountLock lock_attempt;
    const auto acquire = AccountLock::TryAcquire(LockFile(options_.state_root, channel_id,
                                                          account_id),
                                                 self, options_.alive_checker, &lock_attempt);
    if (acquire.status != AccountLock::AcquireResult::Status::Acquired) {
        result.status = AddAccountResult::Status::LockRefused;
        result.lock_holder = acquire.holder;
        result.detail = acquire.detail;
        return result;
    }

    auto entry = std::make_unique<AccountEntry>();
    entry->channel_id = channel_id;
    entry->account_id = account_id;
    entry->config = config;
    entry->transport = transport;
    entry->lock = std::move(lock_attempt);
    entry->inbox = std::make_unique<ChannelInbox>(options_.inbox_limits);

    const std::filesystem::path dir = AccountDir(options_.state_root, channel_id, account_id);
    ChannelIngressStore::OpenResult ingress_result;
    entry->ingress = ChannelIngressStore::Open(dir, channel_id, account_id, &ingress_result);
    if (entry->ingress->write_blocked()) {
        result.status = AddAccountResult::Status::ReplayError;
        result.detail = entry->ingress->last_error();
        return result;
    }

    entry->pairing = PairingStore::Open(dir, channel_id, account_id);
    if (entry->pairing->write_blocked()) {
        result.status = AddAccountResult::Status::IoError;
        result.detail = entry->pairing->last_error();
        return result;
    }

    // 挂 wake 源(README §4:ChannelManager 每个活跃账号挂一枚 wake source)。
    if (options_.wake != nullptr) {
        const std::string channel = channel_id;
        const std::string account = account_id;
        options_.wake->SetAccountWakeSource(
            channel, account, [this, channel, account] { return HasPendingWork(channel, account); });
    }

    accounts_.push_back(std::move(entry));
    result.status = AddAccountResult::Status::Ok;
    if (ingress_result.skipped_lines > 0) {
        result.detail = "journal replay 跳过 " + std::to_string(ingress_result.skipped_lines) +
                        " 行坏行";
    }
    return result;
}

std::optional<std::string> ChannelManager::StartAccount(const std::string& channel_id,
                                                        const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) return "账号不在册: " + channel_id + "/" + account_id;
    if (IsUnrecoverableAccountState(entry->state)) {
        return std::string("账号在不可恢复终态,须先复位: ") +
               ChannelAccountStateName(entry->state);
    }
    if (entry->state != ChannelAccountState::Disabled &&
        entry->state != ChannelAccountState::Stopped) {
        return std::string("账号已在跑或正在起: ") + ChannelAccountStateName(entry->state);
    }
    if (entry->transport == nullptr) {
        return "账号没接 bridge transport(观测注册,不能起)";
    }

    // Disabled/Stopped -> Validating -> Starting,发 initialize。
    // generation 随每次起账号递增(状态迁移账携带)。
    entry->generation += 1;
    if (const auto error = TransitionLocked(*entry, ChannelAccountState::Validating, "", "")) {
        return error;
    }
    if (const auto error = TransitionLocked(*entry, ChannelAccountState::Starting, "", "")) {
        return error;
    }
    nlohmann::json params = nlohmann::json::object();
    params["protocol_version"] = std::string(kBridgeHandshakeProtocolVersion);
    params["channel_id"] = channel_id;
    params["account_id"] = account_id;
    params["state_dir"] = platform::PathToUtf8(AccountDir(options_.state_root, channel_id, account_id));
    params["locale"] = "zh-CN";
    params["host"] = {{"name", "lubancode"}, {"version", "0.x"}};
    params["requested_capabilities"] = nlohmann::json{{"inbound", nlohmann::json::array({"text"})},
                                                      {"delivery", nlohmann::json::array({"send"})}};
    entry->initialize_request_id =
        entry->router.EnqueueOutgoingRequest(BridgeMethod::Initialize, params);
    FlushOutboundLocked(*entry);
    return std::nullopt;
}

std::optional<std::string> ChannelManager::StopAccount(const std::string& channel_id,
                                                       const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) return "账号不在册: " + channel_id + "/" + account_id;

    // 幂等路径:已停/没起/终态,只收锁与状态。
    if (entry->state == ChannelAccountState::Disabled) {
        if (const auto error =
                TransitionLocked(*entry, ChannelAccountState::Stopped, std::string(kReasonStopped), "")) {
            return error;
        }
        entry->lock.Release();
        return std::nullopt;
    }
    if (entry->state == ChannelAccountState::Stopped || IsUnrecoverableAccountState(entry->state)) {
        entry->lock.Release();
        return std::nullopt;
    }

    if (const auto error =
            TransitionLocked(*entry, ChannelAccountState::Stopping, std::string(kReasonStopped), "")) {
        return error;
    }
    if (entry->transport != nullptr) {
        // bridge-protocol.md §4:channel.stop 的 params 是空对象。
        nlohmann::json params = nlohmann::json::object();
        entry->stop_request_id = entry->router.EnqueueOutgoingRequest(BridgeMethod::Stop, params);
        FlushOutboundLocked(*entry);
        // 同步泵:stop 的应答在 DrainFromSidecar 里,当场收掉。
        const std::vector<std::byte> reply = entry->transport->DrainFromSidecar();
        if (!reply.empty()) {
            HandleBytesFromSidecarLocked(*entry, reply.data(), reply.size());
        }
    }
    if (entry->state == ChannelAccountState::Stopping) {
        // sidecar 没回话(或没接 transport):直接收口。真进程的
        // shutdown_timeout 杀树在阶段 5 的 transport 实现里。
        if (const auto error =
                TransitionLocked(*entry, ChannelAccountState::Stopped,
                                 std::string(kReasonShutdownTimeout), "stop 未应答,直接收口")) {
            return error;
        }
    }
    entry->lock.Release();
    return std::nullopt;
}

std::optional<std::string> ChannelManager::RestartAccount(const std::string& channel_id,
                                                          const std::string& account_id) {
    if (const auto error = StopAccount(channel_id, account_id)) return error;
    return StartAccount(channel_id, account_id);
}

void ChannelManager::FlushOutboundLocked(AccountEntry& entry) {
    if (entry.transport == nullptr) return;
    while (entry.router.HasOutbound()) {
        const nlohmann::json message = entry.router.PopOutbound();
        const auto encoded = EncodeFrame(message);
        if (!encoded.has_value()) {
            continue;  // 帧编码失败:协议层已校验过的形状,理论不可达
        }
        entry.transport->WriteToSidecar(encoded->data(), encoded->size());
    }
}

void ChannelManager::Pump(const std::string& channel_id, const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr || entry->transport == nullptr) return;
    FlushOutboundLocked(*entry);
    const std::vector<std::byte> reply = entry->transport->DrainFromSidecar();
    if (!reply.empty()) {
        HandleBytesFromSidecarLocked(*entry, reply.data(), reply.size());
    }
}

void ChannelManager::HandleBytesFromSidecar(const std::string& channel_id,
                                            const std::string& account_id, const std::byte* data,
                                            std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) return;
    HandleBytesFromSidecarLocked(*entry, data, size);
}

// 调用方已持 mutex_。
void ChannelManager::HandleBytesFromSidecarLocked(AccountEntry& entry, const std::byte* data,
                                                  std::size_t size) {
    entry.decoder.Feed(data, size);
    while (true) {
        auto next = entry.decoder.TryDecodeNext();
        if (!next.has_value()) {
            // 帧错(超帽/坏 UTF-8/坏帧):立即停 adapter 进退避
            //(bridge-protocol.md §2)。
            NotifyTransportFailureLocked(entry, "invalid_frame", next.error().message);
            return;
        }
        if (!next->has_value()) return;  // 数据不够,等下次喂
        HandleMessageLocked(entry, ParseIncomingMessage(**next));
    }
}

void ChannelManager::HandleMessageLocked(AccountEntry& entry, const IncomingMessage& message) {
    if (message.kind == IncomingMessageKind::Malformed) {
        NotifyTransportFailureLocked(entry, "invalid_frame", message.malformed_reason);
        return;
    }
    if (message.kind == IncomingMessageKind::ResultResponse ||
        message.kind == IncomingMessageKind::ErrorResponse) {
        const auto dispatch = entry.router.Dispatch(message);
        if (dispatch.action == BridgeRouter::DispatchResult::Action::ResponseMatched &&
            dispatch.matched_request_method.has_value()) {
            const BridgeMethod method = *dispatch.matched_request_method;
            if (message.kind == IncomingMessageKind::ErrorResponse) {
                // 错误应答:按 domain 稳定名入状态机。
                NotifyTransportFailureLocked(entry, message.error_message, message.error_message);
                return;
            }
            switch (method) {
                case BridgeMethod::Initialize:
                    // 握手过:Authenticating -> Connecting,发 start。
                    if (const auto error =
                            TransitionLocked(entry, ChannelAccountState::Authenticating, "", "")) {
                        NotifyTransportFailureLocked(entry, std::string(kReasonTransitionFailed),
                                                     *error);
                        return;
                    }
                    if (const auto error =
                            TransitionLocked(entry, ChannelAccountState::Connecting, "", "")) {
                        NotifyTransportFailureLocked(entry, std::string(kReasonTransitionFailed),
                                                     *error);
                        return;
                    }
                    {
                        nlohmann::json params = nlohmann::json::object();
                        params["transport"] = entry.config.transport;
                        entry.start_request_id = entry.router.EnqueueOutgoingRequest(BridgeMethod::Start, params);
                        FlushOutboundLocked(entry);
                    }
                    return;
                case BridgeMethod::Start:
                    if (const auto error =
                            TransitionLocked(entry, ChannelAccountState::Running, "", "")) {
                        NotifyTransportFailureLocked(entry, std::string(kReasonTransitionFailed),
                                                     *error);
                    }
                    return;
                case BridgeMethod::Stop:
                    if (entry.state == ChannelAccountState::Stopping) {
                        if (const auto error =
                                TransitionLocked(entry, ChannelAccountState::Stopped,
                                                 std::string(kReasonStopped), "")) {
                            return;
                        }
                        entry.lock.Release();
                    }
                    return;
                default:
                    return;  // health/send/... 的应答:阶段 3/4 的口,本批不消费
            }
        }
        return;
    }

    // Request/Notification:sidecar -> host 的通知为主(v1 sidecar 不发
    // request;真出现也走 Dispatch 入队,这里统一消费)。
    const auto dispatch = entry.router.Dispatch(message);
    if (dispatch.action != BridgeRouter::DispatchResult::Action::RequestQueued &&
        dispatch.action != BridgeRouter::DispatchResult::Action::NotificationQueued) {
        return;
    }
    while (entry.router.HasInbound()) {
        const IncomingMessage incoming = entry.router.PopInbound();
        if (incoming.kind != IncomingMessageKind::Request &&
            incoming.kind != IncomingMessageKind::Notification) {
            continue;
        }
        if (incoming.method == BridgeMethod::Inbound) {
            std::string event_error;
            auto event = ChannelInboundEvent::FromJsonStrict(incoming.params, &event_error);
            if (!event.has_value()) {
                NotifyTransportFailureLocked(entry, "invalid_frame",
                                             "channel.inbound 事件解析失败: " + event_error);
                continue;
            }
            OnInboundLocked(entry, *event);
        } else if (incoming.method == BridgeMethod::Status) {
            // sidecar 自报状态:connecting/running/degraded/backoff/stopped。
            const std::string state = incoming.params.value("state", "");
            if (state == "stopped" &&
                CanTransition(entry.state, ChannelAccountState::Stopped)) {
                TransitionLocked(entry, ChannelAccountState::Stopped, "transport_failed",
                                 "sidecar 自报 stopped");
            } else if (state == "degraded" && CanTransition(entry.state, ChannelAccountState::Degraded)) {
                TransitionLocked(entry, ChannelAccountState::Degraded, "transport_failed",
                                 "sidecar 自报 degraded");
            } else if (state == "running" &&
                       CanTransition(entry.state, ChannelAccountState::Running)) {
                TransitionLocked(entry, ChannelAccountState::Running, "", "");
            }
        } else if (incoming.method == BridgeMethod::Fatal) {
            const std::string reason = incoming.params.value("reason", "process_crashed");
            NotifyTransportFailureLocked(entry, reason, incoming.params.value("detail", ""));
        }
        // 其余通知(delivery.receipt/login.*/capabilities.changed):阶段
        // 4/5 的口,先入 router 诊断账,不消费。
    }
}

void ChannelManager::OnInboundLocked(AccountEntry& entry, const ChannelInboundEvent& event) {
    // 1) 耐久 + 去重(不 durable 不进任何后续口)。
    const auto ingest = entry.ingress->Ingest(event);
    if (!ingest.has_value()) {
        // durable 失败:不 ack,sidecar 按退避重发(message-contracts.md §3)。
        // 状态 Degraded 一下,账上留痕。
        if (CanTransition(entry.state, ChannelAccountState::Degraded)) {
            TransitionLocked(entry, ChannelAccountState::Degraded, "spool_write_failed",
                             entry.ingress->last_error());
        }
        return;
    }
    // 2) durable 过即 ack(重投的 duplicate 也 ack:让 sidecar 清 spool)。
    {
        nlohmann::json params = nlohmann::json::object();
        params["delivery_id"] = event.delivery_id;
        entry.router.EnqueueOutgoingRequest(BridgeMethod::InboundAck, params);
        FlushOutboundLocked(entry);
    }
    if (ingest->status == ChannelIngressStore::IngestOutcome::Status::Duplicate) {
        return;  // 重复投递:ack 了就完,不开新账
    }

    // 3) 路由准入(阶段 3 ChannelRouter 全账:bot 拒绝/dm_policy/pairing/
    //    group/mention/binding 冲突;阶段 2 的最小 DM 准入退役)。pairing
    //    账经适配器喂给 router——批准只认宿主看到的 sender id。
    const auto route = RouteInboundLocked(entry, event);
    if (route.status == RouteDecision::Status::Rejected) {
        entry.ingress->Transition(ingest->sid, IngressEventState::Rejected, route.reason);
        return;
    }
    if (route.status == RouteDecision::Status::PendingPairing) {
        entry.ingress->Transition(ingest->sid, IngressEventState::Rejected, "pairing_pending");
        return;
    }
    // 准入过了:主线 Authorized -> Routed(message-contracts.md §4)。
    entry.ingress->Transition(ingest->sid, IngressEventState::Authorized, "");
    entry.ingress->Transition(ingest->sid, IngressEventState::Routed, "");

    // 4) inbox 排队 + 背压(满不默丢:nack retry,事件留在 ingress 账上)。
    const auto queue_result = entry.inbox->Enqueue(
        ingest->sid, event.conversation.id, event.sender.id,
        entry.ingress->FindBySid(ingest->sid).has_value()
            ? entry.ingress->FindBySid(ingest->sid)->parts_sha256
            : std::string(),
        options_.now_ms());
    if (queue_result.status == ChannelInbox::EnqueueResult::Status::Accepted) {
        entry.ingress->Transition(ingest->sid, IngressEventState::Queued, "");
        return;
    }
    // 背压:事件已 durable + 已 ack——内存水位满不等于耐久失败,不 nack
    // sidecar(重发只会撞去重键);RateLimited 旁路留账,水位降下后从
    // ingress 账上重排(replay 路径)。nack 留给"宿主要求 sidecar 别再
    // 送这一枚"的明拒场景,不在背压用(bridge-protocol.md §4/§5)。
    entry.ingress->Transition(ingest->sid, IngressEventState::RateLimited,
                              queue_result.reason);
}

RouteDecision ChannelManager::RouteInboundLocked(AccountEntry& entry,
                                                 const ChannelInboundEvent& event) {
    StorePairingAdmission admission(*entry.pairing);
    RouteInput input;
    input.event = &event;
    input.account = &entry.config;
    const auto bindings = channel_bindings_.find(entry.channel_id);
    input.bindings = bindings != channel_bindings_.end() ? &bindings->second : nullptr;
    input.pairing = &admission;
    input.now_ms = options_.now_ms();
    return RouteChannelEvent(input);
}

void ChannelManager::SetChannelBindings(const std::string& channel_id,
                                        std::vector<ChannelBindingConfig> bindings) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bindings.empty()) {
        channel_bindings_.erase(channel_id);
        return;
    }
    channel_bindings_[channel_id] = std::move(bindings);
}

void ChannelManager::NotifyTransportFailure(const std::string& channel_id,
                                            const std::string& account_id, const std::string& reason,
                                            const std::string& detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) return;
    NotifyTransportFailureLocked(*entry, reason, detail);
}

// 调用方已持 mutex_。
void ChannelManager::NotifyTransportFailureLocked(AccountEntry& entry, const std::string& reason,
                                                  const std::string& detail) {
    // 不可自愈的 reason:按语义落不可恢复终态(configuration.md §9/§10)。
    if (reason == std::string(kReasonMisconfigured) ||
        reason == std::string(kReasonTrustRequired)) {
        if (CanTransition(entry.state, ChannelAccountState::Misconfigured)) {
            TransitionLocked(entry, ChannelAccountState::Misconfigured, reason, detail);
        } else if (CanTransition(entry.state, ChannelAccountState::Fatal)) {
            TransitionLocked(entry, ChannelAccountState::Fatal, reason, detail);
        }
        return;
    }
    if (reason == "login_required") {
        if (CanTransition(entry.state, ChannelAccountState::NeedsLogin)) {
            TransitionLocked(entry, ChannelAccountState::NeedsLogin, reason, detail);
        } else if (CanTransition(entry.state, ChannelAccountState::Fatal)) {
            TransitionLocked(entry, ChannelAccountState::Fatal, reason, detail);
        }
        return;
    }
    if (reason == "protocol_incompatible" || reason == "account_revoked" ||
        reason == std::string(kReasonAccountInUse)) {
        if (CanTransition(entry.state, ChannelAccountState::Fatal)) {
            TransitionLocked(entry, ChannelAccountState::Fatal, reason, detail);
        }
        return;
    }
    // 可重试:Degraded(从 Running 掉下来)或 Backoff(已在 Degraded)。
    if (CanTransition(entry.state, ChannelAccountState::Degraded)) {
        TransitionLocked(entry, ChannelAccountState::Degraded, reason, detail);
    } else if (CanTransition(entry.state, ChannelAccountState::Backoff)) {
        TransitionLocked(entry, ChannelAccountState::Backoff, reason, detail);
    } else if (CanTransition(entry.state, ChannelAccountState::Fatal)) {
        TransitionLocked(entry, ChannelAccountState::Fatal, reason, detail);
    }
}

void ChannelManager::NotifyStableRunning(const std::string& channel_id,
                                         const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) return;
    if (entry->state == ChannelAccountState::Running &&
        options_.now_ms() - entry->running_since_ms >= kBackoffResetAfterStableMs) {
        entry->backoff_attempt = 0;
        entry->retry_at_ms = 0;
    }
}

ChannelManager::AccountSnapshot ChannelManager::SnapshotLocked(const AccountEntry& entry) const {
    AccountSnapshot snapshot;
    snapshot.channel_id = entry.channel_id;
    snapshot.account_id = entry.account_id;
    snapshot.state = entry.state;
    snapshot.generation = entry.generation;
    snapshot.running_since_ms = entry.running_since_ms;
    snapshot.retry_at_ms = entry.retry_at_ms;
    snapshot.backoff_attempt = entry.backoff_attempt;
    const std::size_t recent = std::min(kMaxRecentTransitions, entry.transitions.size());
    snapshot.recent_transitions.assign(entry.transitions.end() - static_cast<std::ptrdiff_t>(recent),
                                       entry.transitions.end());
    snapshot.inbox_pending = entry.inbox->pending_total();
    snapshot.ingress_state_counts = entry.ingress->StateCounts();
    snapshot.dead_letter_count = entry.ingress->dead_letter_count();
    snapshot.pairing_pending = entry.pairing->PendingList(options_.now_ms()).size();
    snapshot.pairing_approved = entry.pairing->approved_count();
    snapshot.lock_held = entry.lock.holds();
    snapshot.dm_policy = entry.config.dm_policy;
    snapshot.group_policy = entry.config.group_policy;
    snapshot.credential = DescribeCredentialSource(entry.config);
    return snapshot;
}

std::vector<ChannelManager::AccountSnapshot> ChannelManager::Snapshots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AccountSnapshot> out;
    out.reserve(accounts_.size());
    for (const auto& entry : accounts_) {
        out.push_back(SnapshotLocked(*entry));
    }
    return out;
}

std::optional<ChannelManager::AccountSnapshot> ChannelManager::Snapshot(
    const std::string& channel_id, const std::string& account_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) return std::nullopt;
    return SnapshotLocked(*entry);
}

bool ChannelManager::HasPendingWork(const std::string& channel_id,
                                    const std::string& account_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) return false;
    return entry->inbox->pending_total() > 0;
}

std::size_t ChannelManager::account_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return accounts_.size();
}

std::optional<ChannelManager::WorkItem> ChannelManager::TakeNextWork(
    const std::string& channel_id, const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) return std::nullopt;
    const auto item = entry->inbox->TakeNext();
    if (!item.has_value()) return std::nullopt;
    WorkItem work;
    work.sid = item->sid;
    work.conversation_id = item->conversation_id;
    work.sender_id = item->sender_id;
    const auto record = entry->ingress->FindBySid(item->sid);
    if (record.has_value()) {
        work.event = record->event;
        entry->ingress->Transition(item->sid, IngressEventState::Running, "");
        // 路由全账现跑(纯函数:与准入时同一只 RouteChannelEvent,同样的
        // 输入同样的决策)。准入后配置又改了(如 SetChannelBindings)按新
        // 账算——正在跑的 turn 不受影响,新 turn 用新快照(§8.4)。
        work.route = RouteInboundLocked(*entry, work.event);
    }
    return work;
}

std::vector<ChannelManager::PendingPairingView> ChannelManager::PendingPairings(
    const std::string& channel_id, const std::string& account_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) return {};
    std::vector<PendingPairingView> out;
    for (const auto& pending : entry->pairing->PendingList(options_.now_ms())) {
        PendingPairingView view;
        view.sender_id = pending.sender_id;
        view.expires_at_ms = pending.expires_at_ms;
        out.push_back(std::move(view));
    }
    return out;
}

std::optional<std::string> ChannelManager::ApprovePairing(const std::string& channel_id,
                                                          const std::string& account_id,
                                                          const std::string& code,
                                                          std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) {
        if (error != nullptr) *error = "account_not_found";
        return std::nullopt;
    }
    auto approved = entry->pairing->Approve(code, options_.now_ms());
    if (!approved.has_value() && error != nullptr) {
        // Approve 没带出参,失败原因从账上取(stable reason)。
        *error = entry->pairing->last_error();
    }
    return approved;
}

std::optional<std::string> ChannelManager::RejectPairing(const std::string& channel_id,
                                                         const std::string& account_id,
                                                         const std::string& code,
                                                         std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) {
        if (error != nullptr) *error = "account_not_found";
        return std::nullopt;
    }
    return entry->pairing->Reject(code, options_.now_ms(), error);
}

}  // namespace lubancode::channel
