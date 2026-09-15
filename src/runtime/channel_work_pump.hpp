// ChannelWorkPump(QQ 接入单 Q2 §六第六项):渠道消息接进 Session V3 与
// outbox 的总装泵——GatewayWorkPump 的渠道业务面,与 GatewayAutomationPump
// 经 CompositeGatewayPump 同 tick 公平推进(各自至多一枚执行/tick)。
//
// 提交次序链(§六,每个跨账窗口都有恢复判据):
//   ingress durable(OnInboundLocked,PowerLoss)
//   → 准入/路由冻结(TakeNextWork 现跑同一纯函数,重验不过就地 rejected)
//   → work claim(ingress queued -> running)
//   → SessionService 接纳(client_operation_id = 渠道域+账号+ingress sid,
//      同信重发命中台账不重跑)
//   → 绑定(work ledger 领域行先于 V3 gateway.work.bound)
//   → 执行(HeadlessExecutor::ExecuteChannelTurn——渠道会话的一轮,多轮
//      同场;重启经映射账 resume-as-new)
//   → reply selection(冻结策略纯函数,恢复路同款)
//   → outbox(拆段入箱,deliveryId 定式幂等)
//   → 投递(manager.SendReply → channel.send;回执/超时/重试分型结算)。
//
// 恢复裁决(每 tick 扫 Running 的 ingress 件;§六/contracts.md §5 矩阵):
//   - 绑定行在、V3 有 assistant、无 selection → 补 selection(不调模型);
//   - selection 已在 → 补 outbox 投影(同 deliveryId 幂等);
//   - 无 assistant(生成中断)/ 绑定行不在(claim 后崩)→ dead letter
//     needs_review,绝不盲重跑;
//   - 投递只补/重投已冻结回复,绝不重跑 Agent;发送失败不把已成功的
//     业务执行改成执行失败(ingress Replied 与投递态分开结算)。
//
// 单飞与帽:同 session 单飞(claim 与执行在泵内原子,同步模型下天然单飞;
// busy/并发帽作多线程泵的门),全局 max_active_channel_turns 帽;取件前
// 查帽,不先 claim 后拒(claim 了不跑会把件搁死在 Running)。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "channel/manager.hpp"
#include "channel/session_map.hpp"
#include "channel/work_ledger.hpp"
#include "gateway/automation_store.hpp"
#include "gateway/reply_outbox.hpp"
#include "gateway/work_pump.hpp"
#include "runtime/channel_automation.hpp"
#include "runtime/channel_interaction_broker.hpp"
#include "runtime/channel_media_service.hpp"
#include "runtime/headless_executor.hpp"
#include "workspace/identity.hpp"

namespace lubancode::hooks {
class HookDispatcher;
}
// 前置声明(不拉 backend/registry 重头;与 automation_pump 同款)。
namespace lubancode::api {
class Backend;
}
namespace lubancode::tools {
class ToolRegistry;
}

namespace lubancode::runtime {

class ChannelWorkPump final : public gateway::GatewayWorkPump {
public:
    struct Options {
        channel::ChannelManager* manager = nullptr;   // 借用(装配层保活)
        gateway::DurableReplyOutbox* outbox = nullptr;  // 借用(automation 泵
                                                        // 的同一本账——单写者)
        std::filesystem::path channels_state_root;    // 账号目录根(session
                                                       // map/work ledger 落位)
        std::filesystem::path workspaces_root;        // V3 会话持久化根
        workspace::WorkspaceIdentity workspace_identity;  // 冻结的工作目录身份
        std::string cwd_utf8;
        std::string lubancode_version;
        std::string wire_name;
        std::string model;
        channel::ToolRoutePolicy tools;  // 会话级基线(逐轮策略来自 route.tools)
        hooks::HookDispatcher* hook_dispatcher = nullptr;
        // 预算三根硬线(0 = 不设;生产装配应设)。
        int max_steps_per_turn = 0;
        int max_wall_secs = 0;
        std::int64_t max_total_tokens = 0;
        // 全局并发帽(多线程泵的门;同步泵一 tick 一轮,恒不越)。
        std::size_t max_active_channel_turns = 4;
        // 同 delivery 的发送尝试帽(限频退避重试;超过 → failed rate_limited)。
        int max_send_attempts = 3;
        std::int64_t send_retry_backoff_ms = 1000;
        // 渠道活场上限(透传执行器;0 = 不限)。
        std::size_t max_live_channel_sessions = 8;
        // ---- Q5:渠道身份建的定时任务(chapter 单 §十一) ----
        // automation 账借用(automation 泵的同一本账,单写者:Composite
        // 串行 tick 下两泵不同时碰账)。空 = automation 域不在:任务创建
        // 工具 fail closed,渠道任务不认领(本地任务仍归 automation 泵)。
        gateway::AutomationStore* automation_store = nullptr;
        // 聊天侧任务桥(ProcessWorkItem 冻结渠道上下文进 TurnScope)。
        std::shared_ptr<ChannelAutomationBridge> automation_bridge;
        // 被动回复窗(毫秒):文档页首 60 分钟与 msg_id 字段 5 分钟互相矛盾
        // (§11.3),按保守取窗下再留 1 分钟余量;真平台窗口归 Q3 实测校准。
        std::int64_t passive_reply_window_ms = 4 * 60 * 1000;
        // ---- Q4 媒体接纳(附件收发) ---------------------------------------
        // 下载 seam:装配层包渠道实现(生产 = qq 适配层的受控下载);
        // 空 = 渠道未装配下载,附件行如实报不可用,正文路照走。
        ChannelMediaDownloadFn media_download;
        ChannelMediaLimits media_limits;
        // 媒体仓根(空 = workspace 身份根下 channel-media/)。
        std::filesystem::path media_root;
        std::function<std::int64_t()> now_ms;  // 空 = wall clock
        // 故障注入(测试专用;生产恒空):executor 两窗(生成后/选择提交后)
        // + 泵自己的窗(入 outbox 后、发送前)。
        std::function<std::string(HeadlessExecutor::Options::FaultPoint)> fault_injection;
        std::function<std::string()> fault_after_enqueue;
        // ---- Q6 远端审批(QQ 按钮批准一次工具调用) --------------------------
        // 审批 broker(平台中立件;空 = 审批带工具照 Q0 fail closed 拒,
        // 行为零变化)。须确认工具在 tools.approve 带内时经它发卡等按钮:
        // 裁决/超时(默认拒绝)/幂等/身份复核都在 broker。
        std::shared_ptr<ChannelInteractionBroker> interaction_broker;
        // 审批窗(毫秒):到期无人答复按拒绝收口(沿 Web 面同款政策,
        // 不默认放行)。
        std::int64_t approval_timeout_ms = 300'000;
        // 渠道 turn 执行线程数(§12.2 第九行:等按钮的线程不能是唯一收
        // 按钮线程)。0 = 同步 tick(今日行为,测试零变化);>0 = 起专用
        // 工作线程跑 turn,tick 线程只推进事件/投递/恢复——审批等待期间
        // QQ 心跳、新来信、控制命令照常运转。生产装配应 ≥1。
        std::size_t channel_turn_workers = 0;
    };

    struct OpenResult {
        bool ok = false;
        std::string error;
    };
    static OpenResult Open(ChannelWorkPump* out, api::Backend& backend,
                           tools::ToolRegistry& registry, Options options);

    ChannelWorkPump();
    ~ChannelWorkPump() override;
    ChannelWorkPump(ChannelWorkPump&&) = delete;
    ChannelWorkPump(const ChannelWorkPump&) = delete;
    ChannelWorkPump& operator=(const ChannelWorkPump&) = delete;

    // ---- GatewayWorkPump 合同 ------------------------------------------------
    bool TickOnce(std::int64_t now_ms) override;
    void StopAccepting() override;
    bool Close(int grace_ms) override;
    void set_owner_epoch(const std::string& epoch) override { owner_epoch_ = epoch; }

    // ---- 观测/测试 -----------------------------------------------------------
    bool accepting() const { return accepting_.load(); }
    // 映射账查询(direct 会话):该会话键在本 workspace 下映射到的 V3 场。
    // 空 = 无映射(还没跑过/别的 workspace 的场)。
    std::string session_id_for(const std::string& channel_id, const std::string& account_id,
                               const std::string& conversation_id) const;
    // 映射账查询(Q5 渠道任务的隔离场):任务自己的场,不进聊天会话。
    std::string job_session_id_for(const std::string& channel_id, const std::string& account_id,
                                   const std::string& job_id) const;
    // 渠道域幂等键(§六第五项):渠道+账号+ingress 身份稳定生成;同信重发
    // 同键(SessionService 台账命中回原受理,不重跑模型)。
    static std::string MakeChannelOperationId(const std::string& channel_id,
                                              const std::string& account_id,
                                              std::int64_t ingress_sid);
    // Q5 渠道任务隔离场的 session key 定式("channel:<ch>:<acct>:job:<jobId>"
    // ——账号段与聊天会话同构,账随账号走;kind=job 不与 direct/group 撞)。
    static std::string MakeChannelJobSessionKey(const std::string& channel_id,
                                                const std::string& account_id,
                                                const std::string& job_id);

private:
    // 账号级账套(session map + work ledger;首用懒开)。
    struct AccountBooks {
        channel::ChannelSessionMap session_map;
        channel::ChannelWorkLedger work_ledger;
    };
    AccountBooks* BooksFor(const std::string& channel_id, const std::string& account_id);
    AccountBooks* BooksForSessionKey(const std::string& session_key);
    // Q4 附件接纳(准入通过、执行前):下载落仓 + 模型投影行。media_service
    // 未开 = 空投影(附件行由 turn_ingress 的占位说明承担)。
    std::string IngestAttachmentsPrompt(const channel::ChannelManager::WorkItem& work);
    // Q4 产物附件:正文拆段 > 1 时末段附带冻结正文原件(任务结果文件,
    // §十 Q4 最保守路——任务结果的附件随最终回复投递)。
    std::optional<gateway::DurableReplyOutbox::ChannelAttachment> ReplyFileAttachment(
        const std::string& selection_id, const std::string& reply_text) const;

    bool ApplyDeliveryOutcomes(std::int64_t now_ms);   // 回执 → outbox 推进
    void ReconcileDeliveredSources(std::int64_t now_ms);  // 终态项 → ingress 结算
    void PumpPairingNotices(std::int64_t now_ms);      // Q1b:配对提示入 outbox
    bool SweepRecovery(std::int64_t now_ms);           // Running 件的跨账裁决
    bool RecoverOne(const std::string& channel_id, const std::string& account_id,
                    const channel::ChannelManager::IngressRunningView& view,
                    std::int64_t now_ms);
    bool RunOneChannelTurn(std::int64_t now_ms);       // 至多一轮新执行
    bool ProcessWorkItem(const std::string& channel_id, const std::string& account_id,
                         const channel::ChannelManager::WorkItem& work, std::int64_t now_ms,
                         const std::string& turn_key, const std::atomic<bool>* cancel);
    // Q6 远端审批:per-turn 确认回调的裁定(显式 allow 放行/hard deny 拒/
    // 审批带发卡等按钮/带外 fail closed 拒)。
    HeadlessExecutor::Options::ToolConfirmDecision DecideChannelToolApproval(
        const std::string& turn_key, const channel::ToolRoutePolicy& tools,
        const std::string& tool_use_id, const std::string& name, const nlohmann::json& input,
        const std::string& channel_id, const std::string& account_id,
        const std::string& conversation_id, const std::string& session_key,
        const std::string& sender_id, const std::string& message_id,
        std::int64_t received_at_ms, const std::atomic<bool>* cancel);
    // Q6:审批卡入交互 outbox(有期限;重试不突破审批 TTL);驱动与回执
    // 结算见 DriveApprovalFlow。
    void EnqueueApprovalCard(const ChannelInteractionBroker::RequestedFact& fact);
    bool DriveApprovalFlow(std::int64_t now_ms);  // 回调裁决+ack;卡片发送/重试
    // 审批回执结算(ApplyDeliveryOutcomes 查不到 outbox 项时对账卡片)。
    // 返回 true = 这笔回执是审批卡片的(已消化)。
    bool SettleApprovalCardOutcome(const std::string& client_delivery_id,
                                   channel::ChannelManager::ChannelDeliveryOutcome::Status status,
                                   const std::string& error_code, std::int64_t now_ms);
    // Q6:turn 收场后把审批流水落进该场 V3(channel.approval.requested/
    // resolved;writer 空闲窗口写,同 session 单飞保证无并发)。
    void WriteApprovalFactsToV3(const std::string& session_id, const std::string& turn_id,
                                const std::string& turn_key);
    // ---- Q5:渠道任务的认领/执行/恢复/补投 -----------------------------------
    bool SweepChannelJobRecovery(std::int64_t now_ms);  // claimed 未结算的跨账裁决
    bool RecoverChannelJobOccurrence(const gateway::AutomationOccurrence& occurrence,
                                     std::int64_t now_ms);
    bool RunOneChannelJob(std::int64_t now_ms, std::string* error);  // 至多一枚新执行
    bool DriveChannelDeliveries(std::int64_t now_ms);  // 渠道段发送/重试
    // 源(ingress sid)的全部段是否终态:全 sent → true(delivered);有
    // failed/unknown → false;未齐 → nullopt(等)。
    std::optional<bool> SourceDeliveryVerdict(const std::string& source_ref) const;
    // Q5 渠道任务段的发送锚:该会话最近一来信在被动回复窗内 → 其 msg_id
    //(补投锚,被动回复);否则 nullopt(主动消息,不带陈旧锚)。
    std::optional<std::string> FreshInboundAnchor(const gateway::ReplyOutboxItem& item,
                                                  std::int64_t now_ms) const;

    api::Backend* backend_ = nullptr;
    tools::ToolRegistry* registry_ = nullptr;
    Options options_;
    std::optional<HeadlessExecutor> executor_;
    std::optional<ChannelMediaService> media_service_;  // Q4 附件接纳仓
    std::atomic<bool> accepting_{true};
    std::atomic<bool> closed_{false};
    std::string owner_epoch_;
    std::mutex books_mutex_;
    std::map<std::string, std::unique_ptr<AccountBooks>> books_;
    // 限频重试的节流账(delivery_id → 下一可试时刻;进程内,重启即清——
    // 退避是建议值,不是正确性账)。
    std::map<std::string, std::int64_t> retry_at_;
    // ---- Q5 渠道任务的补投账(进程内,重启即清) ----
    // 最近一来信锚("<ch>/<acct>/<conv>" → msg_id + 时刻):渠道任务段的
    // 被动回复窗判定原料。重启清空 = 首投按主动消息走,窗判定重新累积。
    // Q6 异步 turn 起,写在工作线程(ProcessWorkItem)、读在 tick
    //(FreshInboundAnchor)——过锁。
    struct RecentInbound {
        std::string message_id;
        std::int64_t received_at_ms = 0;
    };
    mutable std::mutex recent_inbound_mutex_;
    std::map<std::string, RecentInbound> recent_inbound_;
    // 挂起等互动的渠道任务段(主动额度受限/回复窗过期被拒):不硬发不谎报,
    // 下一封来信进窗后锚定补投(§11.3)。重启清空 = 重启后主动重试一次,
    // 再拒再挂——有界,不刷屏。
    std::set<std::string> await_interaction_;
    std::size_t last_account_index_ = 0;  // 账号间轮转公平
    std::size_t active_turns_ = 0;        // 全局并发帽的门(多线程泵)
    // 本进程在跑的轮(sweep 跳过;多线程泵的门)。键 "<ch>/<acct>/<sid>"。
    // Q6 异步 turn 起,这组账(turn_jobs_mutex_)由 tick 线程与工作线程
    // 共用,读写都过锁。
    std::set<std::string> in_flight_sids_;
    // 故障注入窗 3:入箱后、发送前死——本 tick 不驱动投递(恢复路接管)。
    bool suppress_delivery_ = false;
    // ---- Q6:异步 turn 执行线程(channel_turn_workers>0 时) ----------------
    struct TurnJob {
        std::string channel_id;
        std::string account_id;
        std::string turn_key;
        channel::ChannelManager::WorkItem work;
        std::int64_t now_ms = 0;
        std::shared_ptr<std::atomic<bool>> cancel;  // Close/停机打断审批等待
    };
    std::mutex turn_jobs_mutex_;
    std::condition_variable turn_jobs_wake_;
    std::deque<TurnJob> turn_jobs_;
    std::vector<std::shared_ptr<std::atomic<bool>>> inflight_cancels_;
    std::vector<std::thread> turn_workers_;
    std::atomic<bool> turn_workers_stop_{false};
    void TurnWorkerLoop();
    void ShutDownTurnWorkers();  // 打断在飞、退回未跑、join(Close/析构共用)
    // ---- Q6:审批卡片的交互 outbox(进程内;重启后旧请求作废不补投) ----
    // deadline 即审批 TTL:卡片投递失败在窗内退避重试,过线取消等待
    //(fail closed,不为重试卡片突破审批窗)。
    struct ApprovalCard {
        std::string token;
        std::string delivery_id;  // manager send 的 client_delivery_id(回执对账)
        std::string channel_id;
        std::string account_id;
        std::string conversation_id;
        std::string reply_to_message_id;
        std::string markdown;      // 卡片正文(脱敏摘要)
        nlohmann::json keyboard;   // 冻结的键盘载荷
        std::int64_t deadline_ms = 0;
        std::int64_t retry_at_ms = 0;
        bool inflight = false;  // 已递 manager,等回执
        bool done = false;      // 终态(已投递/已失败收口)
    };
    std::mutex approval_cards_mutex_;
    std::vector<ApprovalCard> approval_cards_;
};

}  // namespace lubancode::runtime
