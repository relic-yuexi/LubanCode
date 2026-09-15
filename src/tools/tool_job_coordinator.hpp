// ToolJobCoordinator(异步工具单 P1):持久后台任务的宿主侧服务——队列、
// 租约(ownerEpoch)、单写者完成信封、四接口(start/get/wait/cancel)、权鉴、
// 资源锁、配额、取消与账态注入恢复。合同见
// docs/architecture/trajectory-v3-schema.md §四异步工具条目与 §四.1 P1 定案
// 补遗;只读投影与跨行校验在 trajectory/v3/reader.* (P0)。
//
// 这是宿主侧任务服务,不是给模型的新皮:模型可见性(工具清单/批次闸门/
// 投递规划)归后续批次;P1 不动 AgentLoop、不改工具注册表。
//
// 单写者纪律(单 §5"持久顺序"):
//   调用证据(tool.execution.pending) -> 权鉴 -> job 注册落稳
//   (tool.job.registered,PowerLoss;落稳前不派发) -> 接单结果链
//   (persisted/selected/接单 tool 消息/接纳,start 调用即配齐) ->
//   派发(tool.job.dispatched + tool.execution.started,新租约) ->
//   worker 执行(只见取消旗,不碰账) -> 完成信封(内存投递) ->
//   单写者泵:校验 ownerEpoch -> 执行终态 + tool.job.observed(终态,
//   resultRef 指向结果持久化事件) -> 业务原文走结果仓(32 KiB 预览合同)。
//
// worker 不直接写 history:所有账面追加发生在持协调器锁的泵路径
// (PumpCompletions/Get/Wait/Start 的调用方线程)。旧租约信封与终态后
// 迟到信封一律拒收(计数暴露),不落账。
//
// 线程模型:Start/TryDispatch 在调用方线程起 worker;worker 线程只投
// 信封(mutex+condvar);Wait 循环泵+等待;析构广播取消后有界 join。
// writer 由调用方保证寿命与独占(P1 独占写者场景;与主循环共享写者的
// 装配归后续批次)。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "tools/tool.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::tools {

// ---------------------------------------------------------------------------
// 执行策略(单 §4;七键 P1 定案,枚举见 schema 文档 §四.1)
// ---------------------------------------------------------------------------

struct JobExecutionPolicy {
    bool allow_background = false;
    // read_only|local_write|git|external|irreversible(缺省 read_only;
    // 非 read_only 默认串行——resource_keys 逐键互斥,未声明按工具名单键)。
    std::string side_effect_class = "read_only";
    std::vector<std::string> resource_keys;
    std::string retry_policy = "none";  // none|auto(P1 只执行 none,auto 留档)
    std::uint64_t deadline_ms = 0;      // 0=无限期;到点先请求取消不判失败
    std::string resume_policy = "hold";  // hold|requeue_when_registered
    std::uint64_t max_output_bytes = 1 << 20;  // 结果原文捕获配额

    nlohmann::json ToJson() const;
    // 从账上注册事件载荷恢复;字段缺失取缺省,类型不符取缺省(防御读)。
    static JobExecutionPolicy FromJson(const nlohmann::json& value);

    bool serializes_on_resource() const { return side_effect_class != "read_only"; }
};

// ---------------------------------------------------------------------------
// 权鉴闸门(单 §8:jobId 不是访问凭证)
// ---------------------------------------------------------------------------

struct JobAuthDecision {
    bool allowed = false;
    // 审批未过(单 §6:停 awaiting_approval 不派发)。放行走 GrantApproval
    //(内存放行入队;账面审批随派发隐式放行,P0 折叠口径)。
    bool needs_approval = false;
    std::string reason;  // 拒因(落 tool.execution.rejected 的 reason)
};

// 工具名+入参过原工具权限/作用域/Hook。生产装配归后续批次;缺省
// fail-closed——没挂 gate 一律拒,不静默放行。
using JobAuthorizationGate = std::function<JobAuthDecision(
    const std::string& tool_name, const nlohmann::json& tool_input)>;

// ---------------------------------------------------------------------------
// 并发上限与资源锁(单 §7:全局/Session/工具/资源键四档)
// ---------------------------------------------------------------------------

struct JobConcurrencyLimits {
    std::size_t session_running = 4;  // 本协调器同时在跑上限
    std::size_t per_tool = 2;         // 同逻辑工具同时在跑上限
    std::size_t queued_max = 64;      // 待派队列配额(超了 Start 拒)
};

// 全局在跑配额:跨协调器共享(多会话/测试注入同一只);资源键锁不在
// 这里(键属协调器域),全局档只数在跑总数。limit=0 表示不设上限。
struct GlobalRunningQuota {
    std::atomic<std::size_t> running{0};
    std::size_t limit = 8;
    bool TryAcquire() {
        if (limit == 0) {
            return true;
        }
        std::size_t current = running.load(std::memory_order_acquire);
        while (current < limit) {
            if (running.compare_exchange_weak(current, current + 1,
                                              std::memory_order_acq_rel)) {
                return true;
            }
        }
        return false;
    }
    void Release() {
        std::size_t current = running.load(std::memory_order_acquire);
        while (current > 0) {
            if (running.compare_exchange_weak(current, current - 1,
                                              std::memory_order_acq_rel)) {
                return;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// worker 执行体
// ---------------------------------------------------------------------------

struct JobExecutionContext {
    std::string job_id;
    nlohmann::json input;
    const std::atomic<bool>* cancel = nullptr;  // 取消旗;worker 长操作里查
    std::uint64_t max_output_bytes = 0;         // 结果捕获配额(0=不限)
};

using JobExecutor = std::function<Tool::Result(const JobExecutionContext&)>;

// ---------------------------------------------------------------------------
// 四接口形状(单 §8)
// ---------------------------------------------------------------------------

struct JobStartRequest {
    std::string tool_name;        // 目标 function JSON 工具(P1 只接这类)
    nlohmann::json tool_input;    // 原工具入参
    std::string turn_id;          // originRef(信封 turnId/stepId)
    std::string step_id;
    std::string assistant_message_ref;  // 声明消息(调用证据锚)
    JobExecutionPolicy policy;
    // 异步工具单 P2·native_deferred(账面合同 P0 已钉):mode 缺省
    // job_handle;native_deferred 须带 wireCallRef(provider/wire/callId/
    // async)。native 不写接单链——原调用保持欠账,业务结果由规划器在
    // 请求边界配原 call(P3 原生试点;P2 只接 seam 与假后端剧本)。
    std::string mode = "job_handle";
    nlohmann::json wire_call_ref;  // native_deferred 必带(载荷原样落账)
};

struct JobStartResult {
    bool ok = false;
    std::string error_code;  // job.start.denied|job.start.queue_full|v3writer.*
    std::string error;
    std::string job_id;
    std::string action_id;  // 本 job 的 v3 Action 身份(P2:闸门/规划器引用)
    std::string status;     // queued|awaiting_approval|registered(提前档接单未补)
    // 接单回执正文({"jobId":...,"status":...} JSON):批次闸门拿它当
    // job_handle 调用的即配 tool_result。失败/接单未补时为空。
    std::string admission_content;
};

struct JobStatusView {
    std::string job_id;
    std::string action_id;  // v3 Action 身份(P2:完成通知引用)
    std::string state;  // registered|queued|running|succeeded|failed|cancelled|
                        // unknown|awaiting_approval(单 §6 执行投影)
    bool cancel_requested = false;
    std::string result_ref;  // 终态且结果已落:tool.result.persisted 事件 id
    std::uint64_t result_version = 0;
    std::string preview;  // 终态且成功:≤32 KiB 有界预览(§4.18 合同)
    bool preview_truncated = false;
    std::string failure;  // failed 观测的失败码
    // 四接口都过授权闸门(单 §8:jobId 不是访问凭证);拒时 state 留空。
    bool access_denied = false;
    std::string access_reason;
};

struct JobWaitResult {
    bool satisfied = false;  // any:≥1 终态;all:全部终态
    bool timed_out = false;  // 超时回 pending+快照,不宣告失败(单 §8)
    std::vector<JobStatusView> statuses;  // 游标=本次观测快照
};

struct JobCancelResult {
    bool ok = false;
    std::string error;
    std::string status;  // cancel_requested|already_terminal|unknown_job
    std::string terminal;  // already_terminal 时的终态
};

// ---------------------------------------------------------------------------
// 恢复(单 §6 表;纯读账定计划,账态注入可重复)
// ---------------------------------------------------------------------------

struct JobRecoveryPlan {
    struct Item {
        std::string job_id;
        std::string action_id;
        std::string turn_id;
        std::string step_id;
        std::string mode;  // job_handle(P1 只产这个;账上别的模式原样报)
        // requeue: registered 未派发,可重新入队(epoch 接续递增);
        // awaiting_approval: 审批未过,不派发;
        // unknown_hold: dispatched 无终态,转 unknown 不盲跑;
        // complete_delivery: 终态/原文已在账,补观测/接单链/接纳,不重跑;
        // already_terminal: 终态与账面齐全。
        std::string disposition;
        std::string detail;
        JobExecutionPolicy policy;
        std::string tool_name;       // requeue 执行材料(声明块参数恢复)
        nlohmann::json tool_input;
        std::string assistant_message_ref;
        // 账面事实(PlanRecovery 折好,AdoptRecovery 对齐用;不读活账):
        int dispatched_count = 0;    // 已发租约数,接管派发 epoch 接续
        std::uint64_t attempt = 1;   // 账上当前 attempt 号
        bool attempt_started = false;
        int attempt_terminal = 0;    // 0=无终态;1..5 对应 ToolActionSession::Terminal
        std::string attempt_terminal_event;  // 该终态的 eventId(补链引用锚)
        std::string business_result_ref;     // attempt>1 的业务 tool.result.persisted
        std::string result_ref;      // 已落终态观测的 resultRef
        std::uint64_t result_version = 0;
        bool cancel_requested = false;  // 账上取消意图在案(≠已终止,单 §8)
        bool admission_complete = false;  // 接单 tool 消息已在当前链上
    };
    std::vector<Item> items;
};

// ---------------------------------------------------------------------------
// ToolJobCoordinator
// ---------------------------------------------------------------------------

class ToolJobCoordinator {
public:
    struct Options {
        JobConcurrencyLimits limits;
        std::shared_ptr<GlobalRunningQuota> global;  // 缺省自建(limit 8)
        std::function<std::int64_t()> clock_ms;      // 缺省墙钟;测试注固定钟
    };

    // writer:本会话 v3 单写者(引用,寿命由调用方保证,协调器不收柄);
    // gate:权鉴闸门(空=fail-closed 全拒);executor:worker 执行体。
    ToolJobCoordinator(trajectory::v3::V3Writer& writer, JobAuthorizationGate gate,
                       JobExecutor executor, Options options = {});
    ~ToolJobCoordinator();
    ToolJobCoordinator(const ToolJobCoordinator&) = delete;
    ToolJobCoordinator& operator=(const ToolJobCoordinator&) = delete;

    // ---- 四接口(单 §8)----

    // search_start 式接单:调用证据 -> 权鉴 -> 注册落稳(落稳前不派发)
    // -> 接单结果链 -> 入队并按配额/资源锁试派发。
    JobStartResult StartJob(const JobStartRequest& request);

    // ---- 流式提前档(异步工具单 P2;单 §7"后期提速"并入) ------------------
    // SSE 流中单枚 call item 完整即派发:调用证据 -> 权鉴 -> 注册落稳 ->
    // 接单事实链(attempt 1 的 started/finished/persisted/selected,账面
    // 事实不依赖声明消息落账)-> 入队派发(worker 先跑)。接单 tool 消息
    // 留给 CompleteAdmission——声明消息(assistant)落账之后才许进上下文
    // 链,链序不倒。assistant_message_ref 允许指向流式预留的 messageId
    // (interrupted 路也会以它成行;进程中途崩溃则留恢复缺口,按账面
    // disposition 收)。仅 job_handle;native 模式报 unsupported_mode。
    JobStartResult StartJobEarly(const JobStartRequest& request);

    // 补接单(批次收口:assistant 已落账):幂等;已补/已终态只回 true。
    // 回 false = 接单链落账失败(job 已注册,恢复按 complete_delivery 补)。
    // 成功时 admission_content 给接单回执正文。
    bool CompleteAdmission(const std::string& job_id, std::string* admission_content = nullptr);

    // 当前状态;终态带 resultRef 与 ≤32 KiB 有界预览;不自动重跑
    //(重复 Get 零副作用)。内部先泵一把(收割已到信封)。
    JobStatusView GetJob(const std::string& job_id);

    // 有界等待:mode any|all。内部循环泵+condvar;超时回 pending+快照
    // 游标,timed_out=true,不宣告任务失败。
    JobWaitResult WaitJobs(const std::vector<std::string>& job_ids,
                           std::uint64_t timeout_ms, bool wait_all);

    // 请求取消:落 tool.job.cancel_requested + 置旗;未派发的当场收终态,
    // 在跑的不保证终止(单 §8)。回取消请求状态。
    JobCancelResult CancelJob(const std::string& job_id, const std::string& reason);

    // 审批放行:awaiting_approval 的 job 入队派发。内存放行,不落账
    //(审批随派发隐式放行,单 §6/P0 折叠口径)。
    JobStartResult GrantApproval(const std::string& job_id);

    // ---- 单写者泵 ----

    // 收割完成信封落账一次(非阻塞;Wait/Get/Start 内部自泵,宿主边界
    // 也可显式驱动)。返回本次落账的终态数。
    std::size_t PumpCompletions();

    // 测试/诊断口:绕过 worker 线程直接投一枚完成信封,走与生产完全
    // 同款的校验(租约不符/终态后迟到一律拒收)。返回信封是否被接纳。
    bool DebugSubmitEnvelope(const std::string& job_id, const std::string& owner_epoch,
                             Tool::Result result);

    // ---- 恢复(单 §6 表)----

    // 纯读账定恢复计划(不持锁、不写账;静态,任意线程)。
    static JobRecoveryPlan PlanRecovery(const trajectory::v3::V3Ledger& ledger);

    // 采用计划:requeue 重入队(不重写注册,接管派发 epoch 接续递增);
    // unknown_hold 落 tool.job.observed(unknown) 不盲跑;complete_delivery
    // 补观测/接单结果链/接纳,不重跑工作;awaiting_approval/already_terminal
    // 只登记内存态。返回实际推进的条数。
    std::size_t AdoptRecovery(const JobRecoveryPlan& plan);

    // ---- 诊断 ----

    std::uint64_t stale_envelopes_rejected() const;
    std::uint64_t duplicate_terminal_envelopes_rejected() const;
    std::size_t running_count() const;
    std::size_t queued_count() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace lubancode::tools
