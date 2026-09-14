// ChannelManager:多账号生命周期与入站水路(多渠道消息接入单阶段 2)。
//
// 唯一真源 docs/architecture/channels/README.md §4(总架构)、
// bridge-protocol.md §3-7(握手/method/关闭顺序)、configuration.md
// §9-11(状态机/退避/账号锁)。本批落地 TODO 阶段 2 的七件:
//   账号状态机+generation+退避+锁 / ingress journal+replay+去重 /
//   每账号每会话队列+背压 / pairing / IdleWake 接线。
//
// 形态边界(重要):本批不 spawn 真进程——bridge 的字节面经 Transport
// 抽象注入(测试挂 FakeChannelSidecar,见 tests/support/fake_channel_sidecar)。
// 同步泵模型:Pump() 把出站帧写给 sidecar、收回字节解帧入账。真进程
// stdio 与进程树收尾是阶段 5 bridge_process 的事,届时 Transport 换真
// 实现,manager 的状态机与账不动。
//
// 唤醒接线:channel 库住 lubancode_engine,不能反向 include runtime 的
// IdleWakeCoordinator(engine <- runtime 依赖方向)。故只认
// ChannelWakeCoordinator 小口,装配层(runtime/app 侧)拿真
// IdleWakeCoordinator 适配。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/account_lock.hpp"
#include "channel/account_state.hpp"
#include "channel/channel_config.hpp"
#include "channel/channel_router.hpp"
#include "channel/frame.hpp"
#include "channel/ingress_store.hpp"
#include "channel/inbox.hpp"
#include "channel/pairing.hpp"
#include "channel/router.hpp"

namespace lubancode::channel {

// 唤醒口:manager 每个活跃账号挂一枚源(ready = 该账号有活要让位),
// idle 摘源。实现方持 token 的存活。
class ChannelWakeCoordinator {
public:
    virtual ~ChannelWakeCoordinator() = default;
    virtual void SetAccountWakeSource(const std::string& channel_id, const std::string& account_id,
                                      std::function<bool()> ready) = 0;
    virtual void ClearAccountWakeSource(const std::string& channel_id,
                                        const std::string& account_id) = 0;
};

// sidecar 字节面。WriteToSidecar 把编码好的帧递给 adapter;DrainFromSidecar
// 取回 adapter 攒下的应答字节(同步假件立即有;真进程由 IO 线程积攒)。
class ChannelBridgeTransport {
public:
    virtual ~ChannelBridgeTransport() = default;
    virtual void WriteToSidecar(const std::byte* data, std::size_t size) = 0;
    virtual std::vector<std::byte> DrainFromSidecar() = 0;
};

struct ChannelManagerOptions {
    // 账号状态根(默认 ~/.lubancode/channels,由装配层给;测试注入临时目录)。
    std::filesystem::path state_root;
    // 唤醒口(可空 = 不接 idle wake,测试装配可不传)。
    ChannelWakeCoordinator* wake = nullptr;
    // 进程存活检查(默认 platform::IsProcessAlive;测试注入)。
    AccountLock::AliveChecker alive_checker;
    // 时钟(默认取 wall clock;测试注入固定/步进钟)。
    std::function<std::int64_t()> now_ms;
    // 队列水位(默认 InboxLimits;测试可压小)。
    InboxLimits inbox_limits;
    // channel.send 回执超时(QQ 接入单 Q2 §七:超时 = delivery_unknown,
    // 停自动重发,不虚 exactly-once)。测试注入小值。
    std::int64_t send_timeout_ms = 30'000;
};

class ChannelManager {
public:
    explicit ChannelManager(ChannelManagerOptions options);
    ~ChannelManager();

    ChannelManager(const ChannelManager&) = delete;
    ChannelManager& operator=(const ChannelManager&) = delete;

    // ---- 账号注册与生命周期 ----

    struct AddAccountResult {
        enum class Status {
            Ok,
            InvalidArgument,  // channel/account id 不合法(拼得出根外路径)
            LockRefused,   // 活进程持有(account_in_use)或假锁看不懂
            ReplayError,   // ingress 账打开失败(账损坏不是拒绝,是明错)
            IoError,       // 目录/账建不了
        };
        Status status = Status::IoError;
        std::string detail;
        AccountLockRecord lock_holder;  // LockRefused 时 = 活着的持有者
    };

    // 注册一只账号:建状态目录、replay ingress 账、取账号锁、挂 wake 源。
    // transport 借用(须活过账号生命周期;可空 = 只入账不跑桥,用于纯观测)。
    AddAccountResult AddAccount(const std::string& channel_id, const std::string& account_id,
                                const ChannelAccountUserConfig& config,
                                ChannelBridgeTransport* transport);

    // 渠道层 bindings(阶段 3 路由批,configuration.md §8):按渠道注入,
    // AddAccount 前后都可设;路由每次现读,改完即生效(新 turn 用新账)。
    void SetChannelBindings(const std::string& channel_id,
                            std::vector<ChannelBindingConfig> bindings);
    // 渠道层工具上限(QQ 接入单 Q0;五层交集的渠道层):按渠道注入,
    // 路由每次现读。空 allow/deny 的缺省份会覆盖旧值(收窄可撤,显式
    // 撤销也是配置)。
    void SetChannelToolsPolicy(const std::string& channel_id, ChannelToolsUserPolicy tools);
    // 账号运行状态的盘上快照(Q2:状态迁移时写 account-status.json,
    // 进程外的只读探测面读——gateway status 渠道栏不带密钥与整份平台
    // 事件)。零写盘;文件不在给 present=false。
    struct ChannelAccountStatusFile {
        bool present = false;
        std::string channel_id;
        std::string account_id;
        std::string state;
        int generation = 0;
        std::int64_t updated_at_ms = 0;
        std::string last_reason;
        std::string last_detail;
    };
    static ChannelAccountStatusFile ReadAccountStatusFile(const std::filesystem::path& account_dir);

    // 起账号:Disabled -> Validating -> Starting,发 channel.initialize。
    // 后续推进靠 Pump()(收到 initialize result 发 start,收到 start result
    // 入 Running)。
    std::optional<std::string> StartAccount(const std::string& channel_id,
                                            const std::string& account_id);
    // 停账号:发 channel.stop,收到 result 走 Stopping -> Stopped,释放锁。
    // 析构走同一条路(幂等,bridge-protocol.md §7)。
    std::optional<std::string> StopAccount(const std::string& channel_id,
                                           const std::string& account_id);
    std::optional<std::string> RestartAccount(const std::string& channel_id,
                                              const std::string& account_id);

    // ---- 桥泵(同步模型;阶段 5 真进程换成 IO 线程驱动同一入口) ----

    // 出站帧写给 sidecar + 收回字节处理。一步一泵,状态机前进一步。
    void Pump(const std::string& channel_id, const std::string& account_id);
    // 只收字节(测试把 sidecar 的主动通知喂回来)。
    void HandleBytesFromSidecar(const std::string& channel_id, const std::string& account_id,
                                const std::byte* data, std::size_t size);

    // ---- 故障通知(Transport 层/装配层报上来) ----

    // 传输层故障:状态按当前值进 Degraded/Backoff,记退避账(不可自愈的
    // reason 直落不可恢复终态)。
    void NotifyTransportFailure(const std::string& channel_id, const std::string& account_id,
                                const std::string& reason, const std::string& detail);
    // 稳定运行通知:退避计数归零(account_state.md"成功稳定运行一段后归零")。
    void NotifyStableRunning(const std::string& channel_id, const std::string& account_id);

    // ---- 观测(命令面/测试) ----

    struct AccountSnapshot {
        std::string channel_id;
        std::string account_id;
        ChannelAccountState state = ChannelAccountState::Disabled;
        int generation = 1;
        std::int64_t running_since_ms = 0;
        std::int64_t retry_at_ms = 0;          // Backoff 时的下一次尝试
        int backoff_attempt = 0;
        std::vector<AccountStatusTransition> recent_transitions;  // 最近 8 笔
        std::size_t inbox_pending = 0;
        std::map<std::string, std::size_t> ingress_state_counts;
        std::size_t dead_letter_count = 0;
        std::size_t pairing_pending = 0;
        std::size_t pairing_approved = 0;
        bool lock_held = false;
        DmPolicy dm_policy = DmPolicy::Pairing;
        GroupPolicy group_policy = GroupPolicy::Allowlist;
        CredentialSource credential = CredentialSource::Missing;
    };

    std::vector<AccountSnapshot> Snapshots() const;
    std::optional<AccountSnapshot> Snapshot(const std::string& channel_id,
                                            const std::string& account_id) const;
    // wake 源的 ready 判定:该账号 inbox 有待处理事件。
    bool HasPendingWork(const std::string& channel_id, const std::string& account_id) const;
    std::size_t account_count() const;

    // ---- 入站账的直取口(阶段 3 路由/turn 泵从这里拿活) ----

    // 从 inbox 取下一件待办(公平轮转),并推进 ingress 状态 queued ->
    // running(取走即视作开跑;阶段 3 的 scheduler 接手后此口退役)。
    struct WorkItem {
        std::int64_t sid = 0;
        std::string conversation_id;
        std::string sender_id;
        ChannelInboundEvent event;
        // 路由决策(阶段 3 ChannelRouter):session_key/agent/工具与记忆
        // 策略/provenance。准入时已判过 Admitted;这里现跑同一只纯函数
        // 取全账(同样的输入同样的决策,不另存第二份真账)。这就是本轮
        // 执行的冻结策略版本——权限撤销后重验不过的输入到不了这里。
        RouteDecision route;
    };
    std::optional<WorkItem> TakeNextWork(const std::string& channel_id,
                                         const std::string& account_id);

    // ---- 出站投递(channel.send 的宿主口;QQ 接入单 Q2 §七) -----------
    // 发送受理:冻结正文按 conversation 直发,client_delivery_id 是 outbox
    // 的 delivery id(桥协议 client_id——适配器按它稳定 msg_seq,同
    // delivery 重试同载荷)。受理即编码写给 sidecar(同步面),回执异步:
    // 泵侧 DrainChannelDeliveryOutcomes 收账。账号非 Running / transport
    // 缺 / 正文空 → 拒(错误串)。
    struct ChannelSendRequest {
        std::string conversation_id;        // direct 会话 openid
        std::string text;                   // 冻结正文(单段;拆段归 outbox)
        std::string reply_to_message_id;    // 被动回复锚(空 = 主动消息)
        std::string client_delivery_id;     // 稳定发送身份(outbox delivery id)
    };
    std::optional<std::string> SendReply(const std::string& channel_id,
                                         const std::string& account_id,
                                         const ChannelSendRequest& request);

    // 一笔回执的结算账(泵消费后推进 outbox/ingress)。
    struct ChannelDeliveryOutcome {
        enum class Status {
            Accepted,      // 平台已接受(provider_message_id 有值)
            RateLimited,   // 限频/暂态失败:可退避重试(同 delivery_id 同载荷)
            Rejected,      // 平台明确拒绝(回复窗口过期/内容拒绝/无好友/拒收)
            AuthFailed,    // 令牌失效(账号已转 NeedsLogin)
            Unknown,       // 超时无回执:delivery_unknown,停自动重发
        };
        std::string client_delivery_id;
        Status status = Status::Unknown;
        std::string provider_message_id;
        std::string error_code;   // 稳定码:rate_limited|platform_reject|
                                  // reply_window_expired|auth_failed|delivery_unknown
        std::string detail;       // 脱敏细节(不带密钥/整份平台事件)
        std::int64_t settled_at_ms = 0;
        int generation = 0;       // 结算时的账号代次(陈旧回执如实带出)
    };
    // 收走自上次调用以来的全部回执结算(FIFO;同 delivery 重复回执只结
    // 一次——首笔为准,后续按重复丢弃)。代次隔离:受理时记的代次与结
    // 算时不符 = 陈旧回执,不产出结算(留诊断)。
    std::vector<ChannelDeliveryOutcome> DrainChannelDeliveryOutcomes(const std::string& channel_id,
                                                                     const std::string& account_id);
    // 该 delivery 是否有在途 send(泵的崩溃窗口裁决:账上 sending 而桥上
    // 无在途 = 发出请求丢了,可重驱动)。
    bool HasPendingSend(const std::string& channel_id, const std::string& account_id,
                        const std::string& client_delivery_id) const;

    // ---- 渠道 work 泵的恢复/结算面(Q2 §六) ------------------------------
    // 仍在 Running(claim 后未结算)的入站件——恢复扫描的输入。
    struct IngressRunningView {
        std::int64_t sid = 0;
        std::string conversation_id;
        ChannelInboundEvent event;
    };
    std::vector<IngressRunningView> ListRunningIngress(const std::string& channel_id,
                                                       const std::string& account_id) const;
    // 状态推进窄口(泵侧结算;迁移合法性由 ingress 状态机把关,非法回错)。
    std::optional<std::string> SettleIngressReplied(const std::string& channel_id,
                                                    const std::string& account_id,
                                                    std::int64_t sid);
    std::optional<std::string> SettleIngressDelivered(const std::string& channel_id,
                                                      const std::string& account_id,
                                                      std::int64_t sid, bool delivered,
                                                      const std::string& reason);
    std::optional<std::string> DeadLetterIngress(const std::string& channel_id,
                                                 const std::string& account_id, std::int64_t sid,
                                                 const std::string& reason);
    // 入站账的只读快照(观测/测试)。
    std::vector<ChannelIngressStore::Record> IngressRecords(const std::string& channel_id,
                                                            const std::string& account_id) const;

    // ---- pairing 账的口子(阶段 3 命令面:/channel pairing list/approve/reject) ----

    struct PendingPairingView {
        std::string sender_id;
        std::int64_t expires_at_ms = 0;
    };
    std::vector<PendingPairingView> PendingPairings(const std::string& channel_id,
                                                    const std::string& account_id) const;
    // 批准/拒绝。成功返回被批准/拒绝的 sender id;code 不认、过期、已处理
    // 报错(stable reason:not_found/expired/already_finalized)。
    std::optional<std::string> ApprovePairing(const std::string& channel_id,
                                              const std::string& account_id, const std::string& code,
                                              std::string* error = nullptr);
    std::optional<std::string> RejectPairing(const std::string& channel_id,
                                             const std::string& account_id, const std::string& code,
                                             std::string* error = nullptr);

private:
    // 在途 channel.send(受理即记;回执/超时结算后销账)。
    struct PendingChannelSend {
        std::int64_t request_id = 0;
        std::string client_delivery_id;
        std::string conversation_id;
        int generation = 0;             // 受理时账号代次(代次隔离)
        std::int64_t sent_at_ms = 0;
    };
    struct AccountEntry {
        std::string channel_id;
        std::string account_id;
        ChannelAccountUserConfig config;
        ChannelBridgeTransport* transport = nullptr;  // 借用
        BridgeRouter router;                          // host 侧桥
        FrameDecoder decoder;
        ChannelAccountState state = ChannelAccountState::Disabled;
        int generation = 1;
        std::int64_t running_since_ms = 0;
        std::int64_t retry_at_ms = 0;
        int backoff_attempt = 0;
        std::int64_t initialize_request_id = 0;
        std::int64_t start_request_id = 0;
        std::int64_t stop_request_id = 0;
        AccountLock lock;
        // mutex 件不落值语义:三本账都持指针,AddAccount 里工厂开账。
        std::unique_ptr<ChannelIngressStore> ingress;
        std::unique_ptr<ChannelInbox> inbox;
        std::unique_ptr<PairingStore> pairing;
        std::vector<AccountStatusTransition> transitions;
        // 出站投递账(Q2 §七):在途 send 与已结算回执(泵收走即清)。
        std::map<std::int64_t, PendingChannelSend> pending_sends;
        std::vector<ChannelDeliveryOutcome> delivery_outcomes;
        // 已结算过的 delivery(重复回执只结一次)。
        std::map<std::string, int> settled_deliveries;
        std::vector<std::string> send_diagnostics;
    };

    AccountEntry* Find(const std::string& channel_id, const std::string& account_id);
    const AccountEntry* Find(const std::string& channel_id,
                             const std::string& account_id) const;

    // 状态机推进:合法性过不了报错(账上留下非法尝试的说明)。
    std::optional<std::string> TransitionLocked(AccountEntry& entry, ChannelAccountState to,
                                                const std::string& reason,
                                                const std::string& detail);
    // 状态迁移的盘上快照(Q2:account-status.json,原子写,只读探测面)。
    void PersistAccountStatusLocked(const AccountEntry& entry);
    void FlushOutboundLocked(AccountEntry& entry);
    // 以下两个 *Locked:调用方已持 mutex_(公有口拿锁后转内部,防递归死锁)。
    void HandleBytesFromSidecarLocked(AccountEntry& entry, const std::byte* data,
                                      std::size_t size);
    void NotifyTransportFailureLocked(AccountEntry& entry, const std::string& reason,
                                      const std::string& detail);
    void HandleMessageLocked(AccountEntry& entry, const IncomingMessage& message);
    // channel.inbound 的处理:去重落账 -> ack -> 路由准入 -> inbox/背压。
    void OnInboundLocked(AccountEntry& entry, const ChannelInboundEvent& event);
    // ---- 出站投递(Q2 §七) ----
    // 回执结算:推送 outcome 进 delivery_outcomes(调用方已持 mutex_)。
    void SettleSendLocked(AccountEntry& entry, std::int64_t request_id,
                          ChannelDeliveryOutcome::Status status,
                          const std::string& provider_message_id, const std::string& error_code,
                          const std::string& detail);
    // channel.send 错误应答的分型(限频/令牌失效/窗口过期/平台拒绝)。
    void ClassifySendErrorLocked(AccountEntry& entry, std::int64_t request_id,
                                 const std::string& stable_name, const nlohmann::json& error_data);
    // delivery.receipt 通知的结算(按 outbound_delivery_id 关联;重复/陈旧
    // 只留诊断)。
    void OnDeliveryReceiptLocked(AccountEntry& entry, const nlohmann::json& params);
    // 在途 send 的超时裁决(delivery_unknown)。
    void ExpireStaleSendsLocked(AccountEntry& entry);
    // 路由准入(阶段 3):ChannelRouter 全账,pairing 账经 PairingStore 适配。
    // 调用方已持 mutex_。
    RouteDecision RouteInboundLocked(AccountEntry& entry, const ChannelInboundEvent& event);
    AccountSnapshot SnapshotLocked(const AccountEntry& entry) const;

    ChannelManagerOptions options_;
    std::string instance_token_;  // 进锁账:同进程多 manager 互不相认
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<AccountEntry>> accounts_;
    // 渠道层 bindings(§8):channel_id -> bindings。
    std::map<std::string, std::vector<ChannelBindingConfig>> channel_bindings_;
    // 渠道层工具上限(QQ 接入单 Q0):channel_id -> tools policy。
    std::map<std::string, ChannelToolsUserPolicy> channel_tools_;
};

}  // namespace lubancode::channel
