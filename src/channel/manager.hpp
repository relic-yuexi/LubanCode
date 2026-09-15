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
#include <set>
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

// 渠道账号状态根:<状态根>/channels(应用根语义下状态根即数据根,个人
// 布局=~/.lubancode/channels 原样)。ingress 账、account-status.json、
// sessions 映射、work 账、pairing、账号锁、qq spool 等运行状态全落这棵
// 树——全是"可重建/随会话走"的状态,归数据根(应用Worker接入单 §4.2、
// 合同 capability-contract.md §13.2)。装配层与受保护路径闸
// (tool_guard)都从这里取,不各拼各的。无根可用(应用根变量坏/无主
// 目录)返回空 path,调用方明报不回落个人目录。
std::filesystem::path DefaultChannelsStateRoot();

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
        // Q7 菜单/面板回调的命令绑定(账号配置的快照,随件冻结):泵侧
        // 识别菜单填入文本后走宿主分派。不命中命令表的输入不受影响。
        std::vector<ChannelCommandBindingUserConfig> commands;
    };
    std::optional<WorkItem> TakeNextWork(const std::string& channel_id,
                                         const std::string& account_id);

    // 只读路由探针(Q5:渠道任务执行前重验创建者准入):按 (渠道,账号,
    // 会话,sender) 现跑同一只 ChannelRouter 纯函数——配对撤销/allowlist
    // 移除/策略收窄都如实报,不落 ingress 账、不发配对提示(未知 sender
    // 回 PendingPairing 且 code 为空)。查无账号给 nullopt。
    RouteDecision ProbeRoute(const std::string& channel_id, const std::string& account_id,
                             const ChannelConversation& conversation,
                             const std::string& sender_id, std::int64_t now_ms) const;

    // ---- 出站投递(channel.send 的宿主口;QQ 接入单 Q2 §七) -----------
    // 发送受理:冻结正文按 conversation 直发,client_delivery_id 是 outbox
    // 的 delivery id(桥协议 client_id——适配器按它稳定 msg_seq,同
    // delivery 重试同载荷)。受理即编码写给 sidecar(同步面),回执异步:
    // 泵侧 DrainChannelDeliveryOutcomes 收账。账号非 Running / transport
    // 缺 / 正文与附件全空 → 拒(错误串)。Q4:可带一枚出站附件(纯附件
    // 回复也是合法发送——§十 10.2"没有文字、只有一个文件也算有效回复")。
    struct OutboundAttachment {
        std::string local_path;   // 宿主侧 UTF-8 路径(冻结产物原件)
        std::string file_name;    // 展示名(入箱时净化)
        std::string mime_type;
        std::int64_t size_bytes = 0;
    };
    struct ChannelSendRequest {
        std::string conversation_id;        // direct 会话 openid
        std::string text;                   // 冻结正文(单段;拆段归 outbox)
        std::string reply_to_message_id;    // 被动回复锚(空 = 主动消息)
        std::string client_delivery_id;     // 稳定发送身份(outbox delivery id)
        std::optional<OutboundAttachment> attachment;  // Q4 出站附件(可空)
        // Q6 审批卡片:非空时作为 keyboard 随消息发给适配器(空 object =
        // 纯文本,行为不变)。适配器无 interaction 能力会按协议拒绝——
        // 调用方(审批泵)按失败收口,不静默降级。
        nlohmann::json keyboard = nlohmann::json::object();
    };
    std::optional<std::string> SendReply(const std::string& channel_id,
                                         const std::string& account_id,
                                         const ChannelSendRequest& request);

    // ---- 互动回调(Q6 远端审批;sidecar 的 channel.interaction.create) ---
    // 适配器上报的按钮回调(已过 application_id 对账)。
    struct ChannelInteraction {
        std::string channel_id;
        std::string account_id;
        std::string delivery_id;        // 适配器侧事件身份(诊断)
        std::string interaction_id;     // 回应接口的路径参数
        std::int64_t type = 0;          // 11=消息按钮;其余类型宿主不裁决
        std::string scene;              // c2c/group/guild
        std::int64_t chat_type = -1;
        std::string operator_id;        // 单聊=user_openid;群聊=group_member_openid
        std::string button_data;        // 宿主发的 opaque token(裁决原料)
        std::string button_id;
        std::int64_t received_at_ms = 0;
    };
    // 收走自上次调用以来的全部互动回调(FIFO)。泵消费后裁决并回 AckInteraction。
    std::vector<ChannelInteraction> DrainChannelInteractions(const std::string& channel_id,
                                                             const std::string& account_id);
    // 回应平台(PUT /interactions/{id};code 官方口径 0 成功/1 失败/2 频繁
    // /3 重复/4 没权限/5 仅管理员)。受理即入队,结果异步留账;同
    // interaction_id 只能回应一次(官方限制),调用方自律不重发。
    // 返回空串 = 受理;非空 = 拒收理由(账号非 Running 等)。
    std::string AckInteraction(const std::string& channel_id, const std::string& account_id,
                               const std::string& interaction_id, int code);

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
    // 已批准的配对 sender 清单(Q7:c2c specific 面板按已配对用户关联/
    // 撤销时移除的目标账)。查无账号给空表。
    std::vector<std::string> ApprovedPairingSenders(const std::string& channel_id,
                                                    const std::string& account_id) const;
    // 批准/拒绝。成功返回被批准/拒绝的 sender id;code 不认、过期、已处理
    // 报错(stable reason:not_found/expired/already_finalized)。
    std::optional<std::string> ApprovePairing(const std::string& channel_id,
                                              const std::string& account_id, const std::string& code,
                                              std::string* error = nullptr);
    std::optional<std::string> RejectPairing(const std::string& channel_id,
                                             const std::string& account_id, const std::string& code,
                                             std::string* error = nullptr);
    // 按 sender 身份批准/拒绝(Q1b 控制入口"配对码或身份"的身份路)。
    std::optional<std::string> ApprovePairingBySender(const std::string& channel_id,
                                                      const std::string& account_id,
                                                      const std::string& sender_id,
                                                      std::string* error = nullptr);
    std::optional<std::string> RejectPairingBySender(const std::string& channel_id,
                                                     const std::string& account_id,
                                                     const std::string& sender_id,
                                                     std::string* error = nullptr);

    // ---- 配对提示(Q1b:PendingPairing 来信的宿主回执) ---------------------
    // 一枚待投提示:入站裁决时冻结(会话/被动回复锚/code 明文只此一次)。
    // 投递走 Q2 既有链路(泵侧入 outbox 渠道段,不旁路)。
    struct PairingNotice {
        std::string conversation_id;       // 发给来者的会话
        std::string reply_to_message_id;   // 被动回复锚(触发来信的 msg id)
        std::string sender_id;             // 诊断/审计
        std::string code;                  // 配对码明文(出 manager 即进 outbox 冻结)
        std::string text;                  // 冻结正文(MakePairingNoticeText)
        std::int64_t trigger_sid = 0;      // 触发来信的 ingress sid(审计)
    };
    // 取走全部待投提示(FIFO;泵每 tick 排水)。崩溃窗口如实记:入队前
    // 进程死,提示丢失(限频账已记,冷却窗内不重发)——下一条来信重触发。
    std::vector<PairingNotice> DrainPendingPairingNotices(const std::string& channel_id,
                                                          const std::string& account_id);

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
        // 互动回调(Q6):sidecar 上报的按钮事件,泵收走即清(FIFO)。
        std::vector<ChannelInteraction> interactions;
        // 已回应过的 interaction(官方:同一 id 只能回应一次;重复上报
        // 不再受理回应,幂等留账)。
        std::set<std::string> acked_interactions;
        // 待投配对提示(Q1b):PendingPairing 裁决时入队,泵排水进 outbox。
        std::vector<PairingNotice> pending_pairing_notices;
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
    // channel.interaction.create 通知(Q6):解析入 interactions 队列。
    void OnInteractionLocked(AccountEntry& entry, const nlohmann::json& params);
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
