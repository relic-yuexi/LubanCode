#include "channel/manager.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>

#include "channel/bridge_protocol.hpp"
#include "channel/digest.hpp"

#include "config/config.hpp"  // StateRootDir:DefaultChannelsStateRoot 的根来源
#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace lubancode::channel {

std::filesystem::path DefaultChannelsStateRoot() {
    // 见 manager.hpp 合同注释:状态整树随状态根走(应用根=数据根,个人=
    // ~/.lubancode 原样);坏值/无主目录时 StateRootDir 给 nullopt,这里
    // 回空 path——启动门已对坏值明拒,库级消费按"无根"明报不回落。
    const auto state_root = config::StateRootDir();
    if (!state_root.has_value()) {
        return std::filesystem::path();
    }
    return platform::Utf8ToPath(*state_root) / "channels";
}

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
    // 盘上状态快照(Q2:gateway status 渠道栏的进程外只读面)。迁移是有
    // 界事件,每笔写一次原子件;写不进只留账(状态机本身不依赖它)。
    PersistAccountStatusLocked(entry);
    return std::nullopt;
}

void ChannelManager::PersistAccountStatusLocked(const AccountEntry& entry) {
    nlohmann::json status = nlohmann::json::object();
    status["schema"] = 1;
    status["channelId"] = entry.channel_id;
    status["accountId"] = entry.account_id;
    status["state"] = ChannelAccountStateName(entry.state);
    status["generation"] = entry.generation;
    status["updatedAtMs"] = options_.now_ms();
    const AccountStatusTransition& last = entry.transitions.back();
    status["lastReason"] = last.reason;
    status["lastDetail"] = last.detail;
    const std::filesystem::path file =
        AccountDir(options_.state_root, entry.channel_id, entry.account_id) / "account-status.json";
    (void)platform::AtomicWriteFile(file, status.dump(),
                                    platform::WriteDurability::ProcessCrashDurability);
}

ChannelManager::ChannelAccountStatusFile ChannelManager::ReadAccountStatusFile(
    const std::filesystem::path& account_dir) {
    ChannelAccountStatusFile out;
    const std::filesystem::path file = account_dir / "account-status.json";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) {
        return out;
    }
    std::ifstream stream(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return out;
    }
    // json 缺键一律 contains()(const operator[] 查缺键是 UB)。
    auto read_string = [&parsed](const char* key) {
        return parsed.contains(key) && parsed[key].is_string()
                   ? parsed[key].get<std::string>()
                   : std::string();
    };
    out.present = true;
    out.channel_id = read_string("channelId");
    out.account_id = read_string("accountId");
    out.state = read_string("state");
    if (parsed.contains("generation") && parsed["generation"].is_number_integer()) {
        out.generation = parsed["generation"].get<int>();
    }
    if (parsed.contains("updatedAtMs") && parsed["updatedAtMs"].is_number_integer()) {
        out.updated_at_ms = parsed["updatedAtMs"].get<std::int64_t>();
    }
    out.last_reason = read_string("lastReason");
    out.last_detail = read_string("lastDetail");
    return out;
}

ChannelManager::AddAccountResult ChannelManager::AddAccount(
    const std::string& channel_id, const std::string& account_id,
    const ChannelAccountUserConfig& config, ChannelBridgeTransport* transport) {
    std::lock_guard<std::mutex> lock(mutex_);
    AddAccountResult result;
    // id 先过守门(QQ 接入单 Q0):id 直接拼 state_root/<ch>/<acct> 与锁
    // 文件名,带路径段的 id 拼得出状态根外路径——fail closed。
    if (!IsValidChannelId(channel_id)) {
        result.status = AddAccountResult::Status::InvalidArgument;
        result.detail = "channel id 不合法(非空、无路径段、无控制字符、<=64 字符): " +
                        channel_id;
        return result;
    }
    if (!IsValidChannelAccountId(account_id)) {
        result.status = AddAccountResult::Status::InvalidArgument;
        result.detail = "account id 不合法(非空、无路径段、无控制字符、<=64 字符): " +
                        account_id;
        return result;
    }
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
    // 在途 send 的超时裁决(Q2 §七:超时 = delivery_unknown,停自动重发)。
    ExpireStaleSendsLocked(*entry);
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
                if (method == BridgeMethod::Send) {
                    // channel.send 的错误应答(Q2 §七):按 domain 稳定名分型
                    // 结算,不进传输层状态机(发送失败 ≠ 账号故障)。
                    if (dispatch.matched_request_id.has_value() &&
                        entry.pending_sends.count(*dispatch.matched_request_id) > 0) {
                        ClassifySendErrorLocked(entry, *dispatch.matched_request_id,
                                                message.error_message,
                                                message.error_data);
                    }
                    return;
                }
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
                case BridgeMethod::Send: {
                    // channel.send 的成功应答(Q2 §七第三项):accepted 与
                    // QQ 已接受回执一并结算(provider_message_id 记账)。
                    const std::string provider_message_id =
                        message.result.is_object() && message.result.contains("provider_message_id") &&
                                message.result["provider_message_id"].is_string()
                            ? message.result["provider_message_id"].get<std::string>()
                            : std::string();
                    if (dispatch.matched_request_id.has_value()) {
                        SettleSendLocked(entry, *dispatch.matched_request_id,
                                         ChannelDeliveryOutcome::Status::Accepted,
                                         provider_message_id, "", "");
                    }
                    return;
                }
                default:
                    return;  // health/edit/... 的应答:后续批次的口
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
        } else if (incoming.method == BridgeMethod::DeliveryReceipt) {
            // 平台异步回执(Q2 §七:与 send 请求按 outbound_delivery_id 关联;
            // 重复/陈旧回执只留诊断)。
            OnDeliveryReceiptLocked(entry, incoming.params);
        }
        // 其余通知(login.*/capabilities.changed):后续批次的口,先入
        // router 诊断账,不消费。
    }
}

void ChannelManager::OnInboundLocked(AccountEntry& entry, const ChannelInboundEvent& event) {
    // 0) 身份复核(QQ 接入单 Q0):远端事件的账号身份取实际连接上下文,
    // 宿主逐枚复核 channel/account。sidecar 报上来的事件若声称别的账号
    // (跨账号伪造)按协议错处置——不 ingest、不 ack,账号进 Degraded。
    // sender 永远取事件信封的 sender 字段,不从正文提。
    if (event.channel_id != entry.channel_id || event.account_id != entry.account_id) {
        NotifyTransportFailureLocked(entry, "invalid_frame",
                                     "channel.inbound 事件身份与连接不符(事件自称 " +
                                         event.channel_id + "/" + event.account_id + ",连接是 " +
                                         entry.channel_id + "/" + entry.account_id + ")");
        return;
    }

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
        // Q1b 配对提示:宿主生成的提示经 reply outbox 发给来者。限频——
        // 同 sender 同账号冷却窗内只发一次(持久已提示账,重启不重发);
        // 被拒/限速不发 code 的来信(router 返空 code)也不发提示。提示
        // 只入队不直发:投递走 Q2 既有链路(泵排水进 outbox 渠道段)。
        if (!route.pairing_code.empty() &&
            entry.pairing->MarkNoticeSent(event.sender.id, event.conversation.id,
                                          options_.now_ms())) {
            PairingNotice notice;
            notice.conversation_id = event.conversation.id;
            notice.reply_to_message_id = event.message_id;
            notice.sender_id = event.sender.id;
            notice.code = route.pairing_code;
            notice.text = MakePairingNoticeText(route.pairing_code, entry.channel_id,
                                                entry.account_id);
            notice.trigger_sid = ingest->sid;
            entry.pending_pairing_notices.push_back(std::move(notice));
        }
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
    // 渠道层 tools 上限(Q0 五层交集的渠道层):宿主递了才参与。
    const auto tools = channel_tools_.find(entry.channel_id);
    if (tools != channel_tools_.end()) {
        input.channel_tools = &tools->second;
    }
    const auto bindings = channel_bindings_.find(entry.channel_id);
    input.bindings = bindings != channel_bindings_.end() ? &bindings->second : nullptr;
    input.pairing = &admission;
    input.now_ms = options_.now_ms();
    return RouteChannelEvent(input);
}

RouteDecision ChannelManager::ProbeRoute(const std::string& channel_id,
                                         const std::string& account_id,
                                         const ChannelConversation& conversation,
                                         const std::string& sender_id,
                                         std::int64_t now_ms) const {
    // 只读探针:同一只纯函数路由器,不落账不发提示——pairing 口用只读
    // 适配(未知 sender 不领新码)。查无账号回 nullopt 语义的空决策
    //(status 缺省 Rejected,reason 留空,调用方按"探不到"处理)。
    RouteDecision decision;
    const std::lock_guard<std::mutex> lock(mutex_);
    const AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) {
        decision.reason = "account_not_found";
        return decision;
    }
    // 最小事件投影:路由只看会话/sender/bot 位/群提示,正文与消息 id
    // 不参与准入(与 OnInboundLocked 同一只函数同一份账)。
    ChannelInboundEvent event;
    event.channel_id = channel_id;
    event.account_id = account_id;
    event.conversation = conversation;
    event.sender.id = sender_id;
    event.received_at_ms = now_ms;
    class ReadOnlyPairing final : public PairingAdmission {
    public:
        explicit ReadOnlyPairing(const PairingStore& store) : store_(store) {}
        bool IsSenderApproved(const std::string& sender_id) const override {
            return store_.IsSenderApproved(sender_id);
        }
        std::optional<std::string> RequestCode(const std::string&, std::int64_t) override {
            return std::nullopt;  // 探针不发码:配对提示只随真来信走
        }

    private:
        const PairingStore& store_;
    };
    ReadOnlyPairing admission(*entry->pairing);
    RouteInput input;
    input.event = &event;
    input.account = &entry->config;  // RouteInput 本就收 const 指针(纯函数路由)
    const auto tools = channel_tools_.find(channel_id);
    if (tools != channel_tools_.end()) {
        input.channel_tools = &tools->second;
    }
    const auto bindings = channel_bindings_.find(channel_id);
    input.bindings = bindings != channel_bindings_.end() ? &bindings->second : nullptr;
    input.pairing = &admission;
    input.now_ms = now_ms;
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

void ChannelManager::SetChannelToolsPolicy(const std::string& channel_id,
                                           ChannelToolsUserPolicy tools) {
    std::lock_guard<std::mutex> lock(mutex_);
    channel_tools_[channel_id] = std::move(tools);
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
    // 执行前重验准入(QQ 接入单 Q0):权限撤销影响排队输入——入队后
    // 配置改了(撤 allow_from/bindings 收紧/撤销 pairing),重跑同一只
    // 纯函数不过的输入就地落 Rejected,不进执行;过账的决策即本轮的
    // 冻结策略版本。已开始的工具沿现有取消边界收场,不在这里追杀。
    while (true) {
        const auto item = entry->inbox->TakeNext();
        if (!item.has_value()) return std::nullopt;
        const auto record = entry->ingress->FindBySid(item->sid);
        if (!record.has_value()) {
            continue;  // 账上查不回(理论不可达):不执行,取下一件
        }
        WorkItem work;
        work.sid = item->sid;
        work.conversation_id = item->conversation_id;
        work.sender_id = item->sender_id;
        work.event = record->event;
        // 路由全账现跑(纯函数:与准入时同一只 RouteChannelEvent,同样的
        // 输入同样的决策)。
        work.route = RouteInboundLocked(*entry, work.event);
        if (work.route.status != RouteDecision::Status::Admitted) {
            // 重验不过:落账退场,接着看下一件排队输入。
            entry->ingress->Transition(item->sid, IngressEventState::Rejected,
                                       work.route.reason.empty() ? "revoked_before_run"
                                                                 : work.route.reason);
            continue;
        }
        entry->ingress->Transition(item->sid, IngressEventState::Running, "");
        return work;
    }
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

std::optional<std::string> ChannelManager::ApprovePairingBySender(
    const std::string& channel_id, const std::string& account_id, const std::string& sender_id,
    std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) {
        if (error != nullptr) *error = "account_not_found";
        return std::nullopt;
    }
    return entry->pairing->ApproveSender(sender_id, options_.now_ms(), error);
}

std::optional<std::string> ChannelManager::RejectPairingBySender(
    const std::string& channel_id, const std::string& account_id, const std::string& sender_id,
    std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) {
        if (error != nullptr) *error = "account_not_found";
        return std::nullopt;
    }
    return entry->pairing->RejectSender(sender_id, options_.now_ms(), error);
}

std::vector<ChannelManager::PairingNotice> ChannelManager::DrainPendingPairingNotices(
    const std::string& channel_id, const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) return {};
    std::vector<PairingNotice> out;
    out.swap(entry->pending_pairing_notices);
    return out;
}

// ---- 出站投递(Q2 §七) ------------------------------------------------------

std::optional<std::string> ChannelManager::SendReply(const std::string& channel_id,
                                                     const std::string& account_id,
                                                     const ChannelSendRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) {
        return std::string("账号不在册: ") + channel_id + "/" + account_id;
    }
    if (entry->transport == nullptr) {
        return "账号没接 bridge transport,不能发";
    }
    if (entry->state != ChannelAccountState::Running) {
        return std::string("账号不在 Running(当前 ") + ChannelAccountStateName(entry->state) +
               "),不能发";
    }
    if (request.conversation_id.empty() || request.client_delivery_id.empty() ||
        (request.text.empty() && !request.attachment.has_value())) {
        return "发送请求缺 conversation/text/client_delivery_id";
    }
    // 已结算过的 delivery 不再受理(终态不翻转;防跨代次重复投递)。
    if (entry->settled_deliveries.count(request.client_delivery_id) > 0) {
        entry->send_diagnostics.push_back("duplicate_send_rejected: " +
                                          request.client_delivery_id);
        return std::string("delivery 已结算过,不再发送: ") + request.client_delivery_id;
    }
    // 帧(bridge-protocol.md §4 channel.send):client_id 供平台幂等——
    // 同 delivery 重试同载荷(适配器按它稳定 msg_seq)。Q4:file part 折
    // 出站附件引用(适配器读原件上传,走 msg_type=7);纯文本段不带。
    nlohmann::json params = nlohmann::json::object();
    params["conversation"] = nlohmann::json{{"kind", "direct"}, {"id", request.conversation_id}};
    nlohmann::json parts = nlohmann::json::array();
    if (!request.text.empty()) {
        parts.push_back(nlohmann::json{{"type", "text"}, {"text", request.text}});
    }
    if (request.attachment.has_value()) {
        nlohmann::json file_part = nlohmann::json{
            {"type", "file"},
            {"mime_type", request.attachment->mime_type},
            {"local_path", request.attachment->local_path}};
        if (!request.attachment->file_name.empty()) {
            file_part["file_name"] = request.attachment->file_name;
        }
        if (request.attachment->size_bytes > 0) {
            file_part["size"] = request.attachment->size_bytes;
        }
        parts.push_back(std::move(file_part));
    }
    params["parts"] = std::move(parts);
    if (!request.reply_to_message_id.empty()) {
        params["reply_to_message_id"] = request.reply_to_message_id;
    }
    params["client_id"] = request.client_delivery_id;
    const std::int64_t request_id = entry->router.EnqueueOutgoingRequest(BridgeMethod::Send, params);
    FlushOutboundLocked(*entry);  // 受理即编码写给 sidecar(字节面同步)

    PendingChannelSend pending;
    pending.request_id = request_id;
    pending.client_delivery_id = request.client_delivery_id;
    pending.conversation_id = request.conversation_id;
    pending.generation = entry->generation;
    pending.sent_at_ms = options_.now_ms();
    entry->pending_sends.emplace(request_id, std::move(pending));
    return std::nullopt;
}

void ChannelManager::SettleSendLocked(AccountEntry& entry, std::int64_t request_id,
                                      ChannelDeliveryOutcome::Status status,
                                      const std::string& provider_message_id,
                                      const std::string& error_code, const std::string& detail) {
    const auto pending = entry.pending_sends.find(request_id);
    if (pending == entry.pending_sends.end()) {
        return;  // 不在途(迟到/陌生):只留诊断,不结算
    }
    const PendingChannelSend send = pending->second;
    entry.pending_sends.erase(pending);
    if (send.generation != entry.generation) {
        // 代次隔离(§七第三项):受理与结算之间账号重启过,这笔回执属旧
        // 代次——不产出结算(重发按同 delivery_id 走新代次,平台按
        // msg_id+msg_seq 去重兜底)。
        entry.send_diagnostics.push_back("stale_receipt_dropped: " + send.client_delivery_id +
                                         " gen " + std::to_string(send.generation) + " -> " +
                                         std::to_string(entry.generation));
        return;
    }
    // 重复回执只结一次(§七:send 响应与 delivery.receipt 都可能来)。
    // settled 账只记终态(Accepted/Rejected/AuthFailed/Unknown)——限频是
    // 可重试态,不进 settled(否则同 delivery 的合法重试会被 SendReply 拒)。
    const bool terminal = status != ChannelDeliveryOutcome::Status::RateLimited;
    if (terminal && entry.settled_deliveries.count(send.client_delivery_id) > 0) {
        entry.send_diagnostics.push_back("duplicate_receipt_ignored: " +
                                         send.client_delivery_id);
        return;
    }
    if (terminal) {
        entry.settled_deliveries[send.client_delivery_id] = entry.generation;
        if (entry.settled_deliveries.size() > 512) {
            // 有界:旧的先丢(同 delivery 重复回执通常紧跟着来;这里只防无界涨)。
            entry.settled_deliveries.erase(entry.settled_deliveries.begin());
        }
    }
    ChannelDeliveryOutcome outcome;
    outcome.client_delivery_id = send.client_delivery_id;
    outcome.status = status;
    outcome.provider_message_id = provider_message_id;
    outcome.error_code = error_code;
    outcome.detail = detail;
    outcome.settled_at_ms = options_.now_ms();
    outcome.generation = entry.generation;
    entry.delivery_outcomes.push_back(std::move(outcome));
    if (entry.delivery_outcomes.size() > 256) {
        entry.delivery_outcomes.erase(entry.delivery_outcomes.begin());
    }
}

void ChannelManager::ClassifySendErrorLocked(AccountEntry& entry, std::int64_t request_id,
                                             const std::string& stable_name,
                                             const nlohmann::json& error_data) {
    // 分型(§七第五项):限频/传输暂态 → 可重试;令牌失效 → AuthFailed
    //(账号已由 NotifyTransportFailureLocked 转 NeedsLogin);明确拒绝按
    // 细节分回复窗口过期/平台拒绝。
    const std::string detail =
        error_data.is_object() && error_data.contains("detail") && error_data["detail"].is_string()
            ? error_data["detail"].get<std::string>()
            : std::string();
    const auto name = DomainErrorNameFromStableName(stable_name);
    if (name == DomainErrorName::RateLimited || name == DomainErrorName::TransportFailed) {
        SettleSendLocked(entry, request_id, ChannelDeliveryOutcome::Status::RateLimited, "",
                         "rate_limited", detail.empty() ? stable_name : detail);
        return;
    }
    if (name == DomainErrorName::LoginRequired) {
        // 令牌失效:账号状态同步转 NeedsLogin(发送失败 ≠ 账号故障,故
        // 只这一类穿透到状态机)。
        NotifyTransportFailureLocked(entry, "login_required", detail);
        SettleSendLocked(entry, request_id, ChannelDeliveryOutcome::Status::AuthFailed, "",
                         "auth_failed", detail);
        return;
    }
    if (detail.find("expired") != std::string::npos) {
        // 回复窗口过期(msg_id 过期):不擅自转主动消息,终态失败。
        SettleSendLocked(entry, request_id, ChannelDeliveryOutcome::Status::Rejected, "",
                         "reply_window_expired", detail);
        return;
    }
    SettleSendLocked(entry, request_id, ChannelDeliveryOutcome::Status::Rejected, "",
                     "platform_reject", detail.empty() ? stable_name : detail);
}

void ChannelManager::OnDeliveryReceiptLocked(AccountEntry& entry, const nlohmann::json& params) {
    // 通知无 pending request 可配对——按 outbound_delivery_id 在在途/已结
    // 算账里关联(§七第三项"与发送请求关联")。
    if (!params.is_object() || !params.contains("outbound_delivery_id") ||
        !params["outbound_delivery_id"].is_string()) {
        entry.send_diagnostics.push_back("delivery.receipt 缺 outbound_delivery_id");
        return;
    }
    const std::string delivery_id = params["outbound_delivery_id"].get<std::string>();
    const std::string outcome_text =
        params.contains("outcome") && params["outcome"].is_string()
            ? params["outcome"].get<std::string>()
            : std::string();
    const std::string provider_message_id =
        params.contains("provider_message_id") && params["provider_message_id"].is_string()
            ? params["provider_message_id"].get<std::string>()
            : std::string();
    const std::string reason = params.contains("reason") && params["reason"].is_string()
                                   ? params["reason"].get<std::string>()
                                   : std::string();
    // 已结算:重复回执只留诊断(SettleSendLocked 同款裁决)。
    if (entry.settled_deliveries.count(delivery_id) > 0) {
        entry.send_diagnostics.push_back("duplicate_receipt_ignored: " + delivery_id);
        return;
    }
    // 在途匹配(按 delivery_id 反查 pending)。
    for (const auto& [request_id, pending] : entry.pending_sends) {
        if (pending.client_delivery_id != delivery_id) continue;
        if (pending.generation != entry.generation) {
            entry.send_diagnostics.push_back("stale_receipt_dropped: " + delivery_id);
            return;
        }
        if (outcome_text == "delivered") {
            SettleSendLocked(entry, request_id, ChannelDeliveryOutcome::Status::Accepted,
                             provider_message_id, "", "");
        } else if (outcome_text == "failed") {
            if (reason.find("rate") != std::string::npos) {
                SettleSendLocked(entry, request_id, ChannelDeliveryOutcome::Status::RateLimited,
                                 "", "rate_limited", reason);
            } else {
                SettleSendLocked(entry, request_id, ChannelDeliveryOutcome::Status::Rejected, "",
                                 "platform_reject", reason);
            }
        } else {
            entry.send_diagnostics.push_back("delivery.receipt 未知 outcome: " + outcome_text);
        }
        return;
    }
    entry.send_diagnostics.push_back("receipt_unmatched: " + delivery_id);
}

void ChannelManager::ExpireStaleSendsLocked(AccountEntry& entry) {
    const std::int64_t now = options_.now_ms();
    std::vector<std::int64_t> expired;
    for (const auto& [request_id, pending] : entry.pending_sends) {
        if (now - pending.sent_at_ms > options_.send_timeout_ms) {
            expired.push_back(request_id);
        }
    }
    for (const std::int64_t request_id : expired) {
        // 超时 = delivery_unknown(§七第四项):可能是 QQ 已收到、回执丢失;
        // 不虚 exactly-once,停自动重发。
        SettleSendLocked(entry, request_id, ChannelDeliveryOutcome::Status::Unknown, "",
                         "delivery_unknown", "回执超时");
    }
}

std::vector<ChannelManager::ChannelDeliveryOutcome> ChannelManager::DrainChannelDeliveryOutcomes(
    const std::string& channel_id, const std::string& account_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) {
        return {};
    }
    std::vector<ChannelDeliveryOutcome> out = std::move(entry->delivery_outcomes);
    entry->delivery_outcomes.clear();
    return out;
}

bool ChannelManager::HasPendingSend(const std::string& channel_id, const std::string& account_id,
                                    const std::string& client_delivery_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) {
        return false;
    }
    for (const auto& [request_id, pending] : entry->pending_sends) {
        if (pending.client_delivery_id == client_delivery_id) {
            return true;
        }
    }
    return false;
}

std::vector<ChannelManager::IngressRunningView> ChannelManager::ListRunningIngress(
    const std::string& channel_id, const std::string& account_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) {
        return {};
    }
    std::vector<IngressRunningView> out;
    for (const auto& record : entry->ingress->Records()) {
        if (record.state != IngressEventState::Running) {
            continue;
        }
        IngressRunningView view;
        view.sid = record.sid;
        view.conversation_id = record.event.conversation.id;
        view.event = record.event;
        out.push_back(std::move(view));
    }
    return out;
}

std::optional<std::string> ChannelManager::SettleIngressReplied(const std::string& channel_id,
                                                                const std::string& account_id,
                                                                std::int64_t sid) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) {
        return std::string("账号不在册: ") + channel_id + "/" + account_id;
    }
    return entry->ingress->Transition(sid, IngressEventState::Replied, "");
}

std::optional<std::string> ChannelManager::SettleIngressDelivered(const std::string& channel_id,
                                                                  const std::string& account_id,
                                                                  std::int64_t sid, bool delivered,
                                                                  const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) {
        return std::string("账号不在册: ") + channel_id + "/" + account_id;
    }
    return entry->ingress->Transition(
        sid, delivered ? IngressEventState::Delivered : IngressEventState::DeliveryFailed,
        reason);
}

std::optional<std::string> ChannelManager::DeadLetterIngress(const std::string& channel_id,
                                                             const std::string& account_id,
                                                             std::int64_t sid,
                                                             const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) {
        return std::string("账号不在册: ") + channel_id + "/" + account_id;
    }
    return entry->ingress->MoveToDeadLetter(sid, reason, options_.now_ms());
}

std::vector<ChannelIngressStore::Record> ChannelManager::IngressRecords(
    const std::string& channel_id, const std::string& account_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const AccountEntry* entry = Find(channel_id, account_id);
    if (entry == nullptr) {
        return {};
    }
    return entry->ingress->Records();
}

}  // namespace lubancode::channel
