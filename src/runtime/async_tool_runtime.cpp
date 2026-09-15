// async_tool_runtime.hpp 的实现。闸门裁决(单 §4)+ 提前档策略(单 §7
// 两档派发点,2026-09-16 指示)+ 完成信封回灌(泵 → mailbox)。
#include "runtime/async_tool_runtime.hpp"

#include <algorithm>
#include <utility>

#include "platform/log_sink.hpp"
#include "runtime/session_runtime.hpp"
#include "runtime/trajectory_session.hpp"
#include "tools/job_tools.hpp"
#include "trajectory/v3/envelope.hpp"

namespace lubancode::runtime {
namespace {

using tools::JobAuthDecision;
using tools::JobStartRequest;
using tools::JobStartResult;
using tools::ToolJobCoordinator;

// 已接单/已派发的调用簿:闸门自己的内存账(dedup、幂等、完成通知路由)。
struct KnownJob {
    std::string job_id;
    std::string action_id;
    std::string mode;  // job_handle|native_deferred
    std::string provider_call_id;
    std::string tool_name;
    std::string turn_id;
    std::string step_id;
    bool early = false;      // 流式提前档派的(批次时只补接单)
    bool notified = false;   // 完成通知已入 mailbox
};

int TerminalKindOfState(const std::string& state) {
    if (state == "succeeded") return 1;
    if (state == "failed") return 2;
    if (state == "cancelled") return 3;
    return 5;  // unknown → Unknown(补链对齐用)
}

}  // namespace

struct AsyncToolRuntime::Impl final : agent::ToolBatchGate {
    Hooks hooks;
    AsyncToolRuntimeOptions options;
    std::shared_ptr<ToolJobCoordinator> coordinator_;
    std::unique_ptr<ResultDeliveryPlannerImpl> planner_;
    // 当前轮桥(InstallTurnBridge 每轮换;桥面查询口从这里走)。
    TrajectoryTurnBridge* current_bridge = nullptr;
    // 能力快照(首次裁决落一次;basis 留档)。
    std::string capability_event_id;
    bool capability_recorded = false;
    std::mutex book_mutex;  // known_jobs/early 册(批次路径与流式探针同线程,
                            // 恢复/诊断可能异线程,上锁求稳)
    std::map<std::string, KnownJob> known_by_call;   // provider call id -> job
    std::map<std::string, KnownJob> known_by_job;    // job id -> job(泵路由)

    // ---- 能力闸(单 §4 末):合成 + 快照落账 ------------------------------
    ProviderToolContract ContractFor(bool call_marked_async) const {
        ProviderToolContract contract;
        contract.provider = options.provider;
        contract.wire = options.wire;
        contract.model = options.model;
        contract.endpoint = options.endpoint;
        contract.call_marked_async = call_marked_async;
        contract.native_probe_status = options.native_probe_status;
        contract.native_probe_evidence = options.native_probe_evidence;
        contract.job_handle_disabled = options.job_handle_disabled;
        return contract;
    }

    void RecordCapabilitySnapshotLocked(bool call_marked_async) {
        if (capability_recorded || hooks.writer == nullptr || hooks.writer_mutex == nullptr) {
            return;
        }
        // basis 三件非空才落(载荷合同:provider/wire/model 非空 string)。
        if (options.provider.empty() || options.wire.empty() || options.model.empty()) {
            return;
        }
        const ProviderToolContract contract = ContractFor(call_marked_async);
        const auto verdicts = EvaluateProviderToolContract(contract);
        if (verdicts.empty()) {
            return;
        }
        std::unique_lock<std::recursive_mutex> writer_lock(*hooks.writer_mutex);
        trajectory::v3::EventDraft draft;
        draft.kind = trajectory::v3::EventKindV3::ToolCapabilityRecorded;
        draft.payload = ContractSnapshotPayload(contract, verdicts);
        const auto receipt =
            hooks.writer->AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
        capability_recorded = true;  // 落不落稳都只试一次(诊断另有账)
        if (receipt.status == trajectory::v3::WriteReceipt::Status::Committed) {
            capability_event_id = receipt.id;
        } else {
            platform::LogSink::Instance().Error("async-gate",
                                                 "tool.capability.recorded 落账失败: " +
                                                     receipt.error_code);
        }
    }

    bool NativeAllowed(bool call_marked_async) {
        // unknown/缺项 fail-closed(单 §4):只认 verified。
        const auto verdicts = EvaluateProviderToolContract(ContractFor(call_marked_async));
        for (const auto& verdict : verdicts) {
            if (verdict.capability == "native_deferred") {
                return verdict.status == "verified";
            }
        }
        return false;
    }

    // ---- agent::ToolBatchGate:流式提前档探针 ----------------------------
    bool OnCallItemComplete(const api::ToolUseBlock& call,
                            const StreamCallContext& context) override {
        const auto policy_it = options.tools.find(call.name);
        if (policy_it == options.tools.end()) {
            return false;  // 不在白名单:不提前
        }
        const AsyncToolPolicy& policy = policy_it->second;
        if (policy.dispatch_point != ToolDispatchPoint::OnCallItemComplete) {
            return false;  // 派发点策略:本枚走批次档
        }
        // 可先跑的执行策略(2026-09-16 指示):只读/幂等 + 无资源键冲突 +
        // 权限已在握。side_effect_class 只收 read_only;声明了 resource_keys
        // 的照旧同步队列(键要排队,提前派发抢位)。
        if (policy.execution.side_effect_class != "read_only" ||
            !policy.execution.resource_keys.empty()) {
            return false;
        }
        if (policy.require_call_async_mark && !call.async_call) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(book_mutex);
            if (known_by_call.count(call.id) != 0) {
                return false;  // 重复终帧:只派发一次
            }
        }
        // 权限已在手:权鉴现查(fail-closed)。
        if (!hooks.auth) {
            return false;
        }
        const JobAuthDecision auth = hooks.auth(call.name, call.input);
        if (!auth.allowed) {
            return false;  // 审批未过/拒绝:不提前(needs_approval 走批次档问)
        }
        // 调用证据落账(P1 纪律:证据落不了账不开跑)。assistant 还没成行,
        // 锚用流式预留的 messageId(interrupted 路也以它成行;进程中途崩
        // 溃则留恢复缺口,按账面 disposition 收)。证据锚解析不到(没轮桥/
        // 流没起账)不派发。
        if (!hooks.reserved_assistant_message_id) {
            return false;
        }
        const auto reserved = hooks.reserved_assistant_message_id(context.trajectory_request_id);
        if (!reserved.has_value() || reserved->empty()) {
            return false;
        }
        RecordCapabilitySnapshotLocked(call.async_call);
        JobStartRequest request;
        request.tool_name = call.name;
        request.tool_input = call.input;
        request.turn_id = context.turn_id;
        request.step_id = context.step_id;
        request.assistant_message_ref = *reserved;
        request.policy = policy.execution;
        const JobStartResult started = coordinator_->StartJobEarly(request);
        if (!started.ok) {
            if (!started.job_id.empty()) {
                // 已注册但接单事实没落稳:收掉,不留恢复会重跑的悬空 job。
                coordinator_->CancelJob(started.job_id, "early_start_fallback_inline");
            }
            return false;
        }
        KnownJob job;
        job.job_id = started.job_id;
        job.action_id = started.action_id;
        job.mode = "job_handle";
        job.provider_call_id = call.id;
        job.tool_name = call.name;
        job.turn_id = context.turn_id;
        job.step_id = context.step_id;
        job.early = true;
        {
            std::lock_guard<std::mutex> lock(book_mutex);
            known_by_call[call.id] = job;
            known_by_job[job.job_id] = job;
        }
        platform::LogSink::Instance().Info(
            "async-gate", "[early-dispatch] " + call.name + " call=" + call.id +
                              " job=" + job.job_id + "(call item 完整即派发,宿主继续消费流)");
        return true;
    }

    // ---- agent::ToolBatchGate:批次裁决 ----------------------------------
    std::vector<ToolCallAdjudication> AdjudicateBatch(
        const std::vector<api::ToolUseBlock>& calls) override {
        std::vector<ToolCallAdjudication> adjudications(calls.size());
        bool saw_async_call = false;
        for (std::size_t i = 0; i < calls.size(); ++i) {
            const api::ToolUseBlock& call = calls[i];
            ToolCallAdjudication& adjudication = adjudications[i];
            const auto policy_it = options.tools.find(call.name);
            if (policy_it == options.tools.end()) {
                continue;  // 缺省 inline
            }
            const AsyncToolPolicy& policy = policy_it->second;
            saw_async_call = saw_async_call || call.async_call;
            if (call.async_call && NativeAllowed(true)) {
                // 原生调用带 async 且能力 verified:留欠账(单 §8 native
                // 轨迹——没有伪造的 tool 结果,业务结果配原 call)。
                adjudication.mode = ToolProtocolMode::NativeDeferred;
                adjudication.dispatch_point = ToolDispatchPoint::OnAssistantComplete;
                adjudication.basis = capability_event_id;
                continue;
            }
            if (policy.require_call_async_mark && !call.async_call) {
                continue;  // 策略只要原生 async 调用:没标按 inline
            }
            if (options.job_handle_disabled) {
                continue;  // 装配明示禁 job_handle:全 inline
            }
            adjudication.mode = ToolProtocolMode::JobHandle;
            adjudication.dispatch_point = policy.dispatch_point;
            adjudication.basis = "host_policy:" + call.name;
            if (call.async_call) {
                adjudication.note =
                    "provider 标了 async 但 native_deferred 能力未验证(unknown "
                    "fail-closed,单 §4),按 job_handle 伪异步接单配对";
            }
        }
        RecordCapabilitySnapshotLocked(saw_async_call);
        return adjudications;
    }

    // ---- agent::ToolBatchGate:接单 --------------------------------------
    std::optional<tools::Tool::Result> TakeJobOrder(
        const api::ToolUseBlock& call, const ToolCallAdjudication& adjudication) override {
        // 提前档派过的:只补接单(幂等),不重派。
        std::optional<KnownJob> early;
        {
            std::lock_guard<std::mutex> lock(book_mutex);
            const auto it = known_by_call.find(call.id);
            if (it != known_by_call.end() && it->second.early) {
                early = it->second;
            }
        }
        if (early.has_value()) {
            std::string admission_content;
            if (!coordinator_->CompleteAdmission(early->job_id, &admission_content) ||
                admission_content.empty()) {
                // 接单链落账失败:合成接单回执保配对(工作已在跑,不许留
                // 悬空;账面缺口由恢复 disposition complete_delivery 补)。
                platform::LogSink::Instance().Warn(
                    "async-gate", "提前档接单链补落失败,合成回执保配对: " + early->job_id);
                admission_content =
                    nlohmann::json::object({{"jobId", early->job_id},
                                            {"status", "running"},
                                            {"note", "admission_pending_recovery"}})
                        .dump();
            }
            tools::Tool::Result admission;
            admission.SetText(admission_content);
            return admission;
        }
        // 批次档接单:账面声明上下文从桥的声明册解析(assistant 已落账)。
        JobStartRequest request;
        const auto policy_it = options.tools.find(call.name);
        if (policy_it == options.tools.end()) {
            return std::nullopt;
        }
        request.tool_name = call.name;
        request.tool_input = call.input;
        request.policy = policy_it->second.execution;
        if (hooks.call_origin_resolver) {
            auto origin = hooks.call_origin_resolver(call.id);
            if (origin.has_value()) {
                request.turn_id = origin->turn_id;
                request.step_id = origin->step_id;
                if (!origin->assistant_message_ref.empty()) {
                    request.assistant_message_ref = origin->assistant_message_ref;
                }
            }
        }
        if (request.turn_id.empty() || request.step_id.empty() ||
            request.assistant_message_ref.empty()) {
            return std::nullopt;  // originRef 三件缺一不注册(单 §5):回落 inline
        }
        if (adjudication.mode == ToolProtocolMode::NativeDeferred) {
            request.mode = "native_deferred";
            request.wire_call_ref = nlohmann::json::object(
                {{"provider", options.provider},
                 {"wire", options.wire},
                 {"callId", call.id},
                 {"async", true}});
        }
        const JobStartResult started = coordinator_->StartJob(request);
        if (!started.ok) {
            if (!started.job_id.empty()) {
                // 已注册但接单链没落稳:收掉,不留恢复会重跑的悬空 job。
                coordinator_->CancelJob(started.job_id, "admission_fallback_inline");
            }
            return std::nullopt;  // 拒绝/队满/落账失败:回落 inline 真执行
        }
        KnownJob job;
        job.job_id = started.job_id;
        job.action_id = started.action_id;
        job.mode = request.mode;
        job.provider_call_id = call.id;
        job.tool_name = call.name;
        job.turn_id = request.turn_id;
        job.step_id = request.step_id;
        {
            std::lock_guard<std::mutex> lock(book_mutex);
            known_by_call[call.id] = job;
            known_by_job[job.job_id] = job;
        }
        if (adjudication.mode == ToolProtocolMode::NativeDeferred) {
            return std::nullopt;  // 留欠账:业务结果由规划器配原 call
        }
        if (started.admission_content.empty()) {
            return std::nullopt;  // 接单没配齐(不应发生):回落 inline
        }
        tools::Tool::Result admission;
        admission.content = started.admission_content;
        return admission;
    }

    // ---- agent::ToolBatchGate:完成信封回灌口 ----------------------------
    void PumpBatchBoundary() override {
        coordinator_->PumpCompletions();
        std::vector<KnownJob> jobs_to_check;
        {
            std::lock_guard<std::mutex> lock(book_mutex);
            for (const auto& [job_id, job] : known_by_job) {
                (void)job_id;
                if (!job.notified) {
                    jobs_to_check.push_back(job);
                }
            }
        }
        for (const KnownJob& job : jobs_to_check) {
            const auto view = coordinator_->GetJob(job.job_id);
            if (view.access_denied) {
                continue;  // 观察权都没有:不通知(账面有,读取侧另查)
            }
            const bool terminal = view.state == "succeeded" || view.state == "failed" ||
                                  view.state == "cancelled" || view.state == "unknown";
            if (!terminal) {
                continue;
            }
            CompletionNotice notice;
            notice.job_id = job.job_id;
            notice.action_id = job.action_id;
            notice.mode = job.mode;
            notice.provider_call_id = job.provider_call_id;
            notice.turn_id = job.turn_id;
            notice.step_id = job.step_id;
            notice.result_ref = view.result_ref;
            notice.result_version = view.result_version;
            notice.preview = view.preview;
            notice.preview_truncated = view.preview_truncated;
            notice.failed = view.state != "succeeded";
            notice.failure = view.failure.empty() ? view.state : view.failure;
            notice.attempt = 2;  // 工作在 attempt 2(接单是 1,P1 账序)
            notice.terminal_kind = TerminalKindOfState(view.state);
            notice.branch = hooks.writer != nullptr ? hooks.writer->session_id() : std::string();
            planner_->NotifyCompletion(std::move(notice));
            {
                std::lock_guard<std::mutex> lock(book_mutex);
                known_by_job[job.job_id].notified = true;
            }
        }
    }
};

std::unique_ptr<AsyncToolRuntime> AsyncToolRuntime::Create(Hooks hooks,
                                                           AsyncToolRuntimeOptions options) {
    if (hooks.writer == nullptr || hooks.writer_mutex == nullptr) {
        return nullptr;  // 没有会话账:不装(宿主按未接线走旧路)
    }
    auto runtime = std::unique_ptr<AsyncToolRuntime>(new AsyncToolRuntime());
    runtime->impl_ = std::make_unique<Impl>();
    runtime->impl_->hooks = std::move(hooks);
    runtime->impl_->options = std::move(options);
    Impl* impl = runtime->impl_.get();
    // 桥面查询口缺省绑当前轮桥(宿主显式给的优先,不覆盖)。
    if (!impl->hooks.current_turn_id) {
        impl->hooks.current_turn_id = [impl]() -> std::string {
            std::lock_guard<std::mutex> lock(impl->book_mutex);
            return impl->current_bridge != nullptr ? impl->current_bridge->current_turn_id()
                                                   : std::string();
        };
    }
    if (!impl->hooks.response_evidence) {
        impl->hooks.response_evidence =
            [impl](const std::string& request_id) -> std::optional<std::string> {
                std::lock_guard<std::mutex> lock(impl->book_mutex);
                return impl->current_bridge != nullptr
                           ? impl->current_bridge->V3ResponseEvidenceId(request_id)
                           : std::nullopt;
            };
    }
    if (!impl->hooks.reserved_assistant_message_id) {
        impl->hooks.reserved_assistant_message_id =
            [impl](const std::string& request_id) -> std::optional<std::string> {
                std::lock_guard<std::mutex> lock(impl->book_mutex);
                return impl->current_bridge != nullptr
                           ? impl->current_bridge->V3ReservedAssistantMessageId(request_id)
                           : std::nullopt;
            };
    }
    if (!impl->hooks.call_origin_resolver) {
        impl->hooks.call_origin_resolver =
            [impl](const std::string& call_id) -> std::optional<JobStartRequest> {
                std::lock_guard<std::mutex> lock(impl->book_mutex);
                if (impl->current_bridge == nullptr) {
                    return std::nullopt;
                }
                const auto origin = impl->current_bridge->V3DeclaredCallOrigin(call_id);
                if (!origin.has_value()) {
                    return std::nullopt;
                }
                JobStartRequest request;
                request.turn_id = origin->turn_id;
                request.step_id = origin->step_id;
                request.assistant_message_ref = origin->message_id;
                return request;
            };
    }
    // 协调器与规划器共享会话写者(P1"与主循环共享写者的装配归 P2"落地)。
    runtime->impl_->coordinator_ = std::make_shared<ToolJobCoordinator>(
        *runtime->impl_->hooks.writer, runtime->impl_->hooks.auth,
        runtime->impl_->hooks.executor, runtime->impl_->options.coordinator);
    ResultDeliveryPlannerImpl::Hooks planner_hooks;
    planner_hooks.writer = runtime->impl_->hooks.writer;
    planner_hooks.writer_mutex = runtime->impl_->hooks.writer_mutex;
    planner_hooks.current_turn_id = runtime->impl_->hooks.current_turn_id;
    planner_hooks.response_evidence = runtime->impl_->hooks.response_evidence;
    runtime->impl_->planner_ = std::make_unique<ResultDeliveryPlannerImpl>(std::move(planner_hooks));
    return runtime;
}

void AsyncToolRuntime::InstallTurnBridge(TrajectoryTurnBridge* bridge) {
    std::lock_guard<std::mutex> lock(impl_->book_mutex);
    impl_->current_bridge = bridge;
}

void AsyncToolRuntime::NoteModelIdentity(const std::string& provider, const std::string& model) {
    std::lock_guard<std::mutex> lock(impl_->book_mutex);
    impl_->options.provider = provider;
    impl_->options.model = model;
}

agent::ToolBatchGate* AsyncToolRuntime::gate() { return impl_.get(); }

agent::ResultDeliveryPlanner* AsyncToolRuntime::planner() { return impl_->planner_.get(); }

std::shared_ptr<ToolJobCoordinator> AsyncToolRuntime::coordinator() { return impl_->coordinator_; }

void AsyncToolRuntime::RestoreFromLedger() {
    if (impl_ == nullptr || impl_->hooks.writer == nullptr) {
        return;
    }
    auto ledger = trajectory::v3::ReadV3Ledger(impl_->hooks.writer->path());
    if (!ledger.has_value()) {
        platform::LogSink::Instance().Warn(
            "async-gate", "恢复读账失败,异步欠账不重建: " + ledger.error_or(""));
        return;
    }
    const auto plan = ToolJobCoordinator::PlanRecovery(*ledger);
    impl_->coordinator_->AdoptRecovery(plan);
    impl_->planner_->RestoreFromLedger(*ledger);
}

std::size_t AsyncToolRuntime::early_dispatched_count() const {
    std::lock_guard<std::mutex> lock(impl_->book_mutex);
    std::size_t count = 0;
    for (const auto& [call_id, job] : impl_->known_by_call) {
        (void)call_id;
        if (job.early) {
            ++count;
        }
    }
    return count;
}

bool AttachDefaultAsyncToolRuntime(SessionRuntime& session, const std::string& wire_name) {
    if (session.async_tool_runtime() != nullptr) {
        return true;  // 幂等
    }
    TrajectorySessionLedger* ledger = session.trajectory();
    if (ledger == nullptr || ledger->v3_main_writer() == nullptr) {
        return false;  // v2 场/账没开:不装
    }
    AsyncToolRuntime::Hooks hooks;
    hooks.writer = ledger->v3_main_writer();
    hooks.writer_mutex = ledger->v3_tool_results_mutex();
    AsyncToolRuntimeOptions options;
    options.wire = wire_name;
    auto runtime = AsyncToolRuntime::Create(std::move(hooks), std::move(options));
    if (runtime == nullptr) {
        return false;
    }
    session.AttachAsyncToolRuntime(std::move(runtime));
    return true;
}

}  // namespace lubancode::runtime
