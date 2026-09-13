// tool_job_coordinator.hpp 的实现。单写者纪律:所有 v3 账面追加发生在
// 主锁(jobs_mutex)内的泵/派发/接口路径;worker 线程只经信封锁投递完成
// 信封,不碰 writer。旧租约与终态后的迟到信封拒收(计数,不落账)。
//
// worker 线程模型:派发即 detach,线程闭包持 shared_ptr<Impl>(协调器亡后
// 仍可安全投信封,写到无人消费的队列无害);收场广播取消后有界等待在跑
// 归零,不 join 挂死(AgentTaskCoordinator::JoinAllBounded 同款兜底口径)。
#include "tools/tool_job_coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <thread>
#include <utility>

#include "hooks/hash.hpp"
#include "platform/log_sink.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/result_store.hpp"

namespace lubancode::tools {

namespace {

using trajectory::v3::Durability;
using trajectory::v3::EventDraft;
using trajectory::v3::EventKindV3;
using trajectory::v3::WriteReceipt;

constexpr std::uint64_t kPreviewBudgetBytes = 32768;  // §4.18 预览合同

bool ReceiptOk(const WriteReceipt& receipt) {
    return receipt.status == WriteReceipt::Status::Committed;
}

void NoteWriteFailure(const char* what, const WriteReceipt& receipt) {
    platform::LogSink::Instance().Error(
        "job coordinator", std::string(what) + " 落账失败: " + receipt.error_code + " " +
                               receipt.error_message);
}

// 防御取串:缺键/类型不符回缺省(nlohmann const json 上 operator[] 查缺键
// 是 UB,一律先 find)。
std::string JsonStr(const nlohmann::json& object, const char* key,
                    std::string fallback = std::string()) {
    if (!object.is_object()) {
        return fallback;
    }
    auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

std::string ZeroPad6(std::uint64_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%06llu", static_cast<unsigned long long>(value));
    return buffer;
}

bool IsTerminalJobState(const std::string& state) {
    return state == "succeeded" || state == "failed" || state == "cancelled" ||
           state == "unknown";
}

// 完成信封:worker -> 单写者的内存投递件。带租约代号,不写账。
struct JobCompletionEnvelope {
    std::string job_id;
    std::string owner_epoch;
    bool succeeded = false;
    bool cancelled = false;    // worker 响应取消
    std::string error_code;    // failed 时
    std::string cancel_reason; // cancelled 时
    Tool::Result result;       // 成功载荷(原文走结果仓,预览走 32 KiB 合同)
    std::uint64_t duration_ms = 0;
};

trajectory::v3::ToolActionSession::Terminal IntToTerminal(int value) {
    switch (value) {
        case 1: return trajectory::v3::ToolActionSession::Terminal::Finished;
        case 2: return trajectory::v3::ToolActionSession::Terminal::Failed;
        case 3: return trajectory::v3::ToolActionSession::Terminal::Cancelled;
        case 4: return trajectory::v3::ToolActionSession::Terminal::Rejected;
        case 5: return trajectory::v3::ToolActionSession::Terminal::Unknown;
        default: return trajectory::v3::ToolActionSession::Terminal::None;
    }
}

struct JobRecord {
    std::string job_id;
    std::string action_id;
    std::string turn_id;
    std::string step_id;
    std::string tool_name;
    std::string assistant_message_ref;
    nlohmann::json tool_input;
    JobExecutionPolicy policy;
    trajectory::v3::ToolIdentity identity;

    // 内存执行投影(单 §6):queued|awaiting_approval|running|succeeded|
    // failed|cancelled|unknown|registered(接单链未落稳的恢复缺口)。
    std::string state = "queued";
    int epoch_counter = 0;    // 已发租约数(接管派发接续递增)
    std::string owner_epoch;  // 当前租约(空=未派发)
    bool dispatched = false;
    bool cancel_requested = false;
    bool cancel_event_written = false;
    bool admission_complete = false;  // 接单结果链(含 tool 消息)已落稳
    std::uint64_t deadline_at_ms = 0;

    std::shared_ptr<std::atomic<bool>> cancel_flag =
        std::make_shared<std::atomic<bool>>(false);
    std::optional<trajectory::v3::ToolActionSession> action;

    // 终态结果(泵或恢复后)。
    std::string result_ref;        // 业务 tool.result.persisted 事件 id
    std::uint64_t result_version = 0;
    std::string preview;
    bool preview_truncated = false;
    std::string failure;

    // 落账链中途 IO 失败时暂存信封,下次泵重试(至少一次完成通知,单 §6)。
    std::optional<JobCompletionEnvelope> pending_completion;

    // 恢复补链锚:账上当前 attempt 终态的事件 id(新 session 不带内存态,
    // persisted 的 executionEventRef 要指它)。
    std::string recovered_terminal_event;
};

}  // namespace

// ---------------------------------------------------------------------------
// JobExecutionPolicy
// ---------------------------------------------------------------------------

nlohmann::json JobExecutionPolicy::ToJson() const {
    nlohmann::json value = nlohmann::json::object();
    value["allow_background"] = allow_background;
    value["side_effect_class"] = side_effect_class;
    value["resource_keys"] = resource_keys;
    value["retry_policy"] = retry_policy;
    value["deadline_ms"] = deadline_ms;
    value["resume_policy"] = resume_policy;
    value["max_output_bytes"] = max_output_bytes;
    return value;
}

JobExecutionPolicy JobExecutionPolicy::FromJson(const nlohmann::json& value) {
    JobExecutionPolicy policy;
    if (!value.is_object()) {
        return policy;
    }
    if (value.contains("allow_background") && value["allow_background"].is_boolean()) {
        policy.allow_background = value["allow_background"].get<bool>();
    }
    policy.side_effect_class = JsonStr(value, "side_effect_class", policy.side_effect_class);
    if (value.contains("resource_keys") && value["resource_keys"].is_array()) {
        for (const auto& key : value["resource_keys"]) {
            if (key.is_string() && !key.get<std::string>().empty()) {
                policy.resource_keys.push_back(key.get<std::string>());
            }
        }
    }
    policy.retry_policy = JsonStr(value, "retry_policy", policy.retry_policy);
    if (value.contains("deadline_ms") && value["deadline_ms"].is_number_unsigned()) {
        policy.deadline_ms = value["deadline_ms"].get<std::uint64_t>();
    }
    policy.resume_policy = JsonStr(value, "resume_policy", policy.resume_policy);
    if (value.contains("max_output_bytes") && value["max_output_bytes"].is_number_unsigned()) {
        policy.max_output_bytes = value["max_output_bytes"].get<std::uint64_t>();
    }
    return policy;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct ToolJobCoordinator::Impl {
    trajectory::v3::V3Writer* writer = nullptr;
    JobAuthorizationGate gate;
    JobExecutor executor;
    JobConcurrencyLimits limits;
    std::shared_ptr<GlobalRunningQuota> global;
    std::function<std::int64_t()> clock_ms;
    std::weak_ptr<Impl> self_lock;  // worker 闭包经它拿稳定引用

    // 主锁:jobs/queue/writer 落账/资源占用。信封另设锁——worker 投递不
    // 与主锁交叉(投递侧永不碰 writer),泵在主锁内收信封。
    std::mutex jobs_mutex;
    std::mutex envelope_mutex;
    std::condition_variable state_cv;  // 泵推进/信封到达时唤醒 waiter

    std::map<std::string, std::shared_ptr<JobRecord>> jobs;
    std::vector<std::string> queue;  // FIFO 待派 jobId
    std::uint64_t next_job_number = 1;
    std::deque<JobCompletionEnvelope> envelopes;
    std::uint64_t stale_rejected = 0;
    std::uint64_t duplicate_rejected = 0;
    // 资源键占用:key -> 持有 jobId(单 §7:同键同时至多一个)。
    std::map<std::string, std::string> resource_holders;
    bool closing = false;

    std::int64_t NowMs() const {
        if (clock_ms) {
            return clock_ms();
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    JobAuthDecision CheckGate(const std::string& tool_name, const nlohmann::json& input) const {
        if (!gate) {
            // fail-closed:没挂闸门一律拒(单 §8:jobId 不是访问凭证)。
            return JobAuthDecision{false, false, "authorization gate missing"};
        }
        return gate(tool_name, input);
    }

    // job 的资源键集(串行档未声明 resource_keys 时按工具名单键,单 §7:
    // 文件写/git/同一外部实体默认串行)。
    std::vector<std::string> ResourceKeysOf(const JobRecord& job) const {
        if (!job.policy.serializes_on_resource()) {
            return {};
        }
        if (!job.policy.resource_keys.empty()) {
            return job.policy.resource_keys;
        }
        return {"tool:" + job.tool_name};
    }

    std::size_t RunningCountLocked() const {
        std::size_t count = 0;
        for (const auto& [id, job] : jobs) {
            (void)id;
            if (job->state == "running") {
                ++count;
            }
        }
        return count;
    }

    std::size_t ToolRunningCountLocked(const std::string& tool_name) const {
        std::size_t count = 0;
        for (const auto& [id, job] : jobs) {
            (void)id;
            if (job->state == "running" && job->tool_name == tool_name) {
                ++count;
            }
        }
        return count;
    }

    bool ResourceFreeLocked(const JobRecord& job) const {
        for (const auto& key : ResourceKeysOf(job)) {
            auto it = resource_holders.find(key);
            if (it != resource_holders.end() && it->second != job.job_id) {
                return false;
            }
        }
        return true;
    }

    void AcquireResourcesLocked(JobRecord& job) {
        for (const auto& key : ResourceKeysOf(job)) {
            resource_holders[key] = job.job_id;
        }
    }

    void ReleaseResourcesLocked(JobRecord& job) {
        for (const auto& key : ResourceKeysOf(job)) {
            auto it = resource_holders.find(key);
            if (it != resource_holders.end() && it->second == job.job_id) {
                resource_holders.erase(it);
            }
        }
    }

    // 结果仓:每次临时开(扫目录续号),防两只实例号池错位撞不可变名。
    std::optional<trajectory::v3::ResultStore> OpenStore() {
        auto store = trajectory::v3::ResultStore::Open(writer->path().parent_path());
        if (!store.has_value()) {
            platform::LogSink::Instance().Error("job coordinator",
                                                "结果仓开不了: " + store.error_or(""));
            return std::nullopt;
        }
        return std::move(*store);
    }

    // 文本结果落仓 + 32 KiB 预览(§4.18 合同)。max_output_bytes=0 视为不限。
    struct PersistedText {
        bool ok = false;
        std::vector<nlohmann::json> result_ref;
        trajectory::v3::PreviewResult preview;
        std::string text_artifact_path;
    };

    PersistedText PersistTextLocked(const JobRecord& job, const std::string& channel,
                                    const std::string& text, const std::string& terminal_event_id,
                                    std::uint64_t attempt) {
        PersistedText outcome;
        auto store = OpenStore();
        if (!store.has_value()) {
            return outcome;
        }
        const std::uint64_t cap = job.policy.max_output_bytes;
        trajectory::v3::ResultStore::ChannelOutput output;
        output.channel = channel;
        output.output_bytes = text.size();
        if (cap != 0 && text.size() > cap) {
            // 配额截断(单 §7:输出捕获有界;超限如实记 quota)。
            output.data = text.substr(0, static_cast<std::size_t>(cap));
            output.capture_complete = false;
            output.capture_reason = "quota";
        } else {
            output.data = text;
        }
        // 预览要在 move 进请求前拷一份捕获文本(use-after-move 会拿空串)。
        const std::string captured_text = output.data;
        const bool capture_complete = output.capture_complete;
        const std::string capture_reason = output.capture_reason;
        const std::uint64_t output_bytes = output.output_bytes;
        trajectory::v3::ResultStore::PersistRequest request;
        request.result_kind = "text";
        request.outputs.push_back(std::move(output));
        request.execution_event_ref = terminal_event_id;
        request.capture_limits = nlohmann::json::object({{"max_output_bytes", cap}});
        request.preview_policy = nlohmann::json::object(
            {{"policy", "job-completion"}, {"maxPreviewBytes", kPreviewBudgetBytes}});
        request.tool_call_id = job.action_id;
        request.attempt = attempt;
        auto persisted = store->Persist(request);
        if (!persisted.ok) {
            platform::LogSink::Instance().Error("job coordinator",
                                                "结果仓 Persist 失败: " + persisted.error);
            return outcome;
        }
        outcome.result_ref = std::move(persisted.result_ref);
        // 预览:从 result_ref 找本通道 artifact 路径(§4.17:display_path 用
        // result_ref 里能唯一对应的路径)。
        for (const auto& ref : outcome.result_ref) {
            if (ref.contains("kind") && ref["kind"].is_string() &&
                ref["kind"].get<std::string>() == channel && ref.contains("path") &&
                ref["path"].is_string()) {
                outcome.text_artifact_path = ref["path"].get<std::string>();
                break;
            }
        }
        trajectory::v3::PreviewRequest preview_request;
        trajectory::v3::PreviewChannel preview_channel;
        preview_channel.display_path = outcome.text_artifact_path.empty()
                                           ? "artifacts/" + persisted.result_id
                                           : outcome.text_artifact_path;
        preview_channel.channel = channel;
        preview_channel.text = captured_text;
        preview_channel.capture_complete = capture_complete;
        preview_channel.capture_reason = capture_reason;
        preview_channel.output_bytes = output_bytes;
        preview_request.channels.push_back(std::move(preview_channel));
        preview_request.max_preview_bytes = kPreviewBudgetBytes;
        outcome.preview = trajectory::v3::BuildToolPreview(preview_request);
        outcome.ok = true;
        return outcome;
    }

    // tool.job.observed 事件(observed 不要求 attempt,单 §5 定案 4)。
    WriteReceipt ObserveLocked(const JobRecord& job, const std::string& observed_status,
                               std::optional<std::string> result_ref = std::nullopt,
                               std::uint64_t result_version = 0) {
        EventDraft draft;
        draft.kind = EventKindV3::ToolJobObserved;
        draft.turn_id = job.turn_id;
        draft.step_id = job.step_id;
        draft.action_id = job.action_id;
        nlohmann::json payload =
            nlohmann::json::object({{"tool_call_id", job.action_id},
                                    {"jobId", job.job_id},
                                    {"observedStatus", observed_status}});
        if (result_ref.has_value()) {
            payload["resultRef"] = *result_ref;
            payload["resultVersion"] = result_version;
        }
        draft.payload = std::move(payload);
        return writer->AppendEvent(std::move(draft), Durability::PowerLoss);
    }

    WriteReceipt EmitCancelRequestedLocked(const JobRecord& job, const std::string& reason) {
        EventDraft draft;
        draft.kind = EventKindV3::ToolJobCancelRequested;
        draft.turn_id = job.turn_id;
        draft.step_id = job.step_id;
        draft.action_id = job.action_id;
        draft.payload = nlohmann::json::object(
            {{"tool_call_id", job.action_id}, {"jobId", job.job_id}, {"reason", reason}});
        return writer->AppendEvent(std::move(draft), Durability::PowerLoss);
    }

    // 接单结果链(单 §5/§8:start 的接单结果即配齐调用):attempt 1 的
    // started/finished + persisted/selected + 接单 tool 消息(接纳随消息)。
    // 恢复补链走同一条路:session 已按账面对齐(ReopenAligned),不会对
    // 账上已有的 started/终态重复落事件。
    bool WriteAdmissionChainLocked(JobRecord& job) {
        if (job.admission_complete) {
            return true;
        }
        if (!job.action.has_value()) {
            return false;
        }
        // 参数摘要指纹(同 inline 主路口径:原文已在声明块,不另存 blob)。
        auto canonical = trajectory::CanonicalJsonDump(job.tool_input);
        const std::string args_hash = canonical.has_value()
                                          ? hooks::Sha256Hex(*canonical)
                                          : hooks::Sha256Hex(job.tool_input.dump());
        const std::string args_ref = "args-" + job.action_id + "-" + args_hash.substr(0, 16);
        if (!job.action->started()) {
            auto started = job.action->Start(*writer, args_ref, job.identity, std::nullopt,
                                             nlohmann::json{{"toolName", job.tool_name}});
            if (!ReceiptOk(started)) {
                NoteWriteFailure("tool.execution.started(接单)", started);
                return false;
            }
        }
        if (job.action->terminal() == trajectory::v3::ToolActionSession::Terminal::None) {
            auto finished = job.action->Finish(*writer, std::nullopt, std::nullopt);
            if (!ReceiptOk(finished)) {
                NoteWriteFailure("tool.execution.finished(接单)", finished);
                return false;
            }
        }
        // 接单正文:{"jobId":...,"status":...}(P0 fixture 同款形状)。
        const std::string admission_text =
            nlohmann::json::object({{"jobId", job.job_id},
                                    {"status", job.state == "awaiting_approval"
                                                   ? "awaiting_approval"
                                                   : "queued"}})
                .dump();
        // 接单 persisted 的 executionEventRef 指向 attempt 1 终态:新起
        // session 用内存 last_event_id,恢复补链用账面锚。
        std::string finished_event = job.action->last_event_id().value_or("");
        if (finished_event.empty()) {
            finished_event = job.recovered_terminal_event;
        }
        if (finished_event.empty()) {
            return false;
        }
        auto persisted_text = PersistTextLocked(job, "combined", admission_text, finished_event, 1);
        if (!persisted_text.ok) {
            return false;
        }
        auto persisted =
            job.action->PersistedResult(*writer, persisted_text.result_ref, finished_event, 1);
        if (!ReceiptOk(persisted)) {
            NoteWriteFailure("tool.result.persisted(接单)", persisted);
            return false;
        }
        auto selected = job.action->SelectResult(*writer, {persisted.id}, {}, "done", 1);
        if (!ReceiptOk(selected)) {
            NoteWriteFailure("tool.result.selected(接单)", selected);
            return false;
        }
        auto message = job.action->AppendToolMessage(*writer, persisted_text.preview.text,
                                                     selected.id, false);
        if (!ReceiptOk(message)) {
            NoteWriteFailure("接单 tool 消息", message);
            return false;
        }
        job.admission_complete = true;
        return true;
    }

    // 派发(单 §5:注册/接单落稳后才走到这):复查授权与取消 -> 新租约
    // dispatched -> attempt 2 执行链起跑 -> worker 线程(detach)。
    enum class DispatchOutcome { Dispatched, KeepQueued, Cancelled, Failed };
    DispatchOutcome DispatchJobLocked(JobRecord& job) {
        // 执行前查取消状态(单 §7:不因入队时获准就永久放行)。
        if (job.cancel_requested) {
            if (!job.cancel_event_written) {
                auto event = EmitCancelRequestedLocked(job, "cancelled_before_dispatch");
                if (!ReceiptOk(event)) {
                    NoteWriteFailure("tool.job.cancel_requested", event);
                    return DispatchOutcome::KeepQueued;
                }
                job.cancel_event_written = true;
            }
            auto observed = ObserveLocked(job, "cancelled");
            if (!ReceiptOk(observed)) {
                NoteWriteFailure("tool.job.observed(cancelled)", observed);
                return DispatchOutcome::KeepQueued;
            }
            job.state = "cancelled";
            return DispatchOutcome::Cancelled;
        }
        // 执行前复查授权有效期(单 §7)。
        JobAuthDecision auth = CheckGate(job.tool_name, job.tool_input);
        if (!auth.allowed) {
            // 未派发的复查拒绝:开 attempt 2 收 rejected(无执行发生),
            // job 观测按 failed 收口(单 §6 执行投影没有 rejected 态)。
            auto pending = job.action->BeginNextAttempt(*writer, "job_dispatch");
            if (!ReceiptOk(pending)) {
                NoteWriteFailure("tool.execution.pending(attempt 2)", pending);
                return DispatchOutcome::KeepQueued;
            }
            auto rejected = job.action->Reject(
                *writer, auth.reason.empty() ? "authorization_denied_at_dispatch" : auth.reason);
            if (!ReceiptOk(rejected)) {
                NoteWriteFailure("tool.execution.rejected", rejected);
                return DispatchOutcome::KeepQueued;
            }
            auto observed = ObserveLocked(job, "failed");
            if (!ReceiptOk(observed)) {
                NoteWriteFailure("tool.job.observed(failed)", observed);
                return DispatchOutcome::KeepQueued;
            }
            job.state = "failed";
            job.failure = auth.reason.empty() ? "authorization_denied_at_dispatch" : auth.reason;
            return DispatchOutcome::Failed;
        }
        // 配额与资源锁(单 §7:全局/Session/工具/资源键;0=不设限)。
        if (!global->TryAcquire()) {
            return DispatchOutcome::KeepQueued;
        }
        if (limits.session_running != 0 &&
            RunningCountLocked() + 1 > limits.session_running) {
            global->Release();
            return DispatchOutcome::KeepQueued;
        }
        if (limits.per_tool != 0 && ToolRunningCountLocked(job.tool_name) + 1 > limits.per_tool) {
            global->Release();
            return DispatchOutcome::KeepQueued;
        }
        if (!ResourceFreeLocked(job)) {
            global->Release();
            return DispatchOutcome::KeepQueued;
        }
        if (!executor) {
            global->Release();
            return DispatchOutcome::KeepQueued;
        }
        // 调度意图先落账,再起线程(注册落稳前不派发的同款纪律:
        // dispatched 落稳前不起 worker)。
        auto pending = job.action->BeginNextAttempt(*writer, "job_dispatch");
        if (!ReceiptOk(pending)) {
            NoteWriteFailure("tool.execution.pending(attempt 2)", pending);
            global->Release();
            return DispatchOutcome::KeepQueued;
        }
        job.epoch_counter += 1;
        job.owner_epoch = "epoch-" + std::to_string(job.epoch_counter);
        EventDraft dispatched;
        dispatched.kind = EventKindV3::ToolJobDispatched;
        dispatched.turn_id = job.turn_id;
        dispatched.step_id = job.step_id;
        dispatched.action_id = job.action_id;
        dispatched.payload =
            nlohmann::json::object({{"tool_call_id", job.action_id},
                                    {"attempt", job.action->attempt()},
                                    {"jobId", job.job_id},
                                    {"ownerEpoch", job.owner_epoch}});
        auto dispatched_receipt = writer->AppendEvent(std::move(dispatched), Durability::PowerLoss);
        if (!ReceiptOk(dispatched_receipt)) {
            NoteWriteFailure("tool.job.dispatched", dispatched_receipt);
            global->Release();
            return DispatchOutcome::KeepQueued;
        }
        auto canonical = trajectory::CanonicalJsonDump(job.tool_input);
        const std::string args_hash = canonical.has_value()
                                          ? hooks::Sha256Hex(*canonical)
                                          : hooks::Sha256Hex(job.tool_input.dump());
        const std::string args_ref = "args-" + job.action_id + "-" + args_hash.substr(0, 16);
        const std::string idempotency_key = trajectory::v3::ComputeToolIdempotencyKey(
            job.action_id, job.identity, args_hash, "");
        auto started = job.action->Start(*writer, args_ref, job.identity, idempotency_key,
                                         nlohmann::json{{"toolName", job.tool_name}});
        if (!ReceiptOk(started)) {
            NoteWriteFailure("tool.execution.started(attempt 2)", started);
            global->Release();
            return DispatchOutcome::KeepQueued;
        }
        job.dispatched = true;
        job.state = "running";
        const std::int64_t now = NowMs();
        job.deadline_at_ms =
            job.policy.deadline_ms == 0 ? 0 : static_cast<std::uint64_t>(now + static_cast<std::int64_t>(job.policy.deadline_ms));
        AcquireResourcesLocked(job);
        job.cancel_flag->store(false);
        // worker:值拷贝材料,终局只投信封(不碰 writer);detach,闭包持
        // shared_ptr<Impl> 保活,协调器亡后投递仍安全。
        std::shared_ptr<Impl> self = self_lock.lock();
        if (self == nullptr) {
            global->Release();
            ReleaseResourcesLocked(job);
            return DispatchOutcome::KeepQueued;
        }
        JobExecutionContext context;
        context.job_id = job.job_id;
        context.input = job.tool_input;
        context.max_output_bytes = job.policy.max_output_bytes;
        const std::string job_id = job.job_id;
        const std::string epoch = job.owner_epoch;
        const std::shared_ptr<std::atomic<bool>> cancel_flag = job.cancel_flag;
        const JobExecutor run = executor;
        const std::int64_t started_at = now;
        std::thread([self, job_id, epoch, context, cancel_flag, run, started_at]() {
            JobExecutionContext local = context;
            local.cancel = cancel_flag.get();
            Tool::Result result = run(local);
            JobCompletionEnvelope envelope;
            envelope.job_id = job_id;
            envelope.owner_epoch = epoch;
            envelope.result = std::move(result);
            envelope.succeeded = !envelope.result.is_error;
            if (envelope.result.is_error) {
                envelope.error_code = envelope.result.error_code.empty()
                                          ? "tool_failed"
                                          : envelope.result.error_code;
            }
            envelope.duration_ms = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, self->NowMs() - started_at));
            // worker 响应取消:旗子置位且执行以错误收尾,按 cancelled 投递
            //(单写者收唯一终态;跑完的正常结果仍按 succeeded 收)。
            if (cancel_flag->load() && envelope.result.is_error) {
                envelope.cancelled = true;
                envelope.succeeded = false;
                envelope.cancel_reason = "worker_saw_cancel";
            }
            {
                std::lock_guard<std::mutex> lock(self->envelope_mutex);
                self->envelopes.push_back(std::move(envelope));
            }
            self->state_cv.notify_all();
        }).detach();
        return DispatchOutcome::Dispatched;
    }

    // 队列泵:尽力派发队首可派者(配额/资源释放后由终态路径再调)。
    void TryDispatchLocked() {
        for (std::size_t i = 0; i < queue.size();) {
            auto it = jobs.find(queue[i]);
            if (it == jobs.end() ||
                (it->second->state != "queued" && it->second->state != "registered")) {
                queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            JobRecord& job = *it->second;
            DispatchOutcome outcome = DispatchJobLocked(job);
            if (outcome == DispatchOutcome::Dispatched || outcome == DispatchOutcome::Cancelled ||
                outcome == DispatchOutcome::Failed) {
                queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;  // 暂不可派(配额/资源),保留队位
            }
        }
    }

    // 单写者收一枚信封:校验租约与终态唯一,再落账(单 §5/§6)。
    void DrainEnvelopeLocked(const JobCompletionEnvelope& envelope) {
        auto it = jobs.find(envelope.job_id);
        if (it == jobs.end()) {
            return;  // 未知 job(协调器没接管):丢弃
        }
        JobRecord& job = *it->second;
        if (IsTerminalJobState(job.state)) {
            // 合法终态只接纳一次(单 §6):取消与完成竞态由先到者收口,
            // 迟到信封拒收不落账。
            duplicate_rejected += 1;
            return;
        }
        if (envelope.owner_epoch != job.owner_epoch) {
            // 旧 worker/双重恢复的迟到信封:拒收,不落终态不落观测。
            stale_rejected += 1;
            return;
        }
        if (!job.action.has_value()) {
            return;
        }
        // 落账链(单 §5 持久顺序):执行终态 -> 原文落仓 -> 终态观测。
        WriteReceipt terminal_receipt;
        std::string terminal_state;
        if (envelope.cancelled) {
            terminal_receipt =
                job.action->Cancel(*writer, "during_execution",
                                   envelope.cancel_reason.empty() ? "cancelled"
                                                                  : envelope.cancel_reason);
            terminal_state = "cancelled";
        } else if (envelope.succeeded) {
            terminal_receipt = job.action->Finish(*writer, std::nullopt, envelope.duration_ms);
            terminal_state = "succeeded";
        } else {
            terminal_receipt = job.action->Fail(*writer, envelope.error_code, envelope.duration_ms);
            terminal_state = "failed";
        }
        if (!ReceiptOk(terminal_receipt)) {
            NoteWriteFailure("tool.execution 终态(attempt 2)", terminal_receipt);
            job.pending_completion = envelope;  // 至少一次完成通知(单 §6)
            return;
        }
        std::string result_ref;
        std::uint64_t result_version = 0;
        std::string preview;
        bool preview_truncated = false;
        if (envelope.succeeded) {
            auto persisted = PersistTextLocked(job, "combined", envelope.result.content,
                                               terminal_receipt.id, job.action->attempt());
            if (!persisted.ok) {
                // 执行已终态、结果链没立起来:保留 done,另报持久化失败
                //(§4.18),观测先收 succeeded 不带 resultRef。
                auto failed_note = job.action->PersistFailed(*writer, "result_store_write_failed");
                if (!ReceiptOk(failed_note)) {
                    NoteWriteFailure("tool.result.persist_failed", failed_note);
                }
                platform::LogSink::Instance().Error(
                    "job coordinator", "业务结果落仓失败(job " + job.job_id + "),保留执行终态");
            } else {
                auto persisted_event = job.action->PersistedResult(
                    *writer, persisted.result_ref, terminal_receipt.id, std::nullopt);
                if (!ReceiptOk(persisted_event)) {
                    NoteWriteFailure("tool.result.persisted(业务)", persisted_event);
                } else {
                    result_ref = persisted_event.id;
                    result_version = 1;
                    preview = persisted.preview.text;
                    preview_truncated = persisted.preview.truncated;
                }
            }
        }
        WriteReceipt observed = result_ref.empty()
                                     ? ObserveLocked(job, terminal_state)
                                     : ObserveLocked(job, terminal_state, result_ref, result_version);
        if (!ReceiptOk(observed)) {
            NoteWriteFailure("tool.job.observed(终态)", observed);
            job.pending_completion = envelope;
            return;
        }
        job.state = terminal_state;
        job.result_ref = result_ref;
        job.result_version = result_version;
        job.preview = preview;
        job.preview_truncated = preview_truncated;
        job.failure = envelope.cancelled ? envelope.cancel_reason
                      : envelope.succeeded ? std::string()
                                           : envelope.error_code;
        job.pending_completion.reset();
        ReleaseResourcesLocked(job);
        global->Release();
    }

    // deadline 巡检(单 §6:超时先请求取消,不把仍可能运行的副作用判失败;
    // 惰性驱动——泵路径顺带查,不起监控线程)。
    void CheckDeadlinesLocked() {
        const std::int64_t now = NowMs();
        for (auto& [id, job] : jobs) {
            (void)id;
            if (job->state != "running" || job->deadline_at_ms == 0) {
                continue;
            }
            if (static_cast<std::int64_t>(job->deadline_at_ms) > now) {
                continue;
            }
            if (!job->cancel_event_written) {
                auto event = EmitCancelRequestedLocked(*job, "deadline_exceeded");
                if (ReceiptOk(event)) {
                    job->cancel_event_written = true;
                    job->cancel_requested = true;
                    job->cancel_flag->store(true);
                } else {
                    NoteWriteFailure("tool.job.cancel_requested(deadline)", event);
                }
            }
        }
    }

    // 泵:deadline 巡检 -> 重试上次落账失败的信封 -> 收割新信封 -> 队列
    // 让位。返回本次收口的终态数。
    std::size_t PumpLocked() {
        CheckDeadlinesLocked();
        std::size_t settled = 0;
        for (auto& [id, job] : jobs) {
            (void)id;
            if (job->pending_completion.has_value() && !IsTerminalJobState(job->state)) {
                const JobCompletionEnvelope retry = *job->pending_completion;
                const std::string before = job->state;
                DrainEnvelopeLocked(retry);
                if (job->state != before && IsTerminalJobState(job->state)) {
                    ++settled;
                }
            }
        }
        std::deque<JobCompletionEnvelope> incoming;
        {
            std::lock_guard<std::mutex> lock(envelope_mutex);
            incoming.swap(envelopes);
        }
        while (!incoming.empty()) {
            JobCompletionEnvelope envelope = std::move(incoming.front());
            incoming.pop_front();
            auto it = jobs.find(envelope.job_id);
            const std::string before = it == jobs.end() ? std::string() : it->second->state;
            DrainEnvelopeLocked(envelope);
            if (it != jobs.end() && it->second->state != before &&
                IsTerminalJobState(it->second->state)) {
                ++settled;
            }
        }
        if (settled > 0) {
            TryDispatchLocked();
        }
        return settled;
    }
};

// ---------------------------------------------------------------------------
// 构造/析构
// ---------------------------------------------------------------------------

ToolJobCoordinator::ToolJobCoordinator(trajectory::v3::V3Writer& writer,
                                       JobAuthorizationGate gate, JobExecutor executor,
                                       Options options)
    : impl_(std::make_shared<Impl>()) {
    impl_->writer = &writer;
    impl_->gate = std::move(gate);
    impl_->executor = std::move(executor);
    impl_->limits = options.limits;
    impl_->global = options.global != nullptr ? std::move(options.global)
                                              : std::make_shared<GlobalRunningQuota>();
    impl_->clock_ms = std::move(options.clock_ms);
    impl_->self_lock = impl_;
}

ToolJobCoordinator::~ToolJobCoordinator() {
    if (impl_ == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
        impl_->closing = true;
        // 广播取消(单 §9 shutdown:短命线程不承诺跨进程存活):在跑的一律
        // 置旗;detach 线程持 Impl 强引用,协调器亡后投递仍安全。
        for (auto& [id, job] : impl_->jobs) {
            (void)id;
            if (job->state == "running" || job->state == "queued") {
                job->cancel_flag->store(true);
            }
        }
    }
    // 有界等在跑归零(worker 见旗收工,泵顺带收终态);到点放行,不 join
    // 挂死(detach 线程持 Impl 强引用,投递安全)。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
            impl_->PumpLocked();  // 收尾也要收信封,否则终态永不落地
            if (impl_->RunningCountLocked() == 0) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ---------------------------------------------------------------------------
// 四接口
// ---------------------------------------------------------------------------

JobStartResult ToolJobCoordinator::StartJob(const JobStartRequest& request) {
    JobStartResult result;
    if (request.tool_name.empty() || request.turn_id.empty() || request.step_id.empty() ||
        request.assistant_message_ref.empty()) {
        // originRef 三件(信封 turnId/stepId + payload assistantMessageRef)
        // 是 registered 的载荷合同,缺一不注册(单 §5)。
        result.error_code = "job.start.bad_request";
        result.error = "tool_name/turn_id/step_id/assistant_message_ref 不得为空";
        return result;
    }
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    if (impl_->closing) {
        result.error_code = "job.start.closing";
        result.error = "协调器已收场";
        return result;
    }
    if (impl_->queue.size() >= impl_->limits.queued_max) {
        result.error_code = "job.start.queue_full";
        result.error = "待派队列已满(queued_max=" + std::to_string(impl_->limits.queued_max) + ")";
        return result;
    }
    // 调用证据(tool.execution.pending):注册的前置(单 §5 持久顺序)。
    const std::string job_id = "job-" + ZeroPad6(impl_->next_job_number);
    const std::string action_id = "action-job-" + ZeroPad6(impl_->next_job_number);
    impl_->next_job_number += 1;
    auto record = std::make_shared<JobRecord>();
    record->job_id = job_id;
    record->action_id = action_id;
    record->turn_id = request.turn_id;
    record->step_id = request.step_id;
    record->tool_name = request.tool_name;
    record->assistant_message_ref = request.assistant_message_ref;
    record->tool_input = request.tool_input;
    record->policy = request.policy;
    record->identity.logical_name = request.tool_name;
    record->identity.registration_source = "host_job_service";
    record->action = trajectory::v3::ToolActionSession::Admit(
        *impl_->writer, request.turn_id, request.step_id, action_id, "queued",
        std::optional<std::string>(request.assistant_message_ref), std::nullopt,
        nlohmann::json{{"toolName", request.tool_name}}, Durability::ProcessCrash);
    if (!record->action->last_event_id().has_value()) {
        result.error_code = "job.start.evidence_write_failed";
        result.error = "tool.execution.pending 落账失败,不注册";
        return result;
    }
    // 权鉴(单 §8:jobId 不是访问凭证;fail-closed)。
    JobAuthDecision auth = impl_->CheckGate(request.tool_name, request.tool_input);
    if (!auth.allowed && !auth.needs_approval) {
        auto rejected = record->action->Reject(
            *impl_->writer, auth.reason.empty() ? "authorization_denied" : auth.reason);
        if (!ReceiptOk(rejected)) {
            NoteWriteFailure("tool.execution.rejected", rejected);
        }
        result.error_code = "job.start.denied";
        result.error = auth.reason.empty() ? "授权闸门拒绝" : auth.reason;
        return result;
    }
    // 注册落稳(单 §5:注册落稳前不派发)。executionPolicy 落持久档案。
    EventDraft registered;
    registered.kind = EventKindV3::ToolJobRegistered;
    registered.turn_id = request.turn_id;
    registered.step_id = request.step_id;
    registered.action_id = action_id;
    nlohmann::json payload =
        nlohmann::json::object({{"tool_call_id", action_id},
                                {"attempt", 1},
                                {"jobId", job_id},
                                {"mode", "job_handle"},
                                {"assistantMessageRef", request.assistant_message_ref}});
    if (auth.needs_approval) {
        payload["approvalRequired"] = true;
    }
    payload["executionPolicy"] = request.policy.ToJson();
    registered.payload = std::move(payload);
    auto registered_receipt = impl_->writer->AppendEvent(std::move(registered), Durability::PowerLoss);
    if (!ReceiptOk(registered_receipt)) {
        // 注册没落稳:不派发、不接单,action 收口 failed。
        NoteWriteFailure("tool.job.registered", registered_receipt);
        record->action->Fail(*impl_->writer, "job_register_write_failed");
        result.error_code = "job.start.register_write_failed";
        result.error = registered_receipt.error_message;
        return result;
    }
    record->state = auth.needs_approval ? "awaiting_approval" : "queued";
    impl_->jobs[job_id] = record;
    // 接单结果链(单 §8:start 的接单结果即配齐调用)。审批未过也接单
    //(消息如实报 awaiting_approval),但不入队(单 §6:审批未过不派发)。
    if (!impl_->WriteAdmissionChainLocked(*record)) {
        result.error_code = "job.start.admission_write_failed";
        result.error = "接单结果链落账失败;job 已注册,恢复按 complete_delivery 补链";
        result.job_id = job_id;
        result.status = record->state = "registered";  // 不入队:接单没配齐不派发
        impl_->state_cv.notify_all();
        return result;
    }
    if (record->state == "queued") {
        impl_->queue.push_back(job_id);
        impl_->TryDispatchLocked();
    }
    impl_->PumpLocked();
    impl_->state_cv.notify_all();
    result.ok = true;
    result.job_id = job_id;
    result.status = record->state;
    return result;
}

JobStartResult ToolJobCoordinator::GrantApproval(const std::string& job_id) {
    JobStartResult result;
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    auto it = impl_->jobs.find(job_id);
    if (it == impl_->jobs.end()) {
        result.error_code = "job.grant.unknown_job";
        result.error = "job 不在本协调器: " + job_id;
        return result;
    }
    JobRecord& job = *it->second;
    if (job.state != "awaiting_approval") {
        result.error_code = "job.grant.not_awaiting";
        result.error = "job 状态是 " + job.state + ",不在审批挂起";
        return result;
    }
    // 审批随派发隐式放行(P0 折叠口径):内存转 queued 入队,不落账。
    job.state = "queued";
    impl_->queue.push_back(job_id);
    impl_->TryDispatchLocked();
    result.ok = true;
    result.job_id = job_id;
    result.status = job.state;
    return result;
}

JobStatusView ToolJobCoordinator::GetJob(const std::string& job_id) {
    JobStatusView view;
    view.job_id = job_id;
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    impl_->PumpLocked();
    auto it = impl_->jobs.find(job_id);
    if (it == impl_->jobs.end()) {
        view.state = "unknown_job";
        return view;
    }
    JobRecord& job = *it->second;
    // jobId 不是访问凭证(单 §8):读取同样过闸门。不自动重跑:重复 Get
    // 零副作用,终态带 resultRef 与 ≤32 KiB 有界预览。
    JobAuthDecision auth = impl_->CheckGate(job.tool_name, job.tool_input);
    if (!auth.allowed) {
        view.access_denied = true;
        view.access_reason = auth.reason.empty() ? "授权闸门拒绝" : auth.reason;
        return view;
    }
    view.state = job.state;
    view.cancel_requested = job.cancel_requested;
    view.result_ref = job.result_ref;
    view.result_version = job.result_version;
    view.preview = job.preview;
    view.preview_truncated = job.preview_truncated;
    view.failure = job.failure;
    return view;
}

JobWaitResult ToolJobCoordinator::WaitJobs(const std::vector<std::string>& job_ids,
                                           std::uint64_t timeout_ms, bool wait_all) {
    JobWaitResult result;
    if (job_ids.empty()) {
        result.satisfied = true;
        return result;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(impl_->jobs_mutex);
    for (;;) {
        impl_->PumpLocked();
        // 状态快照(游标=本次观测,单 §8:超时回 pending+游标不宣告失败)。
        std::size_t terminal = 0;
        result.statuses.clear();
        for (const auto& id : job_ids) {
            JobStatusView view;
            view.job_id = id;
            auto it = impl_->jobs.find(id);
            if (it == impl_->jobs.end()) {
                view.state = "unknown_job";
            } else {
                JobRecord& job = *it->second;
                JobAuthDecision auth = impl_->CheckGate(job.tool_name, job.tool_input);
                if (!auth.allowed) {
                    view.access_denied = true;
                    view.access_reason = auth.reason.empty() ? "授权闸门拒绝" : auth.reason;
                } else {
                    view.state = job.state;
                    view.cancel_requested = job.cancel_requested;
                    view.result_ref = job.result_ref;
                    view.result_version = job.result_version;
                    view.preview = job.preview;
                    view.preview_truncated = job.preview_truncated;
                    view.failure = job.failure;
                }
            }
            if (IsTerminalJobState(view.state)) {
                ++terminal;
            }
            result.statuses.push_back(std::move(view));
        }
        const bool satisfied = wait_all ? terminal == job_ids.size() : terminal > 0;
        if (satisfied) {
            result.satisfied = true;
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            return result;
        }
        impl_->state_cv.wait_until(lock, deadline);
    }
}

JobCancelResult ToolJobCoordinator::CancelJob(const std::string& job_id,
                                              const std::string& reason) {
    JobCancelResult result;
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    auto it = impl_->jobs.find(job_id);
    if (it == impl_->jobs.end()) {
        result.status = "unknown_job";
        result.error = "job 不在本协调器: " + job_id;
        return result;
    }
    JobRecord& job = *it->second;
    // 取消也要过闸门(jobId 不是访问凭证)。
    JobAuthDecision auth = impl_->CheckGate(job.tool_name, job.tool_input);
    if (!auth.allowed) {
        result.status = "denied";
        result.error = auth.reason.empty() ? "授权闸门拒绝" : auth.reason;
        return result;
    }
    if (IsTerminalJobState(job.state)) {
        result.ok = true;
        result.status = "already_terminal";
        result.terminal = job.state;
        return result;
    }
    const std::string effective_reason = reason.empty() ? "user_cancel" : reason;
    // 取消是请求不是终态(单 §8):先落 cancel_requested。
    if (!job.cancel_event_written) {
        auto event = impl_->EmitCancelRequestedLocked(job, effective_reason);
        if (!ReceiptOk(event)) {
            NoteWriteFailure("tool.job.cancel_requested", event);
            result.status = "write_failed";
            result.error = event.error_message;
            return result;
        }
        job.cancel_event_written = true;
    }
    job.cancel_requested = true;
    job.cancel_flag->store(true);
    if (!job.dispatched) {
        // 未派发:当场收终态(唯一终态;无执行发生)。attempt 2 未开,
        // 不落执行事件,job 线以 observed(cancelled) 收口。
        auto observed = impl_->ObserveLocked(job, "cancelled");
        if (!ReceiptOk(observed)) {
            NoteWriteFailure("tool.job.observed(cancelled)", observed);
            result.status = "write_failed";
            result.error = observed.error_message;
            return result;
        }
        job.state = "cancelled";
    }
    // 已派发:不保证终止;worker 信封(响应取消或跑完)由泵收唯一终态
    //(真实完成不改写成"未执行",单 §6)。
    result.ok = true;
    result.status = "cancel_requested";
    result.terminal = job.state;
    impl_->state_cv.notify_all();
    return result;
}

std::size_t ToolJobCoordinator::PumpCompletions() {
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    std::size_t settled = impl_->PumpLocked();
    impl_->state_cv.notify_all();
    return settled;
}

bool ToolJobCoordinator::DebugSubmitEnvelope(const std::string& job_id,
                                             const std::string& owner_epoch, Tool::Result result) {
    JobCompletionEnvelope envelope;
    envelope.job_id = job_id;
    envelope.owner_epoch = owner_epoch;
    envelope.result = std::move(result);
    envelope.succeeded = !envelope.result.is_error;
    envelope.cancelled = false;
    if (envelope.result.is_error) {
        envelope.error_code =
            envelope.result.error_code.empty() ? "tool_failed" : envelope.result.error_code;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->envelope_mutex);
        impl_->envelopes.push_back(std::move(envelope));
    }
    impl_->state_cv.notify_all();
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    auto existing = impl_->jobs.find(job_id);
    const bool was_terminal =
        existing != impl_->jobs.end() && IsTerminalJobState(existing->second->state);
    impl_->PumpLocked();
    const auto it = impl_->jobs.find(job_id);
    const bool accepted =
        it != impl_->jobs.end() && !was_terminal && IsTerminalJobState(it->second->state);
    return accepted;
}

// ---------------------------------------------------------------------------
// 恢复(单 §6 表;纯读账定计划,账态注入可重复)
// ---------------------------------------------------------------------------

JobRecoveryPlan ToolJobCoordinator::PlanRecovery(const trajectory::v3::V3Ledger& ledger) {
    JobRecoveryPlan plan;
    const auto jobs = trajectory::v3::FoldJobExecutions(ledger);
    const auto actions = trajectory::v3::FoldToolActions(ledger);
    // 注册载荷(执行策略/turn/step)与派发计数(epoch 接续)。
    std::map<std::string, const trajectory::v3::EventLine*> registered;
    std::map<std::string, int> dispatch_counts;
    // 业务结果(同 action、attempt>1 的最后一条 persisted)。
    std::map<std::string, std::string> business_persisted;
    // 末枚 attempt 的终态事件 id(补链引用锚):actionId -> (attempt, eventId)。
    std::map<std::string, std::pair<std::uint64_t, std::string>> terminal_events;
    for (const auto& event : ledger.events) {
        const std::string job_id = JsonStr(event.payload, "jobId");
        if (event.kind == EventKindV3::ToolJobRegistered) {
            registered[job_id] = &event;
        } else if (event.kind == EventKindV3::ToolJobDispatched) {
            dispatch_counts[job_id] += 1;
        } else if (event.kind == EventKindV3::ToolResultPersisted && event.action_id.has_value()) {
            const auto attempt_it = event.payload.find("attempt");
            if (attempt_it != event.payload.end() && attempt_it->is_number_unsigned() &&
                attempt_it->get<std::uint64_t>() > 1) {
                business_persisted[*event.action_id] = event.event_id;
            }
        } else if (event.action_id.has_value() &&
                   (event.kind == EventKindV3::ToolExecutionFinished ||
                    event.kind == EventKindV3::ToolExecutionFailed ||
                    event.kind == EventKindV3::ToolExecutionCancelled ||
                    event.kind == EventKindV3::ToolExecutionRejected ||
                    event.kind == EventKindV3::ToolExecutionUnknown)) {
            const auto attempt_it = event.payload.find("attempt");
            if (attempt_it != event.payload.end() && attempt_it->is_number_unsigned()) {
                terminal_events[*event.action_id] =
                    std::make_pair(attempt_it->get<std::uint64_t>(), event.event_id);
            }
        }
    }
    for (const auto& job : jobs) {
        JobRecoveryPlan::Item item;
        item.job_id = job.job_id;
        item.action_id = job.origin_action_id;
        item.mode = job.mode;
        auto reg = registered.find(job.job_id);
        if (reg != registered.end()) {
            const auto policy_it = reg->second->payload.find("executionPolicy");
            if (policy_it != reg->second->payload.end()) {
                item.policy = JobExecutionPolicy::FromJson(policy_it.value());
            }
            item.turn_id = reg->second->turn_id.value_or("");
            item.step_id = reg->second->step_id.value_or("");
        }
        item.assistant_message_ref = job.assistant_message_ref.value_or("");
        item.dispatched_count = dispatch_counts.count(job.job_id) != 0
                                    ? dispatch_counts[job.job_id]
                                    : 0;
        const trajectory::v3::ToolActionSnapshot* snap =
            trajectory::v3::FindActionSnapshot(actions, job.origin_action_id);
        if (snap != nullptr) {
            item.tool_name = snap->tool_name.value_or("");
            item.tool_input =
                snap->declared_args.has_value() ? *snap->declared_args : nlohmann::json::object();
            if (!snap->attempts.empty()) {
                // 账面对齐取末枚 attempt 的折叠事实:接单链(attempt 1)
                // 补链用;接管过的重派(attempt>1)以末枚对齐后由
                // BeginNextAttempt 接续。
                const auto& last = snap->attempts.back();
                item.attempt = last.attempt;
                item.attempt_started = last.started;
                item.attempt_terminal = last.status == "done"       ? 1
                                        : last.status == "failed"    ? 2
                                        : last.status == "cancelled" ? 3
                                        : last.status == "rejected"  ? 4
                                        : last.status == "unknown"   ? 5
                                       : 0;
            }
        }
        item.business_result_ref = business_persisted.count(job.origin_action_id) != 0
                                       ? business_persisted[job.origin_action_id]
                                       : std::string();
        item.result_ref = job.observed_result_ref.value_or("");
        item.result_version = job.observed_result_version;
        item.cancel_requested = job.cancel_requested;
        if (item.attempt_terminal != 0) {
            auto terminal = terminal_events.find(job.origin_action_id);
            if (terminal != terminal_events.end() && terminal->second.first == item.attempt) {
                item.attempt_terminal_event = terminal->second.second;
            }
        }
        // 接单消息在当前链上?(job_handle 必有;缺则补链不重跑,单 §6。)
        const bool admission_message_done =
            snap != nullptr &&
            std::any_of(snap->message_versions.begin(), snap->message_versions.end(),
                        [](const auto& version) { return version.on_current_chain; });
        item.admission_complete = admission_message_done;
        // 终态观测缺口:业务执行终态(finished)已在账、observed 终态没落。
        const bool execution_terminal_in_ledger =
            job.dispatched && snap != nullptr && !snap->attempts.empty() &&
            snap->attempts.back().status == "done";
        const bool observed_missing = job.state == "running" && execution_terminal_in_ledger;
        const bool terminal_state = IsTerminalJobState(job.state);
        if (job.mode != "job_handle") {
            item.disposition = "unsupported_mode";
            item.detail = "mode=" + job.mode + " 的执行归后续批次(P1 只接 job_handle)";
        } else if (!admission_message_done) {
            item.disposition = "complete_delivery";
            item.detail = "admission_chain_missing";
        } else if (observed_missing) {
            // 业务终态已在账、observed 终态没落:补观测不重跑。业务原文
            // 在账则 resultRef 指过去;缺原文(落仓失败窗口)只收投影。
            item.disposition = "complete_delivery";
            item.detail = "terminal_observation_missing";
        } else if (terminal_state) {
            item.disposition = "already_terminal";
            item.detail = job.state;
        } else if (job.state == "awaiting_approval") {
            item.disposition = "awaiting_approval";
            item.detail = "审批未过不派发(单 §6)";
        } else if (!job.dispatched) {
            item.disposition = "requeue";
            item.detail = "registered 未派发:接单链在账,可重新入队(epoch 接续)";
        } else {
            item.disposition = "unknown_hold";
            item.detail = "dispatched 无终态:转 unknown 不盲跑(单 §6)";
        }
        plan.items.push_back(std::move(item));
    }
    return plan;
}

std::size_t ToolJobCoordinator::AdoptRecovery(const JobRecoveryPlan& plan) {
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    std::size_t adopted = 0;
    for (const auto& item : plan.items) {
        if (impl_->jobs.count(item.job_id) > 0) {
            continue;  // 已接管(重复 Adopt 幂等)
        }
        // 号池对齐:新单不得与账上 jobId 撞号。
        if (item.job_id.rfind("job-", 0) == 0 && item.job_id.size() > 4) {
            const std::string number = item.job_id.substr(4);
            if (!number.empty() && number.find_first_not_of("0123456789") == std::string::npos) {
                const std::uint64_t value = std::stoull(number);
                if (value >= impl_->next_job_number) {
                    impl_->next_job_number = value + 1;
                }
            }
        }
        auto record = std::make_shared<JobRecord>();
        record->job_id = item.job_id;
        record->action_id = item.action_id;
        record->turn_id = item.turn_id;
        record->step_id = item.step_id;
        record->tool_name = item.tool_name;
        record->assistant_message_ref = item.assistant_message_ref;
        record->tool_input = item.tool_input;
        record->policy = item.policy;
        record->identity.logical_name = item.tool_name;
        record->identity.registration_source = "host_job_service";
        // 账面对齐:补链不重复落已有事件,续派从已终态 attempt 接续。
        record->action = trajectory::v3::ToolActionSession::ReopenAligned(
            item.turn_id, item.step_id, item.action_id, item.attempt, item.attempt_started,
            IntToTerminal(item.attempt_terminal));
        record->epoch_counter = item.dispatched_count;
        record->recovered_terminal_event = item.attempt_terminal_event;
        impl_->jobs[item.job_id] = record;
        JobRecord& job = *record;
        // 账上接单链完整的事实先对齐(缺时由补链路径落);末枚 attempt>1
        // 也说明接单早已配齐(派发只发生在接单配齐之后)。
        job.admission_complete = item.admission_complete || item.attempt > 1;
        if (item.disposition == "unsupported_mode") {
            job.state = "unknown";  // 不接管:只登记可见,不可操作
            continue;
        }
        if (item.disposition == "awaiting_approval") {
            job.state = "awaiting_approval";
            adopted += 1;
            continue;
        }
        if (item.disposition == "unknown_hold") {
            // running 查不明 -> unknown(单 §6):落 observed(unknown),
            // 不盲跑、不合成假终态。observed 不要求 attempt(定案 4)。
            auto observed = impl_->ObserveLocked(job, "unknown");
            if (ReceiptOk(observed)) {
                job.state = "unknown";
                adopted += 1;
            } else {
                NoteWriteFailure("tool.job.observed(unknown)", observed);
            }
            continue;
        }
        if (item.disposition == "already_terminal") {
            job.state = item.detail.empty() ? "unknown" : item.detail;
            job.result_ref = item.result_ref;
            job.result_version = item.result_version;
            job.cancel_requested = item.cancel_requested;  // 取消意图留档(≠已终止)
            adopted += 1;
            continue;
        }
        if (item.disposition == "complete_delivery") {
            // 补投递不重跑(单 §6):接单链缺则补;终态观测缺则补 observed
            //(业务原文在账时 resultRef 指过去;缺原文只收投影不带引用)。
            bool ok = impl_->WriteAdmissionChainLocked(job);
            if (ok && item.detail == "terminal_observation_missing") {
                WriteReceipt observed =
                    item.business_result_ref.empty()
                        ? impl_->ObserveLocked(job, "succeeded")
                        : impl_->ObserveLocked(job, "succeeded", item.business_result_ref, 1);
                if (ReceiptOk(observed)) {
                    job.state = "succeeded";
                    job.result_ref = item.business_result_ref;
                    job.result_version = item.business_result_ref.empty() ? 0 : 1;
                } else {
                    ok = false;
                    NoteWriteFailure("tool.job.observed(恢复补)", observed);
                }
            } else if (ok) {
                // 接单链补齐:job 回 queued 重新入队(requeue 语义)。
                job.state = "queued";
            }
            if (ok) {
                adopted += 1;
                if (job.state == "queued") {
                    impl_->queue.push_back(job.job_id);
                }
            }
            continue;
        }
        if (item.disposition == "requeue") {
            // registered 未派发:接单链确保在账,重新入队;派发时 epoch
            // 从账上租约数接续递增(定案 1:接管=新 dispatched)。
            if (!impl_->WriteAdmissionChainLocked(job)) {
                continue;  // 链没补上:留 registered,下次 Adopt 再试
            }
            job.state = "queued";
            impl_->queue.push_back(job.job_id);
            adopted += 1;
            continue;
        }
    }
    impl_->TryDispatchLocked();
    impl_->state_cv.notify_all();
    return adopted;
}

// ---------------------------------------------------------------------------
// 诊断
// ---------------------------------------------------------------------------

std::uint64_t ToolJobCoordinator::stale_envelopes_rejected() const {
    return impl_->stale_rejected;
}

std::uint64_t ToolJobCoordinator::duplicate_terminal_envelopes_rejected() const {
    return impl_->duplicate_rejected;
}

std::size_t ToolJobCoordinator::running_count() const {
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    return impl_->RunningCountLocked();
}

std::size_t ToolJobCoordinator::queued_count() const {
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    return impl_->queue.size();
}

}  // namespace lubancode::tools
