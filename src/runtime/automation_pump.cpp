// GatewayAutomationPump 实现(常驻总装 V1)。装配合同见头文件。
#include "runtime/automation_pump.hpp"

#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/session_work_scheduler.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"
#include "workspace/index.hpp"

namespace lubancode::runtime {

GatewayAutomationPump::OpenResult GatewayAutomationPump::Open(GatewayAutomationPump* out,
                                                              api::Backend& backend,
                                                              tools::ToolRegistry& registry,
                                                              Options options) {
    OpenResult result;
    if (out == nullptr) {
        result.error = "pump 装配:out 为空";
        return result;
    }
    out->backend_ = &backend;
    out->registry_ = &registry;
    out->options_ = std::move(options);
    out->store_ = gateway::AutomationStore();
    out->outbox_ = gateway::DurableReplyOutbox();
    {
        const auto open = gateway::AutomationStore::Open(&*out->store_,
                                                         out->options_.paths.automation_log);
        if (!open.ok) {
            result.error = open.error;
            out->store_.reset();
            return result;
        }
    }
    {
        gateway::DurableReplyOutbox::Paths outbox_paths;
        outbox_paths.log_file = out->options_.paths.outbox_log;
        outbox_paths.replies_dir = out->options_.paths.replies_dir;
        outbox_paths.published_dir = out->options_.paths.published_dir;
        const auto open = gateway::DurableReplyOutbox::Open(&*out->outbox_, outbox_paths);
        if (!open.ok) {
            result.error = open.error;
            out->store_.reset();
            out->outbox_.reset();
            return result;
        }
    }
    result.ok = true;
    return result;
}

GatewayAutomationPump::~GatewayAutomationPump() = default;

bool GatewayAutomationPump::TickOnce(std::int64_t now_ms) {
    if (closed_.load()) {
        return false;
    }
    // 1) 消费 job 控制命令(暂停接活后不再受理新命令)。
    if (accepting_.load()) {
        const gateway::ConsumedJobCommands commands =
            gateway::PollJobCommands(options_.paths.control_dir);
        for (const auto& add : commands.adds) {
            const auto receipt = store_->CreateOnceJob(
                add.job_id, add.prompt,
                add.due_at_ms != 0 ? add.due_at_ms : now_ms, now_ms, add.idempotency_key);
            if (!receipt.accepted && !receipt.duplicate) {
                // 命令账记不了(job 冲突/写盘失败):消费即删已发生,留给
                // operator 从日志/状态处置;泵只在账 broken 时停。
            }
        }
        for (const auto& run_now : commands.run_nows) {
            (void)store_->RequestRunNow(run_now.job_id, now_ms, run_now.idempotency_key);
        }
    }
    if (store_->broken()) {
        return false;  // 领域账 broken:停泵(写盘失败停止受理/执行)
    }
    // 2) 恢复扫描:未结算 occurrence 的跨账裁决(V1 面,见头文件表)。
    const RecoveryOutcome recovery = SweepRecovery(now_ms);
    if (!recovery.error.empty()) {
        return false;
    }
    // 3) outbox 投递(补齐链的最后一段;pending 留下轮)。
    (void)outbox_->DeliverPending(now_ms);
    if (outbox_->broken()) {
        return false;
    }
    // 4) 至多一枚新执行(有界):SessionWorkScheduler 公平取件。
    if (!accepting_.load()) {
        return true;
    }
    std::string error;
    if (!RunOneOccurrence(now_ms, &error)) {
        if (!error.empty()) {
            return false;  // 账写不进:停泵;执行失败本身已结算,不停
        }
    }
    return !store_->broken();
}

void GatewayAutomationPump::StopAccepting() {
    accepting_.store(false);  // 关机次序第一步:不再受理新活(在飞收尾照走)
}

bool GatewayAutomationPump::Close(int grace_ms) {
    (void)grace_ms;
    // V1 同步泵:Close 时无在飞执行(TickOnce 已收口),writer 析构即关。
    // 真异步化(V2+)时这里等在飞 turn 收口或置 cancel 后等宽限。
    closed_.store(true);
    store_.reset();
    outbox_.reset();
    return true;
}

GatewayAutomationPump::RecoveryOutcome GatewayAutomationPump::SweepRecovery(std::int64_t now_ms) {
    RecoveryOutcome outcome;
    for (const auto& occurrence : store_->OpenOccurrences()) {
        std::string error;
        const auto settled = RecoverOccurrence(occurrence, now_ms, &error);
        if (!error.empty()) {
            outcome.error = error;
            return outcome;
        }
        if (settled.has_value()) {
            outcome.progressed = true;
        }
    }
    return outcome;
}

std::optional<std::string> GatewayAutomationPump::RecoverOccurrence(
    const gateway::AutomationOccurrence& occurrence, std::int64_t now_ms, std::string* error) {
    // bound 行不在:claim 后崩(V1 保守 needs_review——"核实无旧执行再
    // 重派"归 V2;这里不猜)。
    if (occurrence.session_id.empty() || occurrence.turn_id.empty()) {
        if (store_->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                     "claimed_without_binding(V1 保守:核实归 V2)", now_ms)) {
            return std::string("needs_review");
        }
        *error = "automation.append_failed: needs_review 结算落不了盘";
        return std::nullopt;
    }
    // 定位原场(§11.4 resolver:key 反查房门,不拼目录名)。
    const auto room = workspace::ResolveDirByWorkspaceKey(options_.workspaces_root,
                                                          options_.workspace_identity.workspace_key);
    if (!room.has_value()) {
        if (store_->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                     "workspace_room_unresolved", now_ms)) {
            return std::string("needs_review");
        }
        *error = "automation.append_failed: needs_review 结算落不了盘";
        return std::nullopt;
    }
    const auto stream = trajectory::v3::FindV3SessionStream(*room / "sessions" /
                                                            occurrence.session_id);
    if (!stream.has_value()) {
        if (store_->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                     "v3_stream_not_found", now_ms)) {
            return std::string("needs_review");
        }
        *error = "automation.append_failed: needs_review 结算落不了盘";
        return std::nullopt;
    }
    const auto ledger = trajectory::v3::ReadV3Ledger(*stream);
    if (!ledger.has_value()) {
        if (store_->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                     "v3_stream_unreadable: " + ledger.error(), now_ms)) {
            return std::string("needs_review");
        }
        *error = "automation.append_failed: needs_review 结算落不了盘";
        return std::nullopt;
    }
    // 冻结策略重算同一 selectionId(纯函数,不调模型)。
    const ReplySelectionPlan plan = PlanReplySelection(*ledger, occurrence.turn_id);
    if (!plan.ok) {
        // 无 assistant:生成没完成(claim 后崩在执行中)。V1 不盲目重跑
        //(§八"工具 started 无 terminal 不默认重跑";重派归 V2)。
        if (store_->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                     "generation_incomplete: " + plan.error, now_ms)) {
            return std::string("needs_review");
        }
        *error = "automation.append_failed: needs_review 结算落不了盘";
        return std::nullopt;
    }
    if (!SelectionAlreadyCommitted(*ledger, plan.selection_id)) {
        // 窗口 1:生成结束、selection 未提交——补齐(全程不调模型)。
        // 续原卷写这枚事实(V3Writer::Continue 验卷后追加;不开新轮,
        // 与 resume-as-new 不冲突——那是续对话的路,这是补事实)。
        auto writer = trajectory::v3::V3Writer::Continue(*stream);
        if (!writer.has_value()) {
            if (store_->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                         "v3_writer_continue_failed: " + writer.error(), now_ms)) {
                return std::string("needs_review");
            }
            *error = "automation.append_failed: needs_review 结算落不了盘";
            return std::nullopt;
        }
        const auto commit =
            CommitReplySelection(&*writer, options_.paths.replies_dir, plan,
                                 occurrence.session_id);
        if (!commit.committed) {
            if (store_->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                         commit.error_code + ": " + commit.error, now_ms)) {
                return std::string("needs_review");
            }
            *error = "automation.append_failed: needs_review 结算落不了盘";
            return std::nullopt;
        }
    }
    // selection 已在(或刚补齐):补 outbox 投影(同 deliveryId 幂等)。
    const auto enqueued = outbox_->Enqueue(plan.selection_id, plan.text,
                                           occurrence.session_id, occurrence.turn_id, now_ms);
    if (!enqueued.accepted && !enqueued.duplicate) {
        // 入箱失败:投递链断,occurrence 停审(执行事实保留,不冒充成功)。
        if (store_->SettleOccurrence(occurrence.occurrence_id, "needs_review",
                                     "outbox_enqueue_failed: " + enqueued.error_code, now_ms)) {
            return std::string("needs_review");
        }
        *error = "automation.append_failed: needs_review 结算落不了盘";
        return std::nullopt;
    }
    // 投递(本地文件;已发布核 hash 后补回执,不出第二份)。
    (void)outbox_->DeliverPending(now_ms);
    // 结算:投递成功与否分开看(§九:执行成功与投递失败分别显示)。
    const auto item = outbox_->Find(gateway::MakeDeliveryId(plan.selection_id, "local:file", 1));
    const std::string outcome = item.has_value() && item->state == "delivered"
                                    ? std::string("succeeded")
                                    : std::string("needs_review");
    const std::string detail = item.has_value()
                                   ? ("delivery_state=" + item->state)
                                   : std::string("delivery_item_missing");
    if (!store_->SettleOccurrence(occurrence.occurrence_id, outcome, detail, now_ms)) {
        *error = "automation.append_failed: 结算落不了盘";
        return std::nullopt;
    }
    return outcome;
}

bool GatewayAutomationPump::RunOneOccurrence(std::int64_t now_ms, std::string* error) {
    // 公平取件(复用 SessionWorkScheduler):候选包成 SessionWork。
    std::vector<SessionWork> candidates;
    for (const auto& occurrence : store_->ListOccurrences()) {
        if (occurrence.state == gateway::AutomationOccurrence::State::Scheduled &&
            occurrence.slot_ms <= now_ms) {
            SessionWork work;
            work.kind = WorkKind::AutomationDue;
            work.id = occurrence.occurrence_id;
            work.payload = nlohmann::json{{"jobId", occurrence.job_id}};
            candidates.push_back(std::move(work));
        }
    }
    const FairnessCounter fairness;  // automation 一族无 goal 竞争,恒零账
    const auto picked = PumpNextWork(candidates, fairness);
    if (!picked.has_value()) {
        return true;  // 没有到点的活
    }
    // claim(领域账,ownerEpoch fencing)。ClaimDue 自带 slot-FIFO 挑选,
    // 认领结果以它为准(排序泵只决定"该不该取活",不重复做挑选)。
    const auto claimed = store_->ClaimDue(owner_epoch_, now_ms);
    if (!claimed.has_value()) {
        if (store_->broken()) {
            *error = "automation.append_failed: claim 落不了盘";
            return false;
        }
        return true;  // 撞上并发消费(V1 单飞不该发生,防御)
    }
    const std::string occurrence_id = claimed->occurrence_id;
    const auto job = store_->FindJob(claimed->job_id);
    if (!job.has_value()) {
        (void)store_->SettleOccurrence(occurrence_id, "failed", "job_missing", now_ms);
        return true;
    }
    // 执行(共用 headless 装配)。
    HeadlessExecutor::Options executor_options;
    executor_options.workspaces_root = options_.workspaces_root;
    executor_options.workspace_root = options_.workspace_identity.identity_root;
    executor_options.cwd_utf8 = options_.cwd_utf8;
    executor_options.lubancode_version = options_.lubancode_version;
    executor_options.wire_name = options_.wire_name;
    executor_options.model = options_.model;
    executor_options.replies_dir = options_.paths.replies_dir;
    executor_options.tools = options_.tools;
    executor_options.hook_dispatcher = options_.hook_dispatcher;
    executor_options.max_steps_per_turn = options_.max_steps_per_turn;
    executor_options.max_wall_secs = options_.max_wall_secs;
    executor_options.max_total_tokens = options_.max_total_tokens;
    executor_options.fault_injection = options_.fault_injection;
    HeadlessExecutor executor(*backend_, *registry_, std::move(executor_options));

    HeadlessWorkBinding binding;
    binding.work_id = claimed->occurrence_id;
    binding.source_kind = "automation";
    binding.source_id = claimed->job_id;
    binding.owner_epoch = owner_epoch_;
    binding.attempt = claimed->attempt;

    bool domain_bound = false;
    std::atomic<bool> cancel_flag{false};
    const auto result = executor.Execute(
        job->prompt, binding,
        [this, occurrence_id, now_ms, &domain_bound](const std::string& session_id,
                                                     const std::string& turn_id) {
            // 领域绑定先于 V3 work.bound(恢复器优先走领域行定位原场)。
            domain_bound =
                store_->BindOccurrence(occurrence_id, session_id, turn_id, now_ms);
        },
        &cancel_flag);
    if (result.ok) {
        // 领域绑定没落成:恢复锚缺失(claim 后崩窗内的中间态),执行虽
        // 完成但结算如实停审——不冒充可恢复。
        if (!domain_bound) {
            if (!store_->SettleOccurrence(occurrence_id, "needs_review",
                                          "domain_binding_missing", now_ms)) {
                *error = "automation.append_failed: 结算落不了盘";
                return false;
            }
            return true;
        }
        // 入 outbox(同 deliveryId 幂等;原件已在则只落账行)。
        const auto enqueued = outbox_->Enqueue(result.selection_id, result.reply_text,
                                               result.session_id, result.turn_id, now_ms);
        // 故障注入窗 3:入 outbox 后、发布本地文件前——恢复器应从已入箱
        // 项续投,不重跑执行(只在入箱真成了才注入,入箱失败走正常结算)。
        if ((enqueued.accepted || enqueued.duplicate) && options_.fault_after_enqueue) {
            if (const std::string fault = options_.fault_after_enqueue(); !fault.empty()) {
                return true;  // 不投递不结算:模拟进程死在半路,恢复路接管
            }
        }
        (void)outbox_->DeliverPending(now_ms);
        const auto item =
            outbox_->Find(gateway::MakeDeliveryId(result.selection_id, "local:file", 1));
        const bool delivered = item.has_value() && item->state == "delivered";
        const std::string outcome = delivered ? std::string("succeeded") : std::string("needs_review");
        const std::string detail =
            delivered ? std::string("delivered")
                      : ("delivery_incomplete: " +
                         (enqueued.accepted || enqueued.duplicate
                              ? std::string("pending")
                              : enqueued.error_code));
        if (!store_->SettleOccurrence(occurrence_id, outcome, detail, now_ms)) {
            *error = "automation.append_failed: 结算落不了盘";
            return false;
        }
        return true;
    }
    // 执行失败:如实结算 failed(取消/生成失败/账写不进都算);故障注入
    // 模拟"进程死在半路"——不结算(进程真死也确实落不了结算行),留给
    // 重启后的恢复扫描裁决。
    if (result.error_code == "gateway.fault_injected") {
        return true;  // 不结算:恢复路接管
    }
    if (!store_->SettleOccurrence(occurrence_id, "failed",
                                  result.error_code + ": " + result.error, now_ms)) {
        *error = "automation.append_failed: 结算落不了盘";
        return false;
    }
    return true;
}

}  // namespace lubancode::runtime
