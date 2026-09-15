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
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "channel/manager.hpp"
#include "channel/session_map.hpp"
#include "channel/work_ledger.hpp"
#include "gateway/reply_outbox.hpp"
#include "gateway/work_pump.hpp"
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
        std::function<std::int64_t()> now_ms;  // 空 = wall clock
        // 故障注入(测试专用;生产恒空):executor 两窗(生成后/选择提交后)
        // + 泵自己的窗(入 outbox 后、发送前)。
        std::function<std::string(HeadlessExecutor::Options::FaultPoint)> fault_injection;
        std::function<std::string()> fault_after_enqueue;
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
    // 渠道域幂等键(§六第五项):渠道+账号+ingress 身份稳定生成;同信重发
    // 同键(SessionService 台账命中回原受理,不重跑模型)。
    static std::string MakeChannelOperationId(const std::string& channel_id,
                                              const std::string& account_id,
                                              std::int64_t ingress_sid);

private:
    // 账号级账套(session map + work ledger;首用懒开)。
    struct AccountBooks {
        channel::ChannelSessionMap session_map;
        channel::ChannelWorkLedger work_ledger;
    };
    AccountBooks* BooksFor(const std::string& channel_id, const std::string& account_id);
    AccountBooks* BooksForSessionKey(const std::string& session_key);

    bool ApplyDeliveryOutcomes(std::int64_t now_ms);   // 回执 → outbox 推进
    void ReconcileDeliveredSources(std::int64_t now_ms);  // 终态项 → ingress 结算
    void PumpPairingNotices(std::int64_t now_ms);      // Q1b:配对提示入 outbox
    bool SweepRecovery(std::int64_t now_ms);           // Running 件的跨账裁决
    bool RecoverOne(const std::string& channel_id, const std::string& account_id,
                    const channel::ChannelManager::IngressRunningView& view,
                    std::int64_t now_ms);
    bool RunOneChannelTurn(std::int64_t now_ms);       // 至多一轮新执行
    bool ProcessWorkItem(const std::string& channel_id, const std::string& account_id,
                         const channel::ChannelManager::WorkItem& work, std::int64_t now_ms);
    bool DriveChannelDeliveries(std::int64_t now_ms);  // 渠道段发送/重试
    // 源(ingress sid)的全部段是否终态:全 sent → true(delivered);有
    // failed/unknown → false;未齐 → nullopt(等)。
    std::optional<bool> SourceDeliveryVerdict(const std::string& source_ref) const;

    api::Backend* backend_ = nullptr;
    tools::ToolRegistry* registry_ = nullptr;
    Options options_;
    std::optional<HeadlessExecutor> executor_;
    std::atomic<bool> accepting_{true};
    std::atomic<bool> closed_{false};
    std::string owner_epoch_;
    std::mutex books_mutex_;
    std::map<std::string, std::unique_ptr<AccountBooks>> books_;
    // 限频重试的节流账(delivery_id → 下一可试时刻;进程内,重启即清——
    // 退避是建议值,不是正确性账)。
    std::map<std::string, std::int64_t> retry_at_;
    std::size_t last_account_index_ = 0;  // 账号间轮转公平
    std::size_t active_turns_ = 0;        // 全局并发帽的门(多线程泵)
    // 本进程在跑的轮(sweep 跳过;多线程泵的门)。键 "<ch>/<acct>/<sid>"。
    std::set<std::string> in_flight_sids_;
    // 故障注入窗 3:入箱后、发送前死——本 tick 不驱动投递(恢复路接管)。
    bool suppress_delivery_ = false;
};

}  // namespace lubancode::runtime
