// tool_job_coordinator.hpp 的实现。单写者纪律:所有 v3 账面追加发生在
// 旧主锁(jobs_mutex)或新登记域 writer serial 内的泵/派发/接口路径;
// 新宿主 callback 不跨 jobs_mutex，worker 只发布 owned mailbox，不碰
// writer。旧租约与终态后的迟到信封拒收(计数,不落账)。
//
// worker threads stay joinable. Impl keeps the mailbox alive; it does not own
// everything an executor borrows. Shutdown joins callbacks and their captures
// before the host releases its registry, backend, MCP Clients or Lua state.
#include "tools/tool_job_coordinator.hpp"
#include "tools/run_command.hpp"
#include "platform/process.hpp"
#include "platform/bounded_read.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <expected>
#include <limits>
#include <map>
#include <ratio>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

#include "hooks/hash.hpp"
#include "platform/log_sink.hpp"
#include "platform/paths.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/result_store.hpp"

namespace lubancode::tools {

namespace {
thread_local const void* current_job_worker = nullptr;
thread_local const void* current_job_shutdown = nullptr;
thread_local const void* current_owned_callback = nullptr;
thread_local const void* current_owned_post_invocation = nullptr;
struct JobThreadScope {
    const void*& slot;
    const void* previous;
    JobThreadScope(const void*& target, const void* value) : slot(target), previous(target) { slot = value; }
    ~JobThreadScope() { slot = previous; }
};
}  // namespace

struct OwnedJobPostInvocation::State {
    const void* issuer = nullptr;
    const void* record = nullptr;
    const OwnedJobCompletion* completion = nullptr;
    std::uint64_t epoch = 0;
    std::thread::id thread;
    std::atomic<bool> active{true};
};

namespace {

using trajectory::v3::Durability;
using trajectory::v3::EventDraft;
using trajectory::v3::EventKindV3;
using trajectory::v3::WriteReceipt;

constexpr std::uint64_t kPreviewBudgetBytes = 32768;  // §4.18 预览合同
using OwnedClock = std::chrono::steady_clock;
using OwnedDeadline = std::optional<OwnedClock::time_point>;
constexpr const char* kOwnedDeadlineReason = "registration_deadline_elapsed";
static_assert(OwnedClock::is_steady);
static_assert(std::ratio_less_equal_v<OwnedClock::period, std::milli>);
static_assert(std::ratio_divide<OwnedClock::period, std::milli>::num == 1);

// Only the live Owned registration owns this clock. Never call the Legacy
// injected wall clock, serialize its epoch or manufacture a new deadline on resume.
bool FreezeOwnedDeadline(std::uint64_t milliseconds, OwnedDeadline& deadline) {
    if (!milliseconds) { deadline.reset(); return true; }
    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(OwnedClock::duration::max()).count();
    if (maximum <= 0 || milliseconds > static_cast<std::uint64_t>(maximum)) return false;
    const auto span = std::chrono::duration_cast<OwnedClock::duration>(
        std::chrono::milliseconds(static_cast<std::int64_t>(milliseconds)));
    const auto now = OwnedClock::now();
    if (now > OwnedClock::time_point::max() - span) return false;
    deadline = now + span;
    return true;
}

std::uint64_t RemainingOwnedMilliseconds(const OwnedDeadline& deadline) {
    if (!deadline) return (std::numeric_limits<std::uint64_t>::max)();
    const auto now = OwnedClock::now();
    if (now >= *deadline) return 0;
    // Floor, never round up into extra budget. Sub-millisecond remainder is
    // expired; zero must not reach a command's historical unlimited convention.
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now).count());
}

CommandExecutionLimits OwnedCommandLimits(CommandExecutionLimits host, const nlohmann::json& input,
                                          std::uint64_t remaining) {
    host.timeout_ms = (std::min)(host.timeout_ms, remaining);
    const auto model = input.find("timeout_ms");
    if (model != input.end() && model->is_number_integer()) {
        // Invalid model values still go to the real command validator. Do not
        // repair invalid input or reinterpret an oversized unsigned integer.
        const auto value = model->is_number_unsigned() ? model->get<std::uint64_t>() :
            model->get<std::int64_t>() > 0 ? static_cast<std::uint64_t>(model->get<std::int64_t>()) : 0;
        if (value && value <= 86400000) host.timeout_ms = (std::min)(host.timeout_ms, value);
    }
    return host;
}

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
    bool command_not_invoked = false; // Producer-only late deadline fact; result is absent semantically.
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
    std::string mode = "job_handle";  // job_handle|native_deferred(P2)
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
    bool dispatch_quota_owned = false;
    bool startup_settlement_pending = false;
    std::string startup_terminal_event;
    bool cancel_requested = false;
    bool cancel_event_written = false;
    bool admission_complete = false;  // 接单结果链(含 tool 消息)已落稳
    bool admission_facts_complete = false;  // 提前档:接单事实链先落,消息后补
    std::string admission_status = "queued";  // 接单事实落稳时的状态(消息正文用)
    std::string admission_text;               // 接单正文 JSON(消息补落时用)
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
    std::optional<JobRecoveryFacts> recovery;
};

struct PreparedRecord {
    PreparedJobRegistrationState state = PreparedJobRegistrationState::Unconfirmed;
    bool revoked = false;
    std::shared_ptr<PreparedJobFacts> facts;
    OwnedDeadline deadline;
};

struct OwnedRecord {
    JobRecord job;
    OwnedJobScope scope;
    std::shared_ptr<const PreparedJobFacts> facts;
    std::shared_ptr<OwnedJobCapability> capability;
    std::optional<WriteReceipt> adopted, dispatched, started, terminal, persisted, post, observed, cancelled;
    nlohmann::json parent_admission;
    std::string execution_state, gap;
    std::string requested_cancel_reason; // jobs_mutex; first non-writing request only
    bool settled = false;
    bool settlement_started = false;
    bool post_invoked = false;
    std::shared_ptr<std::atomic<bool>> worker_finished;
    OwnedDeadline deadline;
    bool command_not_invoked = false;
};

struct OwnedCommandBudget {
    OwnedDeadline deadline;
    CommandExecutionLimits limits;
};

OwnedJobScope ScopeOf(const PreparedJobFacts& f) {
    return {f.owner, f.job_id, f.action_id, f.turn_id, f.step_id,
            f.parent_action_id, f.provider_tool_call_id, 1};
}

bool SameWriterPrefix(const trajectory::v3::V3Ledger& ledger,
                      const trajectory::v3::V3Writer& writer) {
    const auto last = ledger.LastEntry();
    if (!last || last->seq == std::numeric_limits<std::uint64_t>::max() ||
        last->seq + 1 != writer.next_seq()) return false;
    return writer.last_line_hash() == (last->is_message ? ledger.messages[last->index].line_hash
                                                      : ledger.events[last->index].line_hash);
}

std::expected<std::string, std::string> ReadOwnedAdmissionArtifact(
    const std::filesystem::path& session_dir, const nlohmann::json& ref,
    const std::shared_ptr<trajectory::NamedResultCapability>& named_results) {
    const auto fail = [] { return std::unexpected(std::string("job.owned.parent_material_invalid")); };
    const auto relative = platform::Utf8ToPath(JsonStr(ref, "path"));
    if (relative.empty() || relative.has_root_path() || relative.parent_path() != "artifacts") return fail();
    for (const auto& part : relative) if (part == "." || part == "..") return fail();
    const auto expected_bytes = ref.find("bytes");
    if (expected_bytes == ref.end() || !expected_bytes->is_number_unsigned() ||
        expected_bytes->get<std::uint64_t>() > 65536) return fail();
    if (named_results) {
        auto bytes = named_results->Read(JsonStr(ref, "path"), JsonStr(ref, "sha256"),
            expected_bytes->get<std::uint64_t>(), JsonStr(ref, "mediaType"), 65536);
        if (!bytes) return fail();
        return std::move(*bytes);
    }
    std::error_code error;
    const auto real_root = std::filesystem::canonical(platform::FileIoPath(session_dir), error);
    if (error) return fail();
    const auto requested = platform::FileIoPath(session_dir / relative);
    const auto real_file = std::filesystem::canonical(requested, error);
    if (error) return fail();
    const auto within = real_file.lexically_relative(real_root);
    if (within.empty() || within.has_root_path()) return fail();
    for (const auto& part : within) if (part == "..") return fail();
    const auto bytes = platform::ReadBoundedRegularFile(requested, 65536);
    if (!bytes || bytes->size() != expected_bytes->get<std::uint64_t>() ||
        hooks::Sha256Hex(*bytes) != JsonStr(ref, "sha256")) return fail();
    return *bytes;
}

struct PreparedSource {
    std::string pending_event_id;
    std::string admission_event_id;
};

// Run under the host's real writer serial mutex, without jobs_mutex. These
// facts come from the verified file, not from a host-created input snapshot.
std::expected<PreparedSource, std::string> CheckPreparedSource(
    const trajectory::v3::V3Ledger& ledger, const PreparedJobRequest& request) {
    const auto& owner = request.owner;
    const auto* message = ledger.FindMessage(request.assistant_message_ref);
    if (!message || message->session_id != owner.session_id || message->run_id != owner.run_id ||
        message->turn_id != request.turn_id || message->step_id != request.step_id ||
        JsonStr(message->message, "role") != "assistant")
        return std::unexpected("job.prepared.invalid_declaration");
    const auto calls = message->message.find("tool_calls");
    if (calls == message->message.end() || !calls->is_array())
        return std::unexpected("job.prepared.invalid_declaration");
    unsigned matching_calls = 0;
    for (const auto& call : *calls) {
        if (JsonStr(call, "id") != request.provider_tool_call_id) continue;
        ++matching_calls;
        const auto function = call.find("function");
        if (function == call.end() || !function->is_object() ||
            JsonStr(*function, "name") != request.tool_name)
            return std::unexpected("job.prepared.invalid_declaration");
        const auto args = function->find("arguments");
        if (args == function->end() || !args->is_string())
            return std::unexpected("job.prepared.invalid_declaration");
        const auto original = nlohmann::json::parse(args->get<std::string>(), nullptr, false);
        if (original.is_discarded() || !original.is_object() || original != request.original_input)
            return std::unexpected("job.prepared.invalid_declaration");
    }
    if (matching_calls != 1)
        return std::unexpected("job.prepared.ambiguous_declaration");
    PreparedSource source;
    unsigned matching_pending = 0;
    unsigned matching_admission = 0;
    std::uint64_t admission_seq = 0, pending_seq = 0;
    for (const auto& event : ledger.events) {
        // A persisted temporary ticket can never be registered a second time,
        // even through a fresh coordinator instance. Pending also protects the
        // committed-first-write/failed-registration window.
        if (event.kind == EventKindV3::ToolExecutionPending || event.kind == EventKindV3::ToolJobRegistered) {
            const auto prepared_only = event.payload.find("preparedOnly");
            if (prepared_only != event.payload.end() && !prepared_only->is_boolean())
                return std::unexpected("job.prepared.invalid_marker");
            if (prepared_only != event.payload.end() && prepared_only->get<bool>() &&
                (JsonStr(event.payload, "parentActionId") == request.parent_action_id ||
                 (JsonStr(event.payload, "assistantMessageRef") == request.assistant_message_ref &&
                  JsonStr(event.payload, "provider_tool_call_id") == request.provider_tool_call_id)))
                return std::unexpected("job.prepared.already_registered");
        }
        if (event.kind == EventKindV3::ContextInputApplied) {
            const auto added = event.payload.find("addedMessageRefs");
            if (added == event.payload.end() || !added->is_array()) continue;
            for (const auto& id : *added) {
                if (!id.is_string() || id.get<std::string>() != request.assistant_message_ref) continue;
                if (event.session_id != owner.session_id || event.run_id != owner.run_id || event.seq <= message->seq)
                    return std::unexpected("job.prepared.foreign_admission");
                ++matching_admission;
                admission_seq = event.seq;
                source.admission_event_id = event.event_id;
            }
        }
        if (event.action_id != request.parent_action_id) continue;
        if (event.session_id != owner.session_id || event.run_id != owner.run_id ||
            event.turn_id != request.turn_id || event.step_id != request.step_id)
            return std::unexpected("job.prepared.foreign_action");
        if (event.kind == EventKindV3::ToolExecutionPending) {
            ++matching_pending;
            if (JsonStr(event.payload, "tool_call_id") != request.parent_action_id ||
                JsonStr(event.payload, "assistantMessageRef") != request.assistant_message_ref ||
                JsonStr(event.payload, "provider_tool_call_id") != request.provider_tool_call_id ||
                !event.payload.contains("attempt") || event.payload.at("attempt") != 1 || event.seq <= message->seq)
                return std::unexpected("job.prepared.invalid_pending");
            source.pending_event_id = event.event_id;
            pending_seq = event.seq;
        } else if (event.kind == EventKindV3::ToolExecutionStarted ||
                   event.kind == EventKindV3::ToolExecutionFinished || event.kind == EventKindV3::ToolExecutionFailed ||
                   event.kind == EventKindV3::ToolExecutionCancelled || event.kind == EventKindV3::ToolExecutionRejected ||
                   event.kind == EventKindV3::ToolExecutionUnknown)
            return std::unexpected("job.prepared.source_already_executed");
    }
    const bool on_chain = std::any_of(ledger.context.chain.begin(), ledger.context.chain.end(),
        [&](const auto& node) { return node.message_ref == request.assistant_message_ref; });
    if (matching_pending != 1 || matching_admission != 1 || pending_seq <= admission_seq || !on_chain)
        return std::unexpected("job.prepared.unadmitted_declaration");
    return source;
}

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
    ThreadStarter thread_starter;
    JobConcurrencyLimits limits;
    std::shared_ptr<GlobalRunningQuota> global;
    std::function<std::int64_t()> clock_ms;
    std::weak_ptr<Impl> self_lock;  // worker 闭包经它拿稳定引用
    std::optional<PreparedRegistrationContext> prepared_context;
    std::shared_ptr<trajectory::NamedResultCapability> named_results;
    PreparedJobOwner prepared_owner;
    bool prepared_revoked = false;
    OwnedJobPostPhase owned_post_phase = OwnedJobPostPhase::Open;
    std::map<std::string, std::shared_ptr<PreparedRecord>> prepared;
    std::map<std::string, std::shared_ptr<OwnedRecord>> owned;
    std::vector<std::string> owned_queue;

    // 主锁:jobs/queue/writer 落账/资源占用。worker 经 owned mailbox 原子
    // 发布完成件;旧 debug 信封队列另设锁,泵在主锁内收信封。
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
    std::atomic<bool> shutdown_complete{false};
    bool shutdown_in_progress = false;
    bool shutdown_settlement_ok = true;
    std::condition_variable shutdown_cv;
    struct WorkerLaunch {
        JobExecutionContext context;
        JobCompletionEnvelope completion;
        Tool::Result fallback_result;
        std::string fallback_error_code;
        std::unique_ptr<JobExecutor> run;
        std::atomic<bool> completion_ready{false};
        std::shared_ptr<OwnedCommandBudget> owned_budget; // Null in every Legacy path.
    };
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
        std::shared_ptr<WorkerLaunch> launch;
    };
    std::vector<Worker> workers;
    // Retain cancellation until join returns, independently of job state.
    struct OwnedWorker {
        std::shared_ptr<std::atomic<bool>> cancel;
        std::shared_ptr<std::atomic<bool>> finished;
        std::shared_ptr<WorkerLaunch> launch;
    };
    std::vector<OwnedWorker> owned_workers;
    // Reaping and shutdown never hold jobs_mutex while joining. Ordinary APIs
    // try this mutex, so an executor/capture destructor can query its job while
    // another caller joins it without blocking on the join itself.
    std::mutex shutdown_mutex;

    void ReapFinishedWorkers() {
        if (current_job_worker == this || current_job_shutdown == this) return;
        std::unique_lock reaper(shutdown_mutex, std::try_to_lock);
        if (!reaper.owns_lock() || shutdown_in_progress) return;
        JobThreadScope reap_scope(current_job_shutdown, this);
        std::vector<Worker> finished;
        {
            std::lock_guard lock(jobs_mutex);
            // Allocate before transferring any joinable thread. A failed
            // reserve leaves every live owner in the coordinator.
            finished.reserve(workers.size());
            for (auto it = workers.begin(); it != workers.end();) {
                if (it->finished != nullptr && it->finished->load() &&
                    (!it->launch || !it->launch->completion_ready.load())) {
                    finished.push_back(std::move(*it));
                    it = workers.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& worker : finished) if (worker.thread.joinable()) worker.thread.join();
        {
            std::lock_guard lock(jobs_mutex);
            for (const auto& worker : finished) {
                std::erase_if(owned_workers, [&](const auto& owned) { return owned.finished == worker.finished; });
            }
        }
    }

    void FreezeOwned(OwnedRecord& record, const std::string& gap) {
        std::lock_guard lock(jobs_mutex);
        if (record.gap.empty()) record.gap = gap;
        record.job.state = "unknown";
        record.settled = true;  // Never re-submit any earlier native stage.
    }

    void SettleOwned(OwnedRecord& record, const JobCompletionEnvelope& envelope) {
        auto& job = record.job;
        {
            std::lock_guard lock(jobs_mutex);
            if (record.settled || record.settlement_started) return;
            record.settlement_started = true;
            ReleaseDispatchReservationLocked(job);
        }
        try {
            if (envelope.command_not_invoked) {
                record.command_not_invoked = true;
                if (!record.deadline || !record.started || !ReceiptOk(*record.started)) {
                    FreezeOwned(record, "job.owned.invalid_deadline_completion"); return;
                }
                record.execution_state = "cancelled";
                if (!record.cancelled) {
                    record.cancelled = EmitCancelRequestedLocked(job, kOwnedDeadlineReason);
                    if (!ReceiptOk(*record.cancelled)) { FreezeOwned(record, "job.owned.cancel_unconfirmed"); return; }
                }
                record.terminal = job.action->Cancel(*writer, "during_execution", kOwnedDeadlineReason);
                if (!ReceiptOk(*record.terminal)) { FreezeOwned(record, "job.owned.terminal_unconfirmed"); return; }
                EventDraft observation;
                observation.kind = EventKindV3::ToolJobObserved;
                observation.turn_id = record.scope.turn_id; observation.step_id = record.scope.step_id;
                observation.action_id = record.scope.action_id;
                observation.payload = {{"tool_call_id", job.action_id}, {"jobId", job.job_id}, {"observedStatus", "cancelled"},
                    {"commandNotInvoked", {{"version", 1}, {"reason", kOwnedDeadlineReason}}},
                    {"parentAdmission", record.parent_admission}};
                record.observed = writer->AppendEvent(std::move(observation), Durability::PowerLoss);
                if (!ReceiptOk(*record.observed)) { FreezeOwned(record, "job.owned.observed_unconfirmed"); return; }
                {
                    std::lock_guard lock(jobs_mutex);
                    job.state = "cancelled"; job.failure = kOwnedDeadlineReason;
                    job.cancel_requested = true; job.cancel_flag->store(true); record.settled = true;
                }
                state_cv.notify_all();
                return; // No command result or Post invocation ever existed.
            }
            if (envelope.result.execution_control == ExecutionControl::StopIndeterminate) {
                record.execution_state = "unknown";
                record.terminal = job.action->MarkUnknown(*writer,
                    envelope.error_code.empty() ? "job.execution_unconfirmed" : envelope.error_code);
            } else if (envelope.cancelled) {
                record.execution_state = "cancelled";
                record.terminal = job.action->Cancel(*writer, "during_execution", "worker_saw_cancel");
            } else if (envelope.succeeded) {
                record.execution_state = "succeeded";
                record.terminal = job.action->Finish(*writer, std::nullopt, envelope.duration_ms);
            } else {
                record.execution_state = "failed";
                record.terminal = job.action->Fail(*writer,
                    envelope.error_code.empty() ? "tool_failed" : envelope.error_code, envelope.duration_ms);
            }
            if (!ReceiptOk(*record.terminal)) { FreezeOwned(record, "job.owned.terminal_unconfirmed"); return; }
            if (record.execution_state == "unknown") {
                // Neither an executor exception nor a damaged completion
                // report proves a command raw/ordinary Post result.
                FreezeOwned(record, "job.owned.execution_unconfirmed"); return;
            }
            // Both successful and failed command text are real business raw.
            // Rich blocks are deliberately outside this first internal slice.
            for (const auto& block : envelope.result.payload.content) {
                if (!std::holds_alternative<TextContent>(block)) {
                    FreezeOwned(record, "job.owned.rich_result_unsupported"); return;
                }
            }
            auto material = PersistTextLocked(job, "combined", envelope.result.content, record.terminal->id, 1);
            if (!material.ok) { FreezeOwned(record, "job.owned.raw_unconfirmed"); return; }
            record.persisted = job.action->PersistedResult(*writer, material.result_ref, record.terminal->id, 1);
            if (!ReceiptOk(*record.persisted)) { FreezeOwned(record, "job.owned.persisted_unconfirmed"); return; }
            job.result_ref = record.persisted->id; job.result_version = 1;
            job.preview = material.preview.text; job.preview_truncated = material.preview.truncated;
            OwnedJobCompletion completed{record.scope, envelope.result, *record.started,
                                         *record.terminal, *record.persisted};
            record.post_invoked = true;
            {
                JobThreadScope callback(current_owned_callback, this);
                if (record.capability->live_post) {
                    // This unique invocation is created from the actual record
                    // and completion, not from caller-provided identity values.
                    auto state = std::make_shared<OwnedJobPostInvocation::State>();
                    state->issuer = this; state->record = &record; state->completion = &completed;
                    state->epoch = prepared_owner.epoch; state->thread = std::this_thread::get_id();
                    JobThreadScope active(current_owned_post_invocation, state.get());
                    struct Revoke {
                        OwnedJobPostInvocation::State& state;
                        ~Revoke() { state.active.store(false, std::memory_order_release); }
                    } revoke{*state};
                    const OwnedJobPostInvocation invocation(state);
                    record.post = record.capability->live_post(completed, invocation);
                } else {
                    record.post = record.capability->post(completed);
                }
            }
            if (!ReceiptOk(*record.post) || writer->broken()) { FreezeOwned(record, "job.owned.post_unconfirmed"); return; }
            const auto ledger = trajectory::v3::ReadV3Ledger(writer->path());
            const auto* actual = ledger ? ledger->FindEvent(record.post->id) : nullptr;
            if (!ledger || !SameWriterPrefix(*ledger, *writer) || !actual ||
                actual->seq != record.post->seq || actual->line_hash != record.post->line_hash ||
                actual->seq <= record.persisted->seq ||
                actual->kind != EventKindV3::HookCompleted || actual->session_id != record.scope.owner.session_id ||
                actual->run_id != record.scope.owner.run_id || actual->action_id != record.scope.action_id ||
                actual->turn_id != record.scope.turn_id || actual->step_id != record.scope.step_id) {
                FreezeOwned(record, "job.owned.post_receipt_mismatch"); return;
            }
            EventDraft observation;
            observation.kind = EventKindV3::ToolJobObserved;
            observation.turn_id = record.scope.turn_id; observation.step_id = record.scope.step_id;
            observation.action_id = record.scope.action_id;
            const auto post_ref = trajectory::v3::MakeOwnedJobReference(*ledger, record.post->id);
            if (!post_ref) { FreezeOwned(record, "job.owned.post_receipt_mismatch"); return; }
            const auto adoptions = trajectory::v3::ReadOwnedJobAdoptions(*ledger);
            const auto* adoption = adoptions ? trajectory::v3::FindOwnedJobAdoption(*adoptions, job.job_id) : nullptr;
            if (!adoption || !trajectory::v3::CheckOwnedJobPost(*ledger, *adoption,
                    record.terminal->id, record.persisted->id, *post_ref)) {
                FreezeOwned(record, "job.owned.post_receipt_mismatch"); return;
            }
            observation.payload = {{"tool_call_id", job.action_id}, {"jobId", job.job_id},
                {"observedStatus", record.execution_state}, {"resultRef", job.result_ref}, {"resultVersion", 1},
                {"postEventRef", *post_ref}};
            record.observed = writer->AppendEvent(std::move(observation), Durability::PowerLoss);
            if (!ReceiptOk(*record.observed)) { FreezeOwned(record, "job.owned.observed_unconfirmed"); return; }
            {
                std::lock_guard lock(jobs_mutex);
                job.state = record.execution_state;
                job.failure = envelope.error_code;
                record.settled = true;
            }
        } catch (...) { FreezeOwned(record, "job.owned.settlement_unconfirmed"); }
        state_cv.notify_all();
    }

    void CloseOwnedBeforeStart(OwnedRecord& record, bool cancelled, const std::string& reason) {
        {
            std::lock_guard lock(jobs_mutex);
            if (record.settled || record.settlement_started) return;
            record.settlement_started = true;
        }
        try {
            record.execution_state = cancelled ? "cancelled" : "rejected";
            record.terminal = cancelled ? record.job.action->Cancel(*writer, "before_started", reason)
                                        : record.job.action->Reject(*writer, reason);
            if (!ReceiptOk(*record.terminal)) { FreezeOwned(record, "job.owned.terminal_unconfirmed"); return; }
            if (cancelled && reason == kOwnedDeadlineReason && record.deadline) {
                EventDraft observed;
                observed.kind = EventKindV3::ToolJobObserved;
                observed.turn_id = record.scope.turn_id; observed.step_id = record.scope.step_id;
                observed.action_id = record.scope.action_id;
                observed.payload = {{"tool_call_id", record.job.action_id}, {"jobId", record.job.job_id},
                    {"observedStatus", "cancelled"}, {"parentAdmission", record.parent_admission}};
                record.observed = writer->AppendEvent(std::move(observed), Durability::PowerLoss);
            } else {
                record.observed = ObserveLocked(record.job, cancelled ? "cancelled" : "failed");
            }
            if (!ReceiptOk(*record.observed)) { FreezeOwned(record, "job.owned.observed_unconfirmed"); return; }
            {
                std::lock_guard lock(jobs_mutex);
                record.job.state = cancelled ? "cancelled" : "failed";
                record.job.failure = reason;
                record.settled = true;
            }
        } catch (...) { FreezeOwned(record, "job.owned.settlement_unconfirmed"); }
    }

    void ExpireOwnedBeforeStart(OwnedRecord& record) {
        {
            std::lock_guard lock(jobs_mutex);
            record.job.cancel_requested = true;
            record.job.cancel_flag->store(true);
            record.command_not_invoked = true;
        }
        try {
            if (!record.cancelled) record.cancelled = EmitCancelRequestedLocked(record.job, kOwnedDeadlineReason);
            if (!ReceiptOk(*record.cancelled)) { FreezeOwned(record, "job.owned.cancel_unconfirmed"); return; }
            CloseOwnedBeforeStart(record, true, kOwnedDeadlineReason);
        } catch (...) { FreezeOwned(record, "job.owned.deadline_unconfirmed"); }
    }

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
            if (!job->recovery.has_value() && job->state == "running") {
                ++count;
            }
        }
        for (const auto& [id, record] : owned) {
            (void)id;
            if (record->job.dispatch_quota_owned) ++count;
        }
        return count;
    }

    std::size_t ToolRunningCountLocked(const std::string& tool_name) const {
        std::size_t count = 0;
        for (const auto& [id, job] : jobs) {
            (void)id;
            if (!job->recovery.has_value() && job->state == "running" && job->tool_name == tool_name) {
                ++count;
            }
        }
        for (const auto& [id, record] : owned) {
            (void)id;
            if (record->job.dispatch_quota_owned && record->job.tool_name == tool_name) ++count;
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

    void ReleaseResourcesLocked(JobRecord& job) noexcept {
        // Cleanup cannot allocate another ResourceKeysOf vector or depend on
        // the partially completed acquisition having reached every key.
        for (auto it = resource_holders.begin(); it != resource_holders.end();) {
            if (it->second == job.job_id) it = resource_holders.erase(it);
            else ++it;
        }
    }

    void ReleaseDispatchReservationLocked(JobRecord& job) noexcept {
        ReleaseResourcesLocked(job);
        if (std::exchange(job.dispatch_quota_owned, false)) global->Release();
    }

    // 结果仓:每次临时开(扫目录续号),防两只实例号池错位撞不可变名。
    std::optional<trajectory::v3::ResultStore> OpenStore() {
        auto store = named_results ? trajectory::v3::ResultStore::Open(named_results) :
            trajectory::v3::ResultStore::Open(writer->path().parent_path());
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
        preview_request.host_result_references = named_results && named_results->external();
        trajectory::v3::PreviewChannel preview_channel;
        preview_channel.display_path = outcome.text_artifact_path.empty()
                                           ? "artifacts/" + persisted.result_id
                                           : outcome.text_artifact_path;
        if (named_results && named_results->external())
            preview_channel.display_path = named_results->DisplayPath(preview_channel.display_path);
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
        if (!WriteAdmissionFactsLocked(job)) {
            return false;
        }
        return WriteAdmissionMessageLocked(job);
    }

    // 接单事实链(attempt 1 的 started/finished/persisted/selected):账面
    // 事实不依赖声明消息落账,流式提前档在派发前先落这半截。
    bool WriteAdmissionFactsLocked(JobRecord& job) {
        if (job.admission_facts_complete) {
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
        // 状态取接单事实落稳那一刻的(job_handle 接单=排队;审批挂起如实
        // 报 awaiting_approval)——job 后来跑完/收场不改写接单口径,业务
        // 结果经 get/wait 另取(单 §8)。
        job.admission_status = job.state == "awaiting_approval" ? "awaiting_approval" : "queued";
        const std::string admission_text =
            nlohmann::json::object({{"jobId", job.job_id}, {"status", job.admission_status}})
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
        job.admission_text = admission_text;
        job.admission_facts_complete = true;
        return true;
    }

    // 接单 tool 消息:须等声明消息(assistant)落账后才许进上下文链
    // (流式提前档的 CompleteAdmission 在批次收口补这半截)。
    bool WriteAdmissionMessageLocked(JobRecord& job) {
        if (job.admission_complete) {
            return true;
        }
        if (!job.admission_facts_complete || !job.action.has_value()) {
            return false;
        }
        if (job.admission_text.empty()) {
            // 恢复重建的记录没带正文:接单口径确定,原样重造。
            job.admission_text = nlohmann::json::object(
                {{"jobId", job.job_id}, {"status", job.admission_status}}).dump();
        }
        auto message = job.action->AppendToolMessage(*writer, job.admission_text,
                                                     job.action->selected_event_id(), false);
        if (!ReceiptOk(message)) {
            NoteWriteFailure("接单 tool 消息", message);
            return false;
        }
        job.admission_complete = true;
        return true;
    }

    // 派发(单 §5:注册/接单落稳后才走到这):复查授权与取消 -> 新租约
    // dispatched -> attempt 2 execution -> an owned worker thread.
    enum class DispatchOutcome { Dispatched, KeepQueued, Cancelled, Failed };
    DispatchOutcome DispatchJobLocked(JobRecord& job) {
        if (job.recovery.has_value()) return DispatchOutcome::KeepQueued;
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
        job.dispatch_quota_owned = true;
        std::shared_ptr<WorkerLaunch> launch;
        std::shared_ptr<std::atomic<bool>> finished;
        bool worker_staged = false;
        try {
            if (limits.session_running != 0 &&
                RunningCountLocked() + 1 > limits.session_running) {
                ReleaseDispatchReservationLocked(job);
                return DispatchOutcome::KeepQueued;
            }
            if (limits.per_tool != 0 && ToolRunningCountLocked(job.tool_name) + 1 > limits.per_tool) {
                ReleaseDispatchReservationLocked(job);
                return DispatchOutcome::KeepQueued;
            }
            if (!ResourceFreeLocked(job)) {
                ReleaseDispatchReservationLocked(job);
                return DispatchOutcome::KeepQueued;
            }
            if (!executor) {
                ReleaseDispatchReservationLocked(job);
                return DispatchOutcome::KeepQueued;
            }
            // 调度意图先落账,再起线程(注册落稳前不派发的同款纪律:
            // dispatched 落稳前不起 worker)。job_handle 的接单是 attempt 1,
            // 工作开 attempt 2;native_deferred 没有接单链(原调用欠账),
            // 工作就是 attempt 1——不 BeginNextAttempt(§4.14:上一 attempt
            // 未终态不得开下一 attempt,native 的 attempt 1 只有调用证据)。
            if (job.mode != "native_deferred") {
                auto pending = job.action->BeginNextAttempt(*writer, "job_dispatch");
                if (!ReceiptOk(pending)) {
                    NoteWriteFailure("tool.execution.pending(attempt 2)", pending);
                    ReleaseDispatchReservationLocked(job);
                    return DispatchOutcome::KeepQueued;
                }
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
                ReleaseDispatchReservationLocked(job);
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
                ReleaseDispatchReservationLocked(job);
                return DispatchOutcome::KeepQueued;
            }
            job.dispatched = true;
            job.state = "running";
            const std::int64_t now = NowMs();
            job.deadline_at_ms =
                job.policy.deadline_ms == 0 ? 0 : static_cast<std::uint64_t>(now + static_cast<std::int64_t>(job.policy.deadline_ms));
            AcquireResourcesLocked(job);
            job.cancel_flag->store(false);
            // Worker only posts an envelope. Its entire thread, including captured
            // executor destruction, must exit before borrowed dependencies vanish.
            std::shared_ptr<Impl> self = self_lock.lock();
            if (self == nullptr) {
                throw std::runtime_error("job coordinator owner expired during start");
            }
            // Stage a stable owner before copying any complete user executor.
            launch = std::make_shared<WorkerLaunch>();
            finished = std::make_shared<std::atomic<bool>>(false);
            workers.emplace_back();
            workers.back().finished = finished;
            workers.back().launch = launch;
            worker_staged = true;
            owned_workers.push_back({job.cancel_flag, finished, launch});
            launch->context.job_id = job.job_id;
            launch->context.input = job.tool_input;
            launch->context.max_output_bytes = job.policy.max_output_bytes;
            launch->context.cancel = job.cancel_flag.get();
            launch->completion.job_id = job.job_id;
            launch->completion.owner_epoch = job.owner_epoch;
            launch->completion.error_code = "tool.job.executor_exception";
            launch->completion.cancel_reason = "worker_saw_cancel";
            launch->fallback_result = Tool::Result::Error("tool job completion preparation threw");
            launch->fallback_result.error_code = "tool.job.executor_exception";
            launch->fallback_error_code = "tool.job.executor_exception";
            launch->run = std::make_unique<JobExecutor>(executor);
            const auto cancel_flag = job.cancel_flag;
            std::function<void()> entry = [self, launch, cancel_flag, finished, started_at = now]() mutable {
                JobThreadScope worker_scope(current_job_worker, self.get());
                auto& envelope = launch->completion;
                try {
                    Tool::Result result;
                    try {
                        result = (*launch->run)(launch->context);
                    } catch (const std::exception& error) {
                        result = Tool::Result::Error(error.what());
                        result.error_code = "tool.job.executor_exception";
                    } catch (...) {
                        result = Tool::Result::Error("tool job executor threw");
                        result.error_code = "tool.job.executor_exception";
                    }
                    envelope.result = std::move(result);
                    envelope.succeeded = !envelope.result.is_error;
                    if (envelope.result.is_error) {
                        envelope.error_code = envelope.result.error_code.empty()
                                                  ? "tool_failed" : envelope.result.error_code;
                    }
                    try {
                        envelope.duration_ms = static_cast<std::uint64_t>(
                            std::max<std::int64_t>(0, self->NowMs() - started_at));
                    } catch (...) {
                        // A diagnostic clock cannot hide the actual result.
                    }
                    if (cancel_flag->load() && envelope.result.is_error) {
                        envelope.cancelled = true;
                        envelope.succeeded = false;
                    }
                } catch (...) {
                    envelope.result = std::move(launch->fallback_result);
                    envelope.succeeded = false;
                    envelope.error_code.swap(launch->fallback_error_code);
                }
                // The envelope and its identity were allocated before launch. No
                // worker-side deque allocation may lose a completed callback.
                launch->completion_ready.store(true, std::memory_order_release);
                self->state_cv.notify_all();
                launch->run.reset();
                finished->store(true, std::memory_order_release);
                self->state_cv.notify_all();
            };
            if (thread_starter) thread_starter(workers.back().thread, std::move(entry));
            else workers.back().thread = std::thread(std::move(entry));
            if (!workers.back().thread.joinable()) throw std::runtime_error("thread starter returned no owned thread");
        } catch (...) {
            // A starter may throw after publishing a real thread. Its owner and
            // real completion remain authoritative; never synthesize a second
            // terminal or destroy/detach that live thread.
            if (worker_staged && workers.back().thread.joinable()) return DispatchOutcome::Dispatched;
            if (finished) finished->store(true, std::memory_order_release);
            ReleaseDispatchReservationLocked(job);
            // Establish a non-running, non-replayable gap before any diagnostic
            // or failed-envelope allocation can itself fail.
            job.startup_settlement_pending = true;
            job.state = "unknown";
            // The outer caught exception remains alive while its diagnostic is
            // copied inside the protected failed-envelope assembly below.
            const char* start_error = "tool job worker could not start";
            try {
                throw;
            } catch (const std::exception& error) {
                start_error = error.what();
            } catch (...) {
            }
            try {
                JobCompletionEnvelope failure;
                failure.job_id = job.job_id;
                failure.owner_epoch = job.owner_epoch;
                failure.error_code = "tool.job.worker_start_failed";
                failure.result = Tool::Result::Error(start_error);
                failure.result.error_code = failure.error_code;
                job.pending_completion = failure;
                job.failure = failure.error_code;
                DrainEnvelopeLocked(failure);
            } catch (...) {
                // The attempted execution did not run, but its durable failed
                // facts remain unconfirmed. No running slot and no tool rerun.
            }
            return DispatchOutcome::Failed;
        }
        return DispatchOutcome::Dispatched;
    }

    // 队列泵:尽力派发队首可派者(配额/资源释放后由终态路径再调)。
    void TryDispatchLocked() {
        if (closing) return;
        for (std::size_t i = 0; i < queue.size();) {
            auto it = jobs.find(queue[i]);
            if (it == jobs.end() || it->second->recovery.has_value() ||
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
        if (job.recovery.has_value()) {
            ++stale_rejected;  // No worker lease was created for a held record.
            return;
        }
        if (IsTerminalJobState(job.state) && !job.startup_settlement_pending) {
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
        const bool saved_startup_terminal = job.startup_settlement_pending && !job.startup_terminal_event.empty();
        if (saved_startup_terminal) {
            // Reuse the actual earlier native confirmation, not a fabricated
            // Committed receipt and not a second failed event.
            terminal_state = "failed";
        } else if (envelope.cancelled) {
            terminal_receipt = job.action->Cancel(*writer, "during_execution",
                envelope.cancel_reason.empty() ? "cancelled" : envelope.cancel_reason);
            terminal_state = "cancelled";
        } else if (envelope.succeeded) {
            terminal_receipt = job.action->Finish(*writer, std::nullopt, envelope.duration_ms);
            terminal_state = "succeeded";
        } else {
            terminal_receipt = job.action->Fail(*writer, envelope.error_code, envelope.duration_ms);
            terminal_state = "failed";
            if (job.startup_settlement_pending && ReceiptOk(terminal_receipt))
                job.startup_terminal_event = terminal_receipt.id;
        }
        if (!saved_startup_terminal && !ReceiptOk(terminal_receipt)) {
            NoteWriteFailure("tool.execution terminal", terminal_receipt);
            job.pending_completion = envelope;
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
        job.startup_settlement_pending = false;
        ReleaseDispatchReservationLocked(job);
    }

    // deadline 巡检(单 §6:超时先请求取消,不把仍可能运行的副作用判失败;
    // 惰性驱动——泵路径顺带查,不起监控线程)。
    void CheckDeadlinesLocked() {
        const std::int64_t now = NowMs();
        for (auto& [id, job] : jobs) {
            (void)id;
            if (job->recovery.has_value() || job->state != "running" || job->deadline_at_ms == 0) {
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
            if (!job->recovery.has_value() && job->pending_completion.has_value() &&
                (job->startup_settlement_pending || !IsTerminalJobState(job->state))) {
                const JobCompletionEnvelope retry = *job->pending_completion;
                const std::string before = job->state;
                DrainEnvelopeLocked(retry);
                if (job->state != before && IsTerminalJobState(job->state)) {
                    ++settled;
                }
            }
        }
        for (const auto& worker : owned_workers) {
            if (!worker.launch || !worker.launch->completion_ready.load(std::memory_order_acquire)) continue;
            const auto found = jobs.find(worker.launch->completion.job_id);
            const std::string before = found == jobs.end() ? std::string() : found->second->state;
            DrainEnvelopeLocked(worker.launch->completion);
            worker.launch->completion_ready.store(false, std::memory_order_release);
            if (found != jobs.end() && found->second->state != before && IsTerminalJobState(found->second->state)) ++settled;
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

    // start 共通路(P1 批次档 + P2 流式提前档,单 §5 持久顺序):
    //   调用证据 -> 权鉴 -> 注册落稳 -> 接单(native 不接单:原调用欠账,
    //   业务结果由规划器配原 call)-> 入队派发。early=true 只落接单事实链
    //   (tool 消息等 CompleteAdmission 在声明消息落账后补,链序不倒)。
    JobStartResult StartJobCommon(const JobStartRequest& request, bool early) {
        JobStartResult result;
        if (request.tool_name.empty() || request.turn_id.empty() || request.step_id.empty() ||
            request.assistant_message_ref.empty()) {
            // originRef 三件(信封 turnId/stepId + payload assistantMessageRef)
            // 是 registered 的载荷合同,缺一不注册(单 §5)。
            result.error_code = "job.start.bad_request";
            result.error = "tool_name/turn_id/step_id/assistant_message_ref 不得为空";
            return result;
        }
        const bool native = request.mode == "native_deferred";
        if (request.mode != "job_handle" && !native) {
            result.error_code = "job.start.bad_request";
            result.error = "mode 只认 job_handle|native_deferred,收到: " + request.mode;
            return result;
        }
        if (native) {
            // P0 载荷合同:native 注册必带 wireCallRef{provider,wire,callId,async}。
            const bool ref_ok = request.wire_call_ref.is_object() &&
                                request.wire_call_ref.contains("provider") &&
                                request.wire_call_ref.contains("wire") &&
                                request.wire_call_ref.contains("callId") &&
                                request.wire_call_ref.contains("async");
            if (!ref_ok) {
                result.error_code = "job.start.bad_request";
                result.error = "native_deferred 须带 wireCallRef{provider,wire,callId,async}";
                return result;
            }
            if (early) {
                result.error_code = "job.start.unsupported_mode";
                result.error = "native_deferred 不走流式提前档(账面配对归批次裁决)";
                return result;
            }
        }
        std::lock_guard<std::mutex> lock(jobs_mutex);
        if (closing) {
            result.error_code = "job.start.closing";
            result.error = "协调器已收场";
            return result;
        }
        if (queue.size() >= limits.queued_max) {
            result.error_code = "job.start.queue_full";
            result.error = "待派队列已满(queued_max=" + std::to_string(limits.queued_max) + ")";
            return result;
        }
        // 调用证据(tool.execution.pending):注册的前置(单 §5 持久顺序)。
        const std::string job_id = "job-" + ZeroPad6(next_job_number);
        const std::string action_id = "action-job-" + ZeroPad6(next_job_number);
        next_job_number += 1;
        auto record = std::make_shared<JobRecord>();
        record->job_id = job_id;
        record->action_id = action_id;
        record->turn_id = request.turn_id;
        record->step_id = request.step_id;
        record->tool_name = request.tool_name;
        record->mode = native ? "native_deferred" : "job_handle";
        record->assistant_message_ref = request.assistant_message_ref;
        record->tool_input = request.tool_input;
        record->policy = request.policy;
        record->identity.logical_name = request.tool_name;
        record->identity.registration_source = "host_job_service";
        record->action = trajectory::v3::ToolActionSession::Admit(
            *writer, request.turn_id, request.step_id, action_id, "queued",
            std::optional<std::string>(request.assistant_message_ref), std::nullopt,
            nlohmann::json{{"toolName", request.tool_name}}, Durability::ProcessCrash);
        if (!record->action->last_event_id().has_value()) {
            result.error_code = "job.start.evidence_write_failed";
            result.error = "tool.execution.pending 落账失败,不注册";
            return result;
        }
        // 权鉴(单 §8:jobId 不是访问凭证;fail-closed)。
        JobAuthDecision auth = CheckGate(request.tool_name, request.tool_input);
        if (!auth.allowed && !auth.needs_approval) {
            auto rejected = record->action->Reject(
                *writer, auth.reason.empty() ? "authorization_denied" : auth.reason);
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
                                    {"mode", request.mode},
                                    {"assistantMessageRef", request.assistant_message_ref}});
        if (native) {
            payload["wireCallRef"] = request.wire_call_ref;
        }
        if (auth.needs_approval) {
            payload["approvalRequired"] = true;
        }
        payload["executionPolicy"] = request.policy.ToJson();
        registered.payload = std::move(payload);
        auto registered_receipt = writer->AppendEvent(std::move(registered), Durability::PowerLoss);
        if (!ReceiptOk(registered_receipt)) {
            // 注册没落稳:不派发、不接单,action 收口 failed。
            NoteWriteFailure("tool.job.registered", registered_receipt);
            record->action->Fail(*writer, "job_register_write_failed");
            result.error_code = "job.start.register_write_failed";
            result.error = registered_receipt.error_message;
            return result;
        }
        record->state = auth.needs_approval ? "awaiting_approval" : "queued";
        jobs[job_id] = record;
        // 接单结果链(单 §8:start 的接单结果即配齐调用)。审批未过也接单
        //(消息如实报 awaiting_approval),但不入队(单 §6:审批未过不派发)。
        // native_deferred 不接单:原调用保持欠账,业务结果由规划器在请求
        // 边界配原 call(单 §8 native 轨迹)。early 只落事实链,消息由
        // CompleteAdmission 在声明消息落账后补。
        if (!native) {
            const bool chain_ok =
                early ? WriteAdmissionFactsLocked(*record) : WriteAdmissionChainLocked(*record);
            if (!chain_ok) {
                result.error_code = "job.start.admission_write_failed";
                result.error = "接单结果链落账失败;job 已注册,恢复按 complete_delivery 补链";
                result.job_id = job_id;
                result.status = record->state = "registered";  // 不入队:接单没配齐不派发
                state_cv.notify_all();
                return result;
            }
            if (record->admission_complete) {
                result.admission_content = record->admission_text;
            }
        }
        if (record->state == "queued") {
            queue.push_back(job_id);
            TryDispatchLocked();
        }
        PumpLocked();
        state_cv.notify_all();
        result.ok = true;
        result.job_id = job_id;
        result.action_id = action_id;
        result.status = record->state;
        return result;
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
    impl_->named_results = std::move(options.named_results);
    if (impl_->named_results && impl_->named_results->scope().session_id != writer.session_id())
        throw std::invalid_argument("named_result.owner_mismatch");
    impl_->gate = std::move(gate);
    impl_->executor = std::move(executor);
    impl_->thread_starter = std::move(options.thread_starter);
    impl_->limits = options.limits;
    impl_->global = options.global != nullptr ? std::move(options.global)
                                              : std::make_shared<GlobalRunningQuota>();
    impl_->clock_ms = std::move(options.clock_ms);
    impl_->self_lock = impl_;
    if (options.prepared_registration) {
        auto context = std::move(*options.prepared_registration);
        if (!context.writer_serial || context.project_id.empty() || !context.cwd.is_absolute())
            throw std::invalid_argument("job.prepared.invalid_context");
        std::error_code error;
        const auto canonical = std::filesystem::canonical(context.cwd, error);
        if (error || canonical != context.cwd || !std::filesystem::is_directory(canonical, error) || error)
            throw std::invalid_argument("job.prepared.invalid_context");
        std::lock_guard serial(*context.writer_serial);
        static std::atomic<std::uint64_t> next_coordinator{1};
        auto instance = next_coordinator.load();
        for (;;) {
            if (instance == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("job.prepared.owner_exhausted");
            if (next_coordinator.compare_exchange_weak(instance, instance + 1)) break;
        }
        impl_->prepared_owner = PreparedJobOwner{writer.session_id(), writer.run_id(), instance, 1,
                                                 context.project_id, context.cwd};
        impl_->prepared_revoked = writer.closed();
        impl_->prepared_context = std::move(context);
    }
}

ToolJobCoordinator::~ToolJobCoordinator() {
    if (current_job_worker == impl_.get() || current_job_shutdown == impl_.get() || current_owned_callback == impl_.get()) std::terminate();
    if (!Shutdown() && !shutdown_complete()) std::terminate();
}

void ToolJobCoordinator::RequestShutdown() {
    if (impl_ == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
        impl_->closing = true;
        impl_->prepared_revoked = true;
        if (impl_->owned_post_phase != OwnedJobPostPhase::Retired)
            impl_->owned_post_phase = OwnedJobPostPhase::Draining;
        for (auto& [id, record] : impl_->prepared) { (void)id; record->revoked = true; }
        // Cancellation is an intent, not proof a callback has returned. This
        // phase never calls an authorization gate, clock or executor.
        for (auto& [id, job] : impl_->jobs) {
            (void)id;
            if (!job->recovery.has_value() && (job->state == "running" || job->state == "queued")) {
                job->cancel_flag->store(true);
            }
        }
        for (auto& [id, record] : impl_->owned) {
            (void)id;
            if (!record->settled) {
                record->job.cancel_requested = true;
                record->job.cancel_flag->store(true);
            }
        }
        for (const auto& worker : impl_->owned_workers) {
            worker.cancel->store(true);
        }
    }
    impl_->state_cv.notify_all();
}

bool ToolJobCoordinator::Shutdown() {
    if (impl_ == nullptr) return true;
    // A callback cannot wait for its own exit. Detect the invalid lifecycle
    // call rather than deadlocking or pretending its borrows have been drained.
    if (current_job_worker == impl_.get() || current_job_shutdown == impl_.get() || current_owned_callback == impl_.get()) return false;
    RequestShutdown();
    std::unique_lock shutdown(impl_->shutdown_mutex);
    impl_->shutdown_cv.wait(shutdown, [&] { return !impl_->shutdown_in_progress; });
    if (impl_->shutdown_complete) return impl_->shutdown_settlement_ok;
    impl_->shutdown_in_progress = true;
    shutdown.unlock();
    JobThreadScope shutdown_scope(current_job_shutdown, impl_.get());
    std::vector<Impl::Worker> workers;
    {
        std::lock_guard lock(impl_->jobs_mutex);
        workers.swap(impl_->workers);
    }
    for (auto& worker : workers) if (worker.thread.joinable()) worker.thread.join();
    // Only the new registration domain reads the real writer outside jobs.
    // Drain that serial lease before retiring its pointer; never hold it while
    // joining workers or destroying host callbacks/captures.
    std::unique_lock<std::recursive_mutex> prepared_serial;
    if (impl_->prepared_context)
        prepared_serial = std::unique_lock<std::recursive_mutex>(*impl_->prepared_context->writer_serial);
    bool settled = true;
    // Real joined completions are collected before retiring the shared writer.
    // Host Post and all receipt validation run outside jobs_mutex.
    if (impl_->prepared_context) {
        for (auto& [id, record] : impl_->owned) {
            (void)id;
            try {
            if (!record->settled) {
                if (!record->cancelled) {
                    record->cancelled = impl_->EmitCancelRequestedLocked(record->job, "session_shutdown");
                    if (!ReceiptOk(*record->cancelled)) impl_->FreezeOwned(*record, "job.owned.cancel_unconfirmed");
                }
                if (!record->settled && !record->job.dispatched)
                    impl_->CloseOwnedBeforeStart(*record, true, "session_shutdown");
                if (!record->settled && record->job.dispatched) {
                    bool found = false;
                    for (const auto& worker : workers) {
                        if (!worker.launch || worker.launch->completion.job_id != record->job.job_id) continue;
                        found = true;
                        if (worker.finished && worker.finished->load() && worker.launch->completion_ready.load()) {
                            impl_->SettleOwned(*record, worker.launch->completion);
                            worker.launch->completion_ready.store(false);
                        }
                        break;
                    }
                    if (!found || !record->settled) impl_->FreezeOwned(*record, "job.owned.completion_missing");
                }
            }
            } catch (...) {
                settled = false;
                impl_->FreezeOwned(*record, "job.owned.shutdown_unconfirmed");
            }
            {
                std::lock_guard lock(impl_->jobs_mutex);
                impl_->ReleaseDispatchReservationLocked(record->job);
            }
            settled = settled && record->settled && record->gap.empty();
        }
    }
    {
        std::lock_guard lock(impl_->jobs_mutex);
        // All callbacks have exited. Settlement can fail independently of
        // lifetime cleanup, and must not turn a close into false success.
        try {
            // Persist the cancellation intent now, after every session has
            // been signalled. Running completions below retain their real
            // success/error; work never dispatched is known not to execute.
            for (auto& [id, job] : impl_->jobs) {
                (void)id;
                if (job->recovery.has_value() || IsTerminalJobState(job->state)) continue;
                job->cancel_requested = true;
                job->cancel_flag->store(true);
                if (!job->cancel_event_written) {
                    const auto cancelled = impl_->EmitCancelRequestedLocked(*job, "session_shutdown");
                    if (ReceiptOk(cancelled)) job->cancel_event_written = true;
                    else settled = false;
                }
                if (!job->dispatched) {
                    const auto observed = impl_->ObserveLocked(*job, "cancelled");
                    if (ReceiptOk(observed)) job->state = "cancelled";
                    else settled = false;
                }
            }
            // The registration-only table has no business jobs/deadlines.
            // Do not enter the legacy pump or invoke its host clock on close.
            if (!impl_->prepared_context) impl_->PumpLocked();
            settled = settled && std::all_of(impl_->jobs.begin(), impl_->jobs.end(), [](const auto& item) {
                // Passive recovery projections own no live worker. Closing
                // them is not a receipt for their historical execution gaps.
                return item.second->recovery.has_value() ||
                    (IsTerminalJobState(item.second->state) && !item.second->startup_settlement_pending);
            });
        } catch (...) {
            settled = false;
        }
        for (const auto& worker : workers) {
            std::erase_if(impl_->owned_workers, [&](const auto& owned) { return owned.finished == worker.finished; });
        }
        impl_->owned_post_phase = OwnedJobPostPhase::Retired;
        impl_->writer = nullptr;
    }
    if (prepared_serial.owns_lock()) prepared_serial.unlock();
    // Closing and the drained serial lease prohibit new insertions/users of
    // these capabilities. Retire each capture without an allocating transfer
    // container and outside writer/jobs locks; owned status remains queryable.
    for (auto& [id, record] : impl_->owned) {
        (void)id;
        record->capability.reset();
    }
    // Closing APIs reject before reading these callbacks, and every worker is
    // joined. Clear the actual sources outside all lifecycle/jobs locks: a
    // std::function move may retain an inline callable in its source, and even
    // swap can destroy temporary callable copies. Capture destructors may query
    // ordinary APIs here; a concurrent Shutdown still waits for their return.
    impl_->gate = nullptr;
    impl_->executor = nullptr;
    impl_->thread_starter = nullptr;
    impl_->clock_ms = nullptr;
    workers.clear();  // Failed launch captures must exit before close is published.
    shutdown.lock();
    impl_->shutdown_settlement_ok = settled;
    impl_->shutdown_complete = true;
    impl_->shutdown_in_progress = false;
    shutdown.unlock();
    impl_->shutdown_cv.notify_all();
    impl_->state_cv.notify_all();
    return settled;
}

bool ToolJobCoordinator::shutdown_complete() const {
    return impl_ == nullptr || impl_->shutdown_complete.load();
}

std::optional<PreparedJobOwner> ToolJobCoordinator::PreparedOwner() const {
    if (!impl_->prepared_context) return std::nullopt;
    std::lock_guard serial(*impl_->prepared_context->writer_serial);
    std::lock_guard lock(impl_->jobs_mutex);
    if (impl_->closing || impl_->prepared_revoked || !impl_->writer) return std::nullopt;
    if (impl_->writer->closed()) {
        impl_->prepared_revoked = true;
        for (auto& [id, record] : impl_->prepared) { (void)id; record->revoked = true; }
        return std::nullopt;
    }
    return impl_->prepared_owner;
}

PreparedJobRegistration ToolJobCoordinator::RegisterPreparedJob(const PreparedJobRequest& request) {
    PreparedJobRegistration result;
    auto refuse = [&](const char* code, const std::string& message = std::string()) {
        result.error_code = code;
        result.error = message.empty() ? code : message;
        return result;
    };
    if (!impl_->prepared_context) return refuse("job.prepared.disabled");
    std::lock_guard serial(*impl_->prepared_context->writer_serial);
    trajectory::v3::V3Writer* writer = nullptr;
    {
        std::lock_guard lock(impl_->jobs_mutex);
        if (impl_->closing || impl_->prepared_revoked || !impl_->writer)
            return refuse("job.prepared.closed");
        writer = impl_->writer;
        if (writer->closed()) {
            impl_->prepared_revoked = true;
            for (auto& [id, record] : impl_->prepared) { (void)id; record->revoked = true; }
            return refuse("job.prepared.closed");
        }
        if (request.owner != impl_->prepared_owner || writer->session_id() != request.owner.session_id ||
            writer->run_id() != request.owner.run_id) return refuse("job.prepared.foreign_owner");
        if (writer->broken()) {
            impl_->prepared_revoked = true;
            result.state = PreparedJobRegistrationState::Unconfirmed;
            return refuse("job.prepared.writer_broken");  // No fabricated new receipt.
        }
    }
    if (request.provider_tool_call_id.empty() || request.assistant_message_ref.empty() ||
        request.parent_action_id.empty() || request.turn_id.empty() || request.step_id.empty() ||
        request.tool_name.empty() || !request.original_input.is_object() || !request.effective_input.is_object() ||
        request.tool_identity.logical_name != request.tool_name || request.tool_identity.registration_source.empty() ||
        request.tool_identity.version.empty() || request.tool_identity.execution_scope != platform::PathToUtf8(request.owner.cwd) ||
        !request.policy.allow_background || request.policy.resume_policy != "hold" || request.policy.retry_policy != "none")
        return refuse("job.prepared.bad_request");
    OwnedDeadline checked_deadline;
    if (!FreezeOwnedDeadline(request.policy.deadline_ms, checked_deadline))
        return refuse("job.prepared.deadline_unrepresentable");
    const auto original_bytes = trajectory::CanonicalJsonDump(request.original_input);
    const auto effective_bytes = trajectory::CanonicalJsonDump(request.effective_input);
    if (!original_bytes || !effective_bytes) return refuse("job.prepared.bad_arguments");
    const auto ledger = trajectory::v3::ReadV3Ledger(writer->path());
    if (!ledger) return refuse("job.prepared.invalid_ledger", ledger.error());
    const auto last = ledger->LastEntry();
    if (!last || ledger->session_id != request.owner.session_id ||
        last->seq == std::numeric_limits<std::uint64_t>::max() || last->seq + 1 != writer->next_seq())
        return refuse("job.prepared.writer_prefix_mismatch");
    const auto& last_hash = last->is_message ? ledger->messages[last->index].line_hash : ledger->events[last->index].line_hash;
    if (last_hash != writer->last_line_hash()) return refuse("job.prepared.writer_prefix_mismatch");
    const auto source = CheckPreparedSource(*ledger, request);
    if (!source) return refuse("job.prepared.invalid_source", source.error());

    // Allocate every stable owner/table node and both event payloads before a
    // real write. A later write exception cannot discard the retained owner.
    auto facts = std::make_shared<PreparedJobFacts>();
    facts->owner = request.owner;
    facts->provider_tool_call_id = request.provider_tool_call_id;
    facts->assistant_message_ref = request.assistant_message_ref;
    facts->parent_action_id = request.parent_action_id;
    facts->turn_id = request.turn_id;
    facts->step_id = request.step_id;
    facts->tool_name = request.tool_name;
    facts->original_input = request.original_input;
    facts->effective_input = request.effective_input;
    facts->original_input_sha256 = hooks::Sha256Hex(*original_bytes);
    facts->effective_input_sha256 = hooks::Sha256Hex(*effective_bytes);
    facts->tool_identity = request.tool_identity;
    facts->policy = request.policy;
    facts->source_pending_event_id = source->pending_event_id;
    facts->source_admission_event_id = source->admission_event_id;
    auto record = std::make_shared<PreparedRecord>();
    record->facts = facts;
    std::lock_guard lock(impl_->jobs_mutex);
    if (impl_->closing || impl_->prepared_revoked || impl_->writer != writer || writer->closed()) {
        impl_->prepared_revoked = true;
        return refuse("job.prepared.closed");
    }
    if (writer->session_id() != request.owner.session_id || writer->run_id() != request.owner.run_id ||
        last->seq + 1 != writer->next_seq() || last_hash != writer->last_line_hash())
        return refuse("job.prepared.writer_prefix_mismatch");
    if (impl_->prepared.size() >= impl_->limits.queued_max ||
        impl_->queue.size() >= impl_->limits.queued_max - impl_->prepared.size())
        return refuse("job.prepared.capacity");
    for (const auto& [id, existing] : impl_->prepared) {
        (void)id;
        if (existing->facts->parent_action_id == request.parent_action_id ||
            (existing->facts->assistant_message_ref == request.assistant_message_ref &&
             existing->facts->provider_tool_call_id == request.provider_tool_call_id))
            return refuse("job.prepared.duplicate");
    }
    facts->action_id = writer->NewActionId();
    facts->job_id = "job-owned-" + facts->action_id;
    EventDraft pending;
    pending.kind = EventKindV3::ToolExecutionPending;
    pending.status = trajectory::v3::OpStatus::Pending;
    pending.turn_id = request.turn_id;
    pending.step_id = request.step_id;
    pending.action_id = facts->action_id;
    pending.payload = {{"tool_call_id", facts->action_id}, {"attempt", 1}, {"reason", "queued"},
                       {"assistantMessageRef", request.assistant_message_ref},
                       {"provider_tool_call_id", request.provider_tool_call_id}, {"toolName", request.tool_name},
                       {"parentActionId", request.parent_action_id}, {"preparedOnly", true}};
    EventDraft registered;
    registered.kind = EventKindV3::ToolJobRegistered;
    registered.turn_id = request.turn_id;
    registered.step_id = request.step_id;
    registered.action_id = facts->action_id;
    registered.payload = {{"tool_call_id", facts->action_id}, {"attempt", 1}, {"jobId", facts->job_id},
        {"mode", "job_handle"}, {"assistantMessageRef", request.assistant_message_ref},
        {"provider_tool_call_id", request.provider_tool_call_id}, {"parentActionId", request.parent_action_id},
        {"preparedOnly", true}, {"executionPolicy", request.policy.ToJson()},
        {"effectiveInput", request.effective_input}, {"originalInputSha256", facts->original_input_sha256},
        {"effectiveInputSha256", facts->effective_input_sha256}, {"toolIdentity", request.tool_identity.ToJson()},
        {"sourcePendingEventRef", source->pending_event_id}, {"sourceAdmissionEventRef", source->admission_event_id},
        {"preparedOwner", {{"sessionId", request.owner.session_id}, {"runId", request.owner.run_id},
                           {"coordinatorId", request.owner.coordinator_id}, {"epoch", request.owner.epoch},
                           {"projectId", request.owner.project_id}, {"cwd", platform::PathToUtf8(request.owner.cwd)}}}};
    const auto [entry, inserted] = impl_->prepared.emplace(facts->job_id, record);
    if (!inserted) return refuse("job.prepared.duplicate");
    result.facts = facts;
    try {
        facts->pending_receipt.emplace(writer->AppendEvent(std::move(pending), Durability::ProcessCrash));
        if (!ReceiptOk(*facts->pending_receipt)) {
            const bool uncertain = writer->broken() || facts->pending_receipt->status == WriteReceipt::Status::IoFailed;
            record->state = result.state = uncertain ? PreparedJobRegistrationState::Unconfirmed : PreparedJobRegistrationState::Rejected;
            result.error_code = facts->pending_receipt->error_code;
            result.error = facts->pending_receipt->error_message;
            if (!uncertain) impl_->prepared.erase(entry);
            return result;
        }
        // Freeze immediately at the actual Registered append boundary. Its
        // native write latency consumes budget too. Recheck representability
        // after Pending, retaining that real prefix if the clock advanced too far.
        if (!FreezeOwnedDeadline(request.policy.deadline_ms, record->deadline)) {
            record->state = result.state = PreparedJobRegistrationState::Unconfirmed;
            result.error_code = result.error = "job.prepared.deadline_unrepresentable";
            return result;
        }
        facts->registered_receipt.emplace(writer->AppendEvent(std::move(registered), Durability::PowerLoss));
        if (!ReceiptOk(*facts->registered_receipt)) {
            // Pending really committed. A failed registration is a retained
            // gap even when the native rejection itself was deterministic.
            record->state = result.state = PreparedJobRegistrationState::Unconfirmed;
            result.error_code = facts->registered_receipt->error_code;
            result.error = facts->registered_receipt->error_message;
            return result;
        }
        record->state = result.state = PreparedJobRegistrationState::Registered;
        return result;
    } catch (...) {
        record->state = result.state = PreparedJobRegistrationState::Unconfirmed;
        result.error_code = "job.prepared.write_unconfirmed";
        result.error = "registration write threw; retained owner, no dispatch or retry";
        return result;
    }
}

std::optional<PreparedJobView> ToolJobCoordinator::GetPreparedJob(
    const PreparedJobOwner& owner, const std::string& job_id) const {
    if (!impl_->prepared_context) return std::nullopt;
    std::lock_guard serial(*impl_->prepared_context->writer_serial);
    std::lock_guard lock(impl_->jobs_mutex);
    if (owner != impl_->prepared_owner) return std::nullopt;
    if (impl_->writer && impl_->writer->closed()) {
        impl_->prepared_revoked = true;
        for (auto& [id, record] : impl_->prepared) { (void)id; record->revoked = true; }
    }
    const auto found = impl_->prepared.find(job_id);
    if (found == impl_->prepared.end()) return std::nullopt;
    return PreparedJobView{found->second->state, found->second->revoked || impl_->prepared_revoked, found->second->facts};
}

std::size_t ToolJobCoordinator::prepared_count() const {
    std::lock_guard lock(impl_->jobs_mutex);
    return impl_->prepared.size();
}


OwnedJobAdoption ToolJobCoordinator::AdoptPreparedJob(
    const PreparedJobOwner& owner, const std::string& id, OwnedJobCapability capability) {
    OwnedJobAdoption out;
    auto reject = [&](const std::string& code) { out.error_code = out.error = code; return out; };
    if (!impl_->prepared_context || current_owned_callback == impl_.get()) return reject("job.owned.disabled");
    // Keep every user capture outside the jobs lock, including rejection unwind.
    auto cap = std::make_shared<OwnedJobCapability>(std::move(capability));
    const auto& limit = cap->command_limits;
    if (!cap->command || !cap->scope_gate || (!cap->post && !cap->live_post) ||
        (cap->post && cap->live_post) || !limit.timeout_ms ||
        limit.timeout_ms > 86400000 || !limit.max_output_bytes ||
        limit.max_output_bytes > platform::kDefaultMaxOutputBytes) return reject("job.owned.missing_capability");
    std::lock_guard serial(*impl_->prepared_context->writer_serial);
    trajectory::v3::V3Writer* writer = nullptr;
    std::shared_ptr<const PreparedJobFacts> facts;
    OwnedDeadline deadline;
    {
        std::lock_guard lock(impl_->jobs_mutex);
        if (impl_->closing || impl_->prepared_revoked || !impl_->writer || impl_->writer->closed())
            return reject("job.owned.closed");
        if (owner != impl_->prepared_owner) return reject("job.owned.foreign_owner");
        const auto found = impl_->prepared.find(id);
        if (found == impl_->prepared.end() || found->second->revoked ||
            found->second->state != PreparedJobRegistrationState::Registered) return reject("job.owned.not_registered");
        if (impl_->owned.contains(id)) return reject("job.owned.already_adopted");
        facts = found->second->facts;
        deadline = found->second->deadline;
        writer = impl_->writer;
    }
    if (facts->policy.deadline_ms && !deadline) return reject("job.owned.deadline_unavailable");
    if (facts->tool_name != "run_command" || facts->tool_identity.logical_name != "run_command" ||
        cap->command->name() != "run_command" ||
        limit.max_output_bytes > facts->policy.max_output_bytes ||
        (facts->policy.deadline_ms && limit.timeout_ms > facts->policy.deadline_ms))
        return reject("job.owned.unsupported_binding");
    const auto cwd = facts->effective_input.find("cwd");
    if (cwd == facts->effective_input.end() || !cwd->is_string() ||
        platform::Utf8ToPath(cwd->get<std::string>()) != owner.cwd)
        return reject("job.owned.cwd_mismatch");
    if (facts->effective_input.contains("run_in_background") &&
        (!facts->effective_input.at("run_in_background").is_boolean() ||
         facts->effective_input.at("run_in_background").get<bool>())) return reject("job.owned.background_bypass");
    if (facts->effective_input.contains("max_runtime_ms")) return reject("job.owned.background_bypass");
    auto read = trajectory::v3::ReadV3Ledger(writer->path());
    if (!read || !SameWriterPrefix(*read, *writer)) return reject("job.owned.invalid_prefix");
    auto source = trajectory::v3::ReadOwnedJobRegistration(*read, id);
    if (!source || source->action_id != facts->action_id || source->parent_action_id != facts->parent_action_id ||
        source->session_id != owner.session_id || source->run_id != owner.run_id ||
        source->original_input != facts->original_input || source->effective_input != facts->effective_input ||
        source->tool_identity != facts->tool_identity.ToJson() || source->execution_policy != facts->policy.ToJson() ||
        source->original_input_sha256 != facts->original_input_sha256 ||
        source->effective_input_sha256 != facts->effective_input_sha256 ||
        source->prepared_pending_event_id != facts->pending_receipt->id ||
        source->registered_event_id != facts->registered_receipt->id) return reject("job.owned.invalid_source");
    const auto scope = ScopeOf(*facts);
    JobAuthDecision permission;
    try {
        JobThreadScope callback(current_owned_callback, impl_.get());
        permission = cap->scope_gate(scope, facts->effective_input, facts->tool_identity, facts->policy);
    } catch (...) { return reject("job.owned.scope_failed"); }
    if (!permission.allowed || permission.needs_approval) return reject("job.owned.scope_denied");
    // A trusted callback may have appended actual policy facts. Re-read rather
    // than treating the previous prefix check as an atomic admission lease.
    read = trajectory::v3::ReadV3Ledger(writer->path());
    if (!read || !SameWriterPrefix(*read, *writer)) return reject("job.owned.invalid_prefix");
    source = trajectory::v3::ReadOwnedJobRegistration(*read, id);
    if (!source) return reject("job.owned.invalid_source");
    auto record = std::make_shared<OwnedRecord>();
    record->facts = facts; record->scope = scope; record->capability = cap; record->deadline = deadline;
    auto& job = record->job;
    job.job_id = id; job.action_id = facts->action_id; job.turn_id = facts->turn_id; job.step_id = facts->step_id;
    job.tool_name = facts->tool_name; job.tool_input = facts->effective_input; job.policy = facts->policy;
    job.identity = facts->tool_identity; job.assistant_message_ref = facts->assistant_message_ref;
    job.state = "parent_delivery_pending";
    job.action = trajectory::v3::ToolActionSession::Reopen(facts->turn_id, facts->step_id, facts->action_id);
    EventDraft draft;
    draft.kind = EventKindV3::ToolJobAdopted;
    draft.turn_id = facts->turn_id; draft.step_id = facts->step_id; draft.action_id = facts->action_id;
    draft.payload = {{"tool_call_id", facts->action_id}, {"attempt", 1}, {"jobId", id},
        {"layout", trajectory::v3::kOwnedJobLayout}, {"parentActionId", facts->parent_action_id},
        {"provider_tool_call_id", facts->provider_tool_call_id}, {"toolName", facts->tool_name},
        {"effectiveInput", facts->effective_input}, {"originalInputSha256", facts->original_input_sha256},
        {"effectiveInputSha256", facts->effective_input_sha256}, {"toolIdentity", facts->tool_identity.ToJson()},
        {"executionPolicy", facts->policy.ToJson()}, {"preparedOwner", source->prepared_owner},
        {"commandLimits", {{"timeout_ms", limit.timeout_ms}, {"max_output_bytes", limit.max_output_bytes}}}};
    for (const auto& [name, ref] : std::vector<std::pair<std::string, std::string>>{
        {"assistantMessageRef", facts->assistant_message_ref}, {"sourcePendingEventRef", facts->source_pending_event_id},
        {"sourceAdmissionEventRef", facts->source_admission_event_id}, {"preparedPendingEventRef", facts->pending_receipt->id},
        {"registeredEventRef", facts->registered_receipt->id}}) {
        const auto actual = trajectory::v3::MakeOwnedJobReference(*read, ref);
        if (!actual) return reject("job.owned.invalid_reference");
        draft.payload[name] = *actual;
    }
    {
        std::lock_guard lock(impl_->jobs_mutex);
        if (impl_->closing || impl_->prepared_revoked || impl_->writer != writer || writer->closed())
            return reject("job.owned.closed");
        if (impl_->owned.contains(id)) return reject("job.owned.already_adopted");
        impl_->owned.emplace(id, record);  // Stable owner before the first real write.
    }
    out.facts = facts;
    try { record->adopted = writer->AppendEvent(std::move(draft), Durability::PowerLoss); }
    catch (...) { record->gap = "job.owned.adoption_unconfirmed"; }
    out.receipt = record->adopted;
    if (!record->adopted || !ReceiptOk(*record->adopted)) {
        impl_->FreezeOwned(*record, record->gap.empty() ? "job.owned.adoption_unconfirmed" : record->gap);
        out.state = OwnedJobAdoptionState::Unconfirmed;
        out.error_code = record->adopted ? record->adopted->error_code : record->gap;
        out.error = record->adopted ? record->adopted->error_message : record->gap;
        return out;
    }
    out.state = OwnedJobAdoptionState::Adopted;
    out.admission_content = nlohmann::json{{"jobId", id}, {"status", "parent_delivery_pending"},
        {"adoptionEventRef", record->adopted->id}, {"layout", trajectory::v3::kOwnedJobLayout}}.dump();
    job.admission_text = out.admission_content;
    return out;
}

OwnedJobAdmission ToolJobCoordinator::ConfirmParentAdmission(
    const PreparedJobOwner& owner, const std::string& id, const ParentJobAdmissionRefs& refs, bool confirm_cancelled_delivery) {
    OwnedJobAdmission out;
    auto reject = [&](const std::string& code) { out.error_code = out.error = code; return out; };
    if (!impl_->prepared_context || current_owned_callback == impl_.get()) return reject("job.owned.disabled");
    std::unique_lock serial(*impl_->prepared_context->writer_serial);
    std::shared_ptr<OwnedRecord> record;
    trajectory::v3::V3Writer* writer;
    {
        std::lock_guard lock(impl_->jobs_mutex);
        if (!impl_->writer || impl_->writer->closed()) return reject("job.owned.closed");
        if (owner != impl_->prepared_owner) return reject("job.owned.foreign_owner");
        const auto found = impl_->owned.find(id);
        if (found == impl_->owned.end()) return reject("job.owned.not_adopted");
        record = found->second; writer = impl_->writer;
        const bool cancelled_delivery = confirm_cancelled_delivery && record->job.cancel_requested && record->job.cancel_flag->load();
        if ((impl_->closing || impl_->prepared_revoked) && !cancelled_delivery) return reject("job.owned.closed");
        if (!record->gap.empty()) return reject(record->gap);
        if (record->job.admission_complete) { out.confirmed = true; return out; }
    }
    const auto ledger = trajectory::v3::ReadV3Ledger(writer->path());
    if (!ledger || !SameWriterPrefix(*ledger, *writer)) return reject("job.owned.invalid_prefix");
    const auto adoptions = trajectory::v3::ReadOwnedJobAdoptions(*ledger);
    if (!adoptions) return reject("job.owned.invalid_source");
    const auto* actual = trajectory::v3::FindOwnedJobAdoption(*adoptions, id);
    if (!actual || actual->adopted_event_id != record->adopted->id) return reject("job.owned.invalid_source");
    nlohmann::json parent = nlohmann::json::object();
    for (const auto& [name, ref] : std::vector<std::pair<std::string, std::string>>{
        {"terminalEventRef", refs.terminal_event_id}, {"persistedEventRef", refs.persisted_event_id},
        {"selectedEventRef", refs.selected_event_id}, {"toolMessageRef", refs.tool_message_id},
        {"admissionEventRef", refs.admission_event_id}}) {
        const auto value = trajectory::v3::MakeOwnedJobReference(*ledger, ref);
        if (!value) return reject("job.owned.parent_reference_missing");
        parent[name] = *value;
    }
    const auto check = trajectory::v3::CheckOwnedJobParentAdmission(*ledger, *actual, parent);
    if (!check) return reject(check.error());
    const auto expanded = trajectory::v3::ProjectResultPreview(*ledger, refs.tool_message_id);
    if (!expanded.complete || expanded.result_selection_ref != refs.selected_event_id ||
        expanded.source_result_event_refs.size() != 1 || expanded.source_result_event_refs.front() != refs.persisted_event_id)
        return reject("job.owned.parent_material_missing");
    // Persisted raw must be the truthful same handle; message text alone is
    // not proof that the selected artifact contained this registration result.
    bool found_text = false;
    for (const auto& ref : expanded.result_refs) {
        const auto bytes = ReadOwnedAdmissionArtifact(writer->path().parent_path(), ref, impl_->named_results);
        if (!bytes) return reject(bytes.error());
        if (JsonStr(ref, "mediaType") != "text/plain") continue;
        if (*bytes != record->job.admission_text)
            return reject("job.owned.parent_material_invalid");
        if (found_text) return reject("job.owned.parent_material_ambiguous");
        found_text = true;
    }
    if (!found_text) return reject("job.owned.parent_material_missing");
    const auto cancelled_delivery = [&] {
        std::lock_guard lock(impl_->jobs_mutex);
        return confirm_cancelled_delivery && record->job.cancel_requested && record->job.cancel_flag->load();
    };
    if (!cancelled_delivery()) {
        JobAuthDecision permission;
        try {
            JobThreadScope callback(current_owned_callback, impl_.get());
            permission = record->capability->scope_gate(record->scope, record->job.tool_input,
                                                        record->job.identity, record->job.policy);
        } catch (...) { return reject("job.owned.scope_failed"); }
        if ((!permission.allowed || permission.needs_approval) && !cancelled_delivery())
            return reject("job.owned.scope_denied");
    }
    // This opt-in confirms only the already verified parent delivery. The
    // actual private cancel latch remains set; Pump closes before dispatch.

    {
        std::lock_guard lock(impl_->jobs_mutex);
        const bool cancelled = record->job.cancel_requested || record->job.cancel_flag->load();
        const bool can_confirm_cancelled = confirm_cancelled_delivery && record->job.cancel_requested && record->job.cancel_flag->load();
        if (writer != impl_->writer || writer->closed() ||
            ((impl_->closing || impl_->prepared_revoked) && !can_confirm_cancelled)) return reject("job.owned.closed");
        if (cancelled && !can_confirm_cancelled) return reject("job.owned.cancelled");
        // Capacity counted when registered. Do not count one ticket twice.
        if (!record->settled) impl_->owned_queue.push_back(id);
        record->parent_admission = std::move(parent);
        record->job.admission_complete = true;
        if (!record->settled) record->job.state = "queued";
        out.confirmed = true;
    }
    serial.unlock();
    PumpOwnedJobs();
    return out;
}


std::size_t ToolJobCoordinator::PumpOwnedJobs() {
    if (!impl_->prepared_context || current_job_worker == impl_.get() || current_owned_callback == impl_.get()) return 0;
    impl_->ReapFinishedWorkers();
    std::lock_guard serial(*impl_->prepared_context->writer_serial);
    trajectory::v3::V3Writer* writer;
    std::vector<std::shared_ptr<OwnedRecord>> completions, queued;
    std::vector<std::pair<std::shared_ptr<OwnedRecord>, std::string>> cancellations;
    {
        std::lock_guard lock(impl_->jobs_mutex);
        if (!impl_->writer || impl_->writer->closed() || impl_->shutdown_complete) return 0;
        writer = impl_->writer;
        for (const auto& [id, record] : impl_->owned) {
            (void)id;
            if (!record->requested_cancel_reason.empty() && !record->cancelled && !record->settled)
                cancellations.emplace_back(record, record->requested_cancel_reason);
        }
        for (const auto& worker : impl_->owned_workers) {
            if (!worker.finished || !worker.finished->load(std::memory_order_acquire) || !worker.launch ||
                !worker.launch->completion_ready.load(std::memory_order_acquire)) continue;
            const auto found = impl_->owned.find(worker.launch->completion.job_id);
            if (found == impl_->owned.end()) continue;
            impl_->ReleaseDispatchReservationLocked(found->second->job);
            if (!found->second->settled && !found->second->settlement_started)
                completions.push_back(found->second);
            else {
                const auto& actual = worker.launch->completion;
                found->second->execution_state = actual.result.execution_control == ExecutionControl::StopIndeterminate ? "unknown" :
                    actual.cancelled ? "cancelled" : actual.succeeded ? "succeeded" : "failed";
                worker.launch->completion_ready.store(false, std::memory_order_release);
            }
        }
        for (const auto& id : impl_->owned_queue) {
            const auto found = impl_->owned.find(id);
            if (found != impl_->owned.end() && found->second->job.state == "queued" && !found->second->settled)
                queued.push_back(found->second);
        }
    }
    std::size_t settled = 0;
    for (const auto& [record, reason] : cancellations) {
        try {
            record->cancelled = impl_->EmitCancelRequestedLocked(record->job, reason);
            if (!ReceiptOk(*record->cancelled)) impl_->FreezeOwned(*record, "job.owned.cancel_unconfirmed");
            else if (!record->job.dispatched) {
                impl_->CloseOwnedBeforeStart(*record, true, "cancelled_before_dispatch");
                ++settled;
            }
        } catch (...) { impl_->FreezeOwned(*record, "job.owned.cancel_unconfirmed"); }
    }
    for (const auto& record : completions) {
        std::shared_ptr<Impl::WorkerLaunch> launch;
        {
            std::lock_guard lock(impl_->jobs_mutex);
            for (const auto& worker : impl_->owned_workers)
                if (worker.launch && worker.launch->completion.job_id == record->scope.job_id) { launch = worker.launch; break; }
        }
        if (!launch) continue;
        impl_->SettleOwned(*record, launch->completion);
        launch->completion_ready.store(false, std::memory_order_release);
        ++settled;
    }
    for (const auto& record : queued) {
        if (!record->gap.empty() || record->settled) continue;
        bool cancelled;
        {
            std::lock_guard lock(impl_->jobs_mutex);
            cancelled = impl_->closing || impl_->prepared_revoked || record->job.cancel_requested || record->job.cancel_flag->load();
        }
        if (cancelled) { impl_->CloseOwnedBeforeStart(*record, true, "cancelled_before_dispatch"); ++settled; continue; }
        if (!RemainingOwnedMilliseconds(record->deadline)) {
            impl_->ExpireOwnedBeforeStart(*record); ++settled; continue;
        }
        JobAuthDecision permission;
        try {
            JobThreadScope callback(current_owned_callback, impl_.get());
            permission = record->capability->scope_gate(record->scope, record->job.tool_input,
                                                       record->job.identity, record->job.policy);
        } catch (...) { permission = {false, false, "scope_callback_failed"}; }
        if (!permission.allowed || permission.needs_approval) {
            impl_->CloseOwnedBeforeStart(*record, false, permission.reason.empty() ? "scope_denied" : permission.reason);
            ++settled; continue;
        }
        if (!RemainingOwnedMilliseconds(record->deadline)) {
            impl_->ExpireOwnedBeforeStart(*record); ++settled; continue;
        }
        // Protect the stable destination from Shutdown's transfer while a
        // starter is between publishing a thread and returning/throwing.
        std::unique_lock startup(impl_->shutdown_mutex, std::try_to_lock);
        // A reaper can be joining a capture that queries this writer serial.
        // Leave this ticket queued instead of waiting under that serial lock.
        if (!startup.owns_lock() || impl_->shutdown_in_progress) continue;
        std::shared_ptr<Impl::WorkerLaunch> launch;
        std::shared_ptr<std::atomic<bool>> finished;
        bool staged = false;
        {
            std::lock_guard lock(impl_->jobs_mutex);
            auto& job = record->job;
            if (impl_->closing || impl_->prepared_revoked || job.cancel_requested || job.cancel_flag->load() ||
                job.state != "queued" || writer != impl_->writer) continue;
            if ((impl_->limits.session_running && impl_->RunningCountLocked() >= impl_->limits.session_running) ||
                (impl_->limits.per_tool && impl_->ToolRunningCountLocked(job.tool_name) >= impl_->limits.per_tool) ||
                !impl_->ResourceFreeLocked(job) || !impl_->global->TryAcquire()) continue;
            job.dispatch_quota_owned = true;
        }
        try {
            launch = std::make_shared<Impl::WorkerLaunch>();
            finished = std::make_shared<std::atomic<bool>>(false);
            record->worker_finished = finished;
            auto cap = record->capability;
            launch->owned_budget = std::make_shared<OwnedCommandBudget>(
                OwnedCommandBudget{record->deadline, cap->command_limits});
            launch->context = {record->job.job_id, record->job.tool_input, record->job.cancel_flag.get(),
                               cap->command_limits.max_output_bytes};
            // The producer, not an arbitrary executor, applies the actual
            // command context and leaves operation identity explicitly absent.
            launch->run = std::make_unique<JobExecutor>([cap, budget = launch->owned_budget](const JobExecutionContext& context) {
                ToolExecutionContext invocation;
                invocation.cancel = context.cancel;
                invocation.command_limits = budget->limits;
                return cap->command->execute(context.input, invocation);
            });
            launch->completion.job_id = record->job.job_id;
            launch->completion.owner_epoch = "epoch-1";
            launch->fallback_result = Tool::Result::Error("owned command executor threw");
            launch->fallback_result.error_code = "tool.job.executor_exception";
            launch->fallback_result.execution_control = ExecutionControl::StopIndeterminate;
            launch->fallback_error_code = "tool.job.completion_unconfirmed";
            EventDraft dispatch;
            dispatch.kind = EventKindV3::ToolJobDispatched;
            dispatch.turn_id = record->scope.turn_id; dispatch.step_id = record->scope.step_id;
            dispatch.action_id = record->scope.action_id;
            const auto ledger = trajectory::v3::ReadV3Ledger(writer->path());
            const auto adoption = ledger ? trajectory::v3::MakeOwnedJobReference(*ledger, record->adopted->id) : std::nullopt;
            if (!ledger || !SameWriterPrefix(*ledger, *writer) || !adoption) throw std::runtime_error("job.owned.invalid_prefix");
            dispatch.payload = {{"tool_call_id", record->scope.action_id}, {"attempt", 1}, {"jobId", record->scope.job_id},
                {"ownerEpoch", "epoch-1"}, {"adoptionEventRef", *adoption}, {"parentAdmission", record->parent_admission}};
            if (!RemainingOwnedMilliseconds(record->deadline)) {
                { std::lock_guard lock(impl_->jobs_mutex); impl_->ReleaseDispatchReservationLocked(record->job); }
                impl_->ExpireOwnedBeforeStart(*record); ++settled; continue;
            }
            record->dispatched = writer->AppendEvent(std::move(dispatch), Durability::PowerLoss);
            if (!ReceiptOk(*record->dispatched)) { impl_->FreezeOwned(*record, "job.owned.dispatch_unconfirmed"); }
            else {
                if (!RemainingOwnedMilliseconds(record->deadline)) {
                    { std::lock_guard lock(impl_->jobs_mutex); impl_->ReleaseDispatchReservationLocked(record->job); }
                    impl_->ExpireOwnedBeforeStart(*record); ++settled; continue;
                }
                const auto hash = record->facts->effective_input_sha256;
                record->started = record->job.action->Start(*writer, hash,
                    record->job.identity, trajectory::v3::ComputeToolIdempotencyKey(record->scope.action_id,
                        record->job.identity, hash, ""), nlohmann::json{{"toolName", "run_command"}}, Durability::PowerLoss);
                if (!ReceiptOk(*record->started)) impl_->FreezeOwned(*record, "job.owned.started_unconfirmed");
            }
            if (record->settled) {
                std::lock_guard lock(impl_->jobs_mutex);
                impl_->ReleaseDispatchReservationLocked(record->job); continue;
            }
            {
                std::lock_guard lock(impl_->jobs_mutex);
                impl_->workers.emplace_back();
                impl_->workers.back().finished = finished; impl_->workers.back().launch = launch;
                staged = true;
                impl_->owned_workers.push_back({record->job.cancel_flag, finished, launch});
                impl_->AcquireResourcesLocked(record->job);
                record->job.state = "running"; record->job.dispatched = true;
                record->job.owner_epoch = "epoch-1"; record->worker_finished = finished;
            }
            const auto self = impl_;
            const auto cancel = record->job.cancel_flag;
            std::function<void()> body = [self, launch, finished, cancel]() mutable {
                JobThreadScope worker(current_job_worker, self.get());
                (void)cancel; // Own the storage borrowed by launch->context.cancel.
                const auto begin = std::chrono::steady_clock::now();
                auto& envelope = launch->completion;
                try {
                    try {
                        // Started records intent. A late worker must check the
                        // live registration deadline before invoking a command;
                        // otherwise cancellation uses the actual platform path.
                        const auto remaining = RemainingOwnedMilliseconds(launch->owned_budget->deadline);
                        if (!remaining) {
                            envelope.command_not_invoked = true;
                            envelope.cancelled = true;
                            envelope.error_code = kOwnedDeadlineReason;
                        } else {
                            if (launch->owned_budget->deadline)
                                launch->owned_budget->limits = OwnedCommandLimits(
                                    launch->owned_budget->limits, launch->context.input, remaining);
                            envelope.result = (*launch->run)(launch->context);
                        }
                    } catch (...) { envelope.result = std::move(launch->fallback_result); }
                    if (!envelope.command_not_invoked) {
                        envelope.succeeded = !envelope.result.is_error && envelope.result.execution_control != ExecutionControl::StopIndeterminate;
                        envelope.cancelled = envelope.result.outcome == "cancelled_during_run";
                        envelope.error_code = envelope.result.error_code;
                    }
                    envelope.duration_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - begin).count());
                } catch (...) {
                    // Preserve the actual result already obtained. A reporting
                    // failure is unconfirmed, not evidence of zero execution.
                    envelope.succeeded = false; envelope.cancelled = false;
                    envelope.result.execution_control = ExecutionControl::StopIndeterminate;
                    envelope.error_code.swap(launch->fallback_error_code);
                }
                launch->completion_ready.store(true, std::memory_order_release);
                self->state_cv.notify_all();
                launch->run.reset(); // Actual executor capture retirement precedes finished.
                finished->store(true, std::memory_order_release);
                self->state_cv.notify_all();
            };
            {
                JobThreadScope callback(current_owned_callback, impl_.get());
                // Stable owner is already in workers. No jobs lock crosses
                // user starter code; Shutdown waits this publication lease.
                if (impl_->thread_starter) impl_->thread_starter(impl_->workers.back().thread, std::move(body));
                else impl_->workers.back().thread = std::thread(std::move(body));
            }
            if (!impl_->workers.back().thread.joinable()) throw std::runtime_error("thread starter returned no owned thread");
        } catch (...) {
            if (staged && impl_->workers.back().thread.joinable()) continue; // Real execution wins over starter throw.
            if (finished) finished->store(true, std::memory_order_release);
            {
                std::lock_guard lock(impl_->jobs_mutex);
                impl_->ReleaseDispatchReservationLocked(record->job);
            }
            if (record->started && ReceiptOk(*record->started) && !record->settled) {
                // A real started intent is not a command result. No worker
                // was published, so no synthetic command raw/Post is produced.
                {
                    std::lock_guard lock(impl_->jobs_mutex);
                    record->settlement_started = true;
                }
                try {
                    record->execution_state = "failed";
                    record->terminal = record->job.action->Fail(*writer, "tool.job.thread_start_failed");
                    if (!ReceiptOk(*record->terminal)) impl_->FreezeOwned(*record, "job.owned.terminal_unconfirmed");
                    else {
                        EventDraft observation;
                        observation.kind = EventKindV3::ToolJobObserved;
                        observation.turn_id = record->scope.turn_id; observation.step_id = record->scope.step_id;
                        observation.action_id = record->scope.action_id;
                        observation.payload = {{"tool_call_id", record->scope.action_id}, {"jobId", record->scope.job_id},
                            {"observedStatus", "failed"}, {"startupFailed", true}};
                        record->observed = writer->AppendEvent(std::move(observation), Durability::PowerLoss);
                        if (!ReceiptOk(*record->observed)) impl_->FreezeOwned(*record, "job.owned.observed_unconfirmed");
                        else {
                            std::lock_guard lock(impl_->jobs_mutex);
                            record->job.state = "failed"; record->settled = true;
                        }
                    }
                } catch (...) { impl_->FreezeOwned(*record, "job.owned.startup_unconfirmed"); }
            } else if (!record->settled) impl_->FreezeOwned(*record, "job.owned.startup_unconfirmed");
        }
    }
    {
        std::lock_guard lock(impl_->jobs_mutex);
        std::erase_if(impl_->owned_queue, [&](const auto& id) {
            const auto found = impl_->owned.find(id);
            return found == impl_->owned.end() || found->second->job.state != "queued";
        });
    }
    impl_->state_cv.notify_all();
    return settled;
}

OwnedJobStatusView ToolJobCoordinator::GetOwnedJob(const PreparedJobOwner& owner, const std::string& id) {
    OwnedJobStatusView out;
    if (!impl_->prepared_context) { out.access_denied = true; return out; }
    {
        std::lock_guard lock(impl_->jobs_mutex);
        if (owner != impl_->prepared_owner || !impl_->owned.contains(id)) {
            out.access_denied = true; return out;
        }
    }
    if (current_owned_callback != impl_.get()) PumpOwnedJobs();
    return SnapshotOwnedJob(owner, id);
}

OwnedJobStatusView ToolJobCoordinator::SnapshotOwnedJob(const PreparedJobOwner& owner,
                                                       const std::string& id) const {
    OwnedJobStatusView out;
    if (!impl_->prepared_context) { out.access_denied = true; return out; }
    std::lock_guard serial(*impl_->prepared_context->writer_serial);
    std::lock_guard lock(impl_->jobs_mutex);
    const auto found = impl_->owned.find(id);
    if (owner != impl_->prepared_owner || found == impl_->owned.end()) { out.access_denied = true; return out; }
    const auto& record = *found->second;
    out.scope = record.scope; out.state = record.job.state; out.execution_state = record.execution_state; out.gap = record.gap;
    out.preview = record.job.preview; out.preview_truncated = record.job.preview_truncated; out.result_ref = record.job.result_ref;
    out.command_not_invoked = record.command_not_invoked;
    out.cancel_requested = record.job.cancel_requested; out.revoked = impl_->prepared_revoked;
    out.worker_finished = record.worker_finished && record.worker_finished->load(std::memory_order_acquire);
    out.adopted_receipt = record.adopted; out.dispatched_receipt = record.dispatched; out.started_receipt = record.started;
    out.terminal_receipt = record.terminal; out.persisted_receipt = record.persisted; out.post_receipt = record.post; out.observed_receipt = record.observed;
    return out;
}

std::expected<void, std::string> ToolJobCoordinator::WithOwnedJobBindingSource(
    trajectory::v3::V3Writer& actual_writer, const std::string& id,
    const std::function<void(const OwnedJobBindingSource&)>& consume) const {
    if (!impl_->prepared_context || !consume || current_owned_callback == impl_.get())
        return std::unexpected("job.binding.source_unavailable");
    std::lock_guard serial(*impl_->prepared_context->writer_serial);
    OwnedJobBindingSource source;
    {
        std::lock_guard lock(impl_->jobs_mutex);
        const auto found = impl_->owned.find(id);
        if (impl_->closing || impl_->prepared_revoked || impl_->owned_post_phase != OwnedJobPostPhase::Open ||
            impl_->writer != &actual_writer || actual_writer.closed() || actual_writer.broken() ||
            found == impl_->owned.end()) return std::unexpected("job.binding.source_unavailable");
        const auto& record = *found->second;
        if (!record.facts || record.facts->owner != impl_->prepared_owner ||
            record.scope.attempt != 1 || record.scope.owner != impl_->prepared_owner ||
            record.job.state != "parent_delivery_pending" || record.job.dispatched ||
            record.settled || record.settlement_started || !record.gap.empty() || record.started ||
            record.dispatched || record.terminal || !record.adopted || !ReceiptOk(*record.adopted))
            return std::unexpected("job.binding.source_unavailable");
        source = {record.facts, *record.adopted};
    }
    JobThreadScope callback(current_owned_callback, impl_.get());
    consume(source);
    return {};
}

std::expected<OwnedJobPostSnapshot, std::string> ToolJobCoordinator::CheckOwnedPostInvocation(
    const OwnedJobPostInvocation& invocation) const {
    const auto state = invocation.state_.lock();
    // Reject before locks/pointer access: an observer may be joined by the host
    // holding writer_serial, and saved weak handles must never retain borrows.
    if (!state || state->issuer != impl_.get() ||
        state->thread != std::this_thread::get_id() ||
        current_owned_post_invocation != state.get() || current_owned_callback != impl_.get() ||
        !state->active.load(std::memory_order_acquire) || !impl_->prepared_context)
        return std::unexpected("job.post.invalid_invocation");
    std::lock_guard serial(*impl_->prepared_context->writer_serial);
    std::lock_guard lock(impl_->jobs_mutex);
    if (impl_->owned_post_phase == OwnedJobPostPhase::Retired || !impl_->writer ||
        impl_->writer->closed() || impl_->writer->broken())
        return std::unexpected("job.post.writer_unavailable");
    const auto& completion = *state->completion;
    const auto found = impl_->owned.find(completion.scope.job_id);
    if (state->epoch != impl_->prepared_owner.epoch ||
        completion.scope.owner != impl_->prepared_owner || completion.scope.attempt != 1 ||
        found == impl_->owned.end() || found->second.get() != state->record)
        return std::unexpected("job.post.invalid_invocation");
    const auto& record = *found->second;
    const auto matches = [](const auto& receipt, const auto& actual) {
        return receipt && ReceiptOk(*receipt) && receipt->id == actual.id &&
            receipt->seq == actual.seq && receipt->line_hash == actual.line_hash;
    };
    if (!record.settlement_started || record.settled || !record.post_invoked ||
        !record.job.dispatched || !record.capability || !record.capability->live_post ||
        !record.facts || !record.adopted || !ReceiptOk(*record.adopted) ||
        record.scope.action_id != completion.scope.action_id ||
        record.scope.turn_id != completion.scope.turn_id ||
        record.scope.step_id != completion.scope.step_id ||
        record.scope.owner != completion.scope.owner ||
        impl_->writer->session_id() != completion.scope.owner.session_id ||
        impl_->writer->run_id() != completion.scope.owner.run_id ||
        !matches(record.started, completion.started_receipt) ||
        !matches(record.terminal, completion.terminal_receipt) ||
        !matches(record.persisted, completion.persisted_receipt))
        return std::unexpected("job.post.invalid_invocation");
    return OwnedJobPostSnapshot{completion, record.facts, *record.adopted,
        impl_->owned_post_phase, record.job.cancel_requested};
}

OwnedJobWaitResult ToolJobCoordinator::WaitOwnedJobs(const PreparedJobOwner& owner,
    const std::vector<std::string>& ids, std::uint64_t timeout_ms, bool all) {
    OwnedJobWaitResult out;
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::time_point::max() - now).count();
    const auto deadline = now + std::chrono::milliseconds(std::min<std::uint64_t>(timeout_ms,
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, remaining))));
    for (;;) {
        out.statuses.clear();
        for (const auto& id : ids) out.statuses.push_back(GetOwnedJob(owner, id));
        const auto terminal = [](const auto& value) {
            return !value.access_denied && IsTerminalJobState(value.state) &&
                (!value.started_receipt || !ReceiptOk(*value.started_receipt) || value.worker_finished);
        };
        out.satisfied = !ids.empty() && (all ? std::all_of(out.statuses.begin(), out.statuses.end(), terminal)
                                          : std::any_of(out.statuses.begin(), out.statuses.end(), terminal));
        if (out.satisfied) return out;
        if (current_owned_callback == impl_.get() || current_job_worker == impl_.get() ||
            std::chrono::steady_clock::now() >= deadline || impl_->shutdown_complete) { out.timed_out = true; return out; }
        std::unique_lock lock(impl_->jobs_mutex);
        impl_->state_cv.wait_for(lock, std::chrono::milliseconds(10));
    }
}

JobCancelResult ToolJobCoordinator::RequestOwnedCancellation(const PreparedJobOwner& owner,
    const std::string& id, const std::string& reason) {
    JobCancelResult out;
    if (!impl_->prepared_context) { out.error = "job.owned.disabled"; return out; }
    {
        std::lock_guard lock(impl_->jobs_mutex);
        const auto found = impl_->owned.find(id);
        if (owner != impl_->prepared_owner || found == impl_->owned.end()) {
            out.error = "job.owned.foreign_owner"; return out;
        }
        auto& record = *found->second;
        if (record.settled && record.job.state != "unknown") {
            out.ok = true; out.status = "already_terminal"; out.terminal = record.job.state; return out;
        }
        // Unknown may still own an executing process. Retain that knowledge and
        // the first gap while signalling its real cancellation flag.
        if (record.requested_cancel_reason.empty())
            record.requested_cancel_reason = reason.empty() ? "cancel_requested" : reason;
        record.job.cancel_requested = true;
        record.job.cancel_flag->store(true, std::memory_order_release);
        out.ok = true; out.status = "cancel_requested";
    }
    impl_->state_cv.notify_all();
    return out;
}

JobCancelResult ToolJobCoordinator::CancelOwnedJob(const PreparedJobOwner& owner,
    const std::string& id, const std::string& reason) {
    JobCancelResult out;
    if (!impl_->prepared_context || current_owned_callback == impl_.get()) { out.error = "job.owned.disabled"; return out; }
    std::unique_lock serial(*impl_->prepared_context->writer_serial);
    std::shared_ptr<OwnedRecord> record;
    {
        std::lock_guard lock(impl_->jobs_mutex);
        const auto found = impl_->owned.find(id);
        if (owner != impl_->prepared_owner || found == impl_->owned.end()) { out.error = "job.owned.foreign_owner"; return out; }
        record = found->second;
        if (record->settled) { out.ok = true; out.status = "already_terminal"; out.terminal = record->job.state; return out; }
        if (impl_->closing || impl_->prepared_revoked || !impl_->writer || impl_->writer->closed()) { out.error = "job.owned.closed"; return out; }
        record->job.cancel_requested = true;
        record->job.cancel_flag->store(true);
    }
    if (!record->cancelled) {
        record->cancelled = impl_->EmitCancelRequestedLocked(record->job, reason.empty() ? "cancel_requested" : reason);
        if (!ReceiptOk(*record->cancelled)) { impl_->FreezeOwned(*record, "job.owned.cancel_unconfirmed"); out.error = record->gap; return out; }
    }
    if (!record->job.dispatched) impl_->CloseOwnedBeforeStart(*record, true, "cancelled_before_dispatch");
    out.ok = true; out.status = "cancel_requested";
    serial.unlock(); impl_->state_cv.notify_all();
    return out;
}

// ---------------------------------------------------------------------------
// 四接口
// ---------------------------------------------------------------------------

JobStartResult ToolJobCoordinator::StartJob(const JobStartRequest& request) {
    if (impl_->prepared_context) return JobStartResult{false, "job.prepared.registration_only", "legacy start disabled in prepared domain"};
    impl_->ReapFinishedWorkers();
    return impl_->StartJobCommon(request, /*early=*/false);
}

JobStartResult ToolJobCoordinator::StartJobEarly(const JobStartRequest& request) {
    if (impl_->prepared_context) return JobStartResult{false, "job.prepared.registration_only", "legacy early start disabled in prepared domain"};
    impl_->ReapFinishedWorkers();
    return impl_->StartJobCommon(request, /*early=*/true);
}

bool ToolJobCoordinator::CompleteAdmission(const std::string& job_id,
                                           std::string* admission_content) {
    if (impl_->prepared_context) return false;
    impl_->ReapFinishedWorkers();
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    if (impl_->closing) return false;
    auto it = impl_->jobs.find(job_id);
    if (it == impl_->jobs.end()) {
        return false;
    }
    JobRecord& job = *it->second;
    if (job.recovery.has_value()) {
        if (admission_content != nullptr)
            *admission_content = job.admission_complete ? job.admission_text : std::string();
        return job.admission_complete;  // Never repair an old held admission.
    }
    if (job.admission_facts_complete && !job.admission_complete) {
        if (!impl_->WriteAdmissionMessageLocked(job)) {
            return false;  // 接单链落账失败:恢复按 complete_delivery 补
        }
    }
    if (admission_content != nullptr) {
        *admission_content = job.admission_complete ? job.admission_text : std::string();
    }
    return job.admission_complete;
}

JobStartResult ToolJobCoordinator::GrantApproval(const std::string& job_id) {
    if (impl_->prepared_context) return JobStartResult{false, "job.prepared.registration_only", "legacy grant disabled in prepared domain"};
    impl_->ReapFinishedWorkers();
    JobStartResult result;
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    if (impl_->closing) {
        result.error_code = "job.grant.closing";
        result.error = "job.coordinator.closed";
        return result;
    }
    auto it = impl_->jobs.find(job_id);
    if (it == impl_->jobs.end()) {
        result.error_code = "job.grant.unknown_job";
        result.error = "job 不在本协调器: " + job_id;
        return result;
    }
    JobRecord& job = *it->second;
    if (job.recovery.has_value()) {
        result.error_code = "job.recovery.held";
        result.error = "held historical jobs cannot be granted or dispatched";
        return result;
    }
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
    if (impl_->prepared_context) {
        JobStatusView view; view.job_id = job_id; view.access_denied = true;
        view.access_reason = "job.prepared.registration_only"; return view;
    }
    impl_->ReapFinishedWorkers();
    JobStatusView view;
    view.job_id = job_id;
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    if (impl_->closing) {
        view.access_denied = true;
        view.access_reason = "job.coordinator.closed";
        return view;
    }
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
    view.action_id = job.action_id;
    view.cancel_requested = job.cancel_requested;
    view.result_ref = job.result_ref;
    view.result_version = job.result_version;
    view.preview = job.preview;
    view.preview_truncated = job.preview_truncated;
    view.failure = job.failure;
    view.recovery = job.recovery;
    return view;
}

JobWaitResult ToolJobCoordinator::WaitJobs(const std::vector<std::string>& job_ids,
                                           std::uint64_t timeout_ms, bool wait_all) {
    if (impl_->prepared_context) {
        JobWaitResult result;
        for (const auto& id : job_ids) {
            JobStatusView view; view.job_id = id; view.access_denied = true;
            view.access_reason = "job.prepared.registration_only";
            result.statuses.push_back(std::move(view));
        }
        return result;
    }
    impl_->ReapFinishedWorkers();
    JobWaitResult result;
    if (job_ids.empty()) {
        result.satisfied = true;
        return result;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(impl_->jobs_mutex);
    for (;;) {
        if (impl_->closing) {
            result.statuses.clear();
            for (const auto& id : job_ids) {
                JobStatusView view;
                view.job_id = id;
                view.access_denied = true;
                view.access_reason = "job.coordinator.closed";
                result.statuses.push_back(std::move(view));
            }
            return result;
        }
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
                    if (job.recovery.has_value()) view.action_id = job.action_id;
                    view.cancel_requested = job.cancel_requested;
                    view.result_ref = job.result_ref;
                    view.result_version = job.result_version;
                    view.preview = job.preview;
                    view.preview_truncated = job.preview_truncated;
                    view.failure = job.failure;
                    view.recovery = job.recovery;
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
    if (impl_->prepared_context) return JobCancelResult{false, "job.prepared.registration_only", "registration_only", {}};
    impl_->ReapFinishedWorkers();
    JobCancelResult result;
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    if (impl_->closing) {
        result.status = "closing";
        result.error = "job.coordinator.closed";
        return result;
    }
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
        if (job.recovery.has_value() &&
            job.recovery->knowledge != JobRecoveryKnowledge::TerminalConfirmed) {
            result.status = "recovery_held";
            result.error = "job.recovery.held";
            return result;
        }
        result.ok = true;
        result.status = "already_terminal";
        result.terminal = job.state;
        return result;
    }
    if (job.recovery.has_value()) {
        result.status = "recovery_held";
        result.error = "job.recovery.held";
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
    if (impl_->prepared_context) return 0;
    impl_->ReapFinishedWorkers();
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    if (impl_->closing) return 0;
    std::size_t settled = impl_->PumpLocked();
    impl_->state_cv.notify_all();
    return settled;
}

bool ToolJobCoordinator::DebugSubmitEnvelope(const std::string& job_id,
                                             const std::string& owner_epoch, Tool::Result result) {
    if (impl_->prepared_context) return false;
    impl_->ReapFinishedWorkers();
    // Hold the publication gate through posting. Shutdown must never lose an
    // envelope accepted after its final settlement and callback release.
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    if (impl_->closing) return false;
    const auto historical = impl_->jobs.find(job_id);
    if (historical != impl_->jobs.end() && historical->second->recovery.has_value()) {
        ++impl_->stale_rejected;
        return false;
    }
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

JobRecoveryPlan ToolJobCoordinator::PlanRecovery(const trajectory::v3::V3Ledger& ledger,
                                                JobRecoveryPolicy policy) {
    JobRecoveryPlan plan;
    plan.policy = policy;
    plan.source_session_id = ledger.session_id;
    const auto owned_adoptions = trajectory::v3::ReadOwnedJobAdoptions(ledger);
    if (!owned_adoptions) throw std::invalid_argument(owned_adoptions.error());
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
            const auto prepared = event.payload.find("preparedOnly");
            if (prepared != event.payload.end() && !prepared->is_boolean())
                throw std::invalid_argument("job.recovery.invalid_prepared_marker");
            registered[job_id] = &event;
        } else if (event.kind == EventKindV3::ToolJobDispatched) {
            dispatch_counts[job_id] += 1;
        } else if (event.kind == EventKindV3::ToolResultPersisted && event.action_id.has_value()) {
            const auto attempt_it = event.payload.find("attempt");
            if (attempt_it != event.payload.end() && attempt_it->is_number_unsigned() &&
                (attempt_it->get<std::uint64_t>() > 1 ||
                 trajectory::v3::IsOwnedJobBusinessAction(ledger, *event.action_id))) {
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
            const auto prepared = reg->second->payload.find("preparedOnly");
            item.prepared_only = prepared != reg->second->payload.end() && prepared->is_boolean() && prepared->get<bool>();
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
        const auto* adoption = trajectory::v3::FindOwnedJobAdoption(*owned_adoptions, job.job_id);
        if (adoption) {
            item.owned_layout = true;
            item.prepared_only = false; // True business facts must not hide behind the old temporary flag.
            item.tool_name = adoption->tool_identity.value("logicalName", std::string());
            item.tool_input = adoption->effective_input;
            item.turn_id = adoption->turn_id; item.step_id = adoption->step_id;
            JobRecoveryFacts facts;
            facts.original_state = job.state; facts.turn_id = item.turn_id; facts.step_id = item.step_id;
            facts.dispatched_count = item.dispatched_count;
            facts.execution_attempt = 1; facts.execution_started = item.attempt_started;
            if (snap && !snap->attempts.empty()) facts.execution_state = snap->attempts.back().status;
            facts.execution_terminal_event = item.attempt_terminal_event;
            bool post_confirmed = false;
            bool command_not_invoked = false;
            for (const auto& event : ledger.events) {
                if ((event.kind == EventKindV3::ToolJobDispatched || event.kind == EventKindV3::ToolJobObserved) &&
                    JsonStr(event.payload, "jobId") == item.job_id && event.payload.contains("parentAdmission")) {
                    const auto refs = event.payload.find("parentAdmission");
                    item.admission_complete = refs != event.payload.end() &&
                        trajectory::v3::CheckOwnedJobParentAdmission(ledger, *adoption, *refs).has_value();
                }
                if (event.kind == EventKindV3::HookCompleted && event.action_id == item.action_id &&
                    event.session_id == adoption->session_id && event.run_id == adoption->run_id &&
                    event.turn_id == item.turn_id && event.step_id == item.step_id) {
                    const auto post_ref = trajectory::v3::MakeOwnedJobReference(ledger, event.event_id);
                    if (post_ref && trajectory::v3::CheckOwnedJobPost(ledger, *adoption,
                            item.attempt_terminal_event, item.business_result_ref, *post_ref)) post_confirmed = true;
                }
                // ReadOwnedJobAdoptions already checked version, scope, parent
                // delivery, positive budget, Cancel and absence of raw/Post.
                if (event.kind == EventKindV3::ToolJobObserved && event.action_id == item.action_id &&
                    JsonStr(event.payload, "jobId") == item.job_id && event.payload.contains("commandNotInvoked"))
                    command_not_invoked = true;
            }
            facts.admission_complete = item.admission_complete;
            const bool terminal = item.attempt_terminal != 0;
            const bool observed = IsTerminalJobState(job.state);
            const bool known_unstarted = !facts.execution_started && item.dispatched_count == 0 &&
                (facts.execution_state == "cancelled" || facts.execution_state == "rejected");
            const auto* terminal_event = ledger.FindEvent(item.attempt_terminal_event);
            const bool known_start_failure = terminal_event && terminal_event->kind == EventKindV3::ToolExecutionFailed &&
                JsonStr(terminal_event->payload, "error_code") == "tool.job.thread_start_failed";
            const bool deadline_before_start = !facts.execution_started && terminal_event &&
                terminal_event->kind == EventKindV3::ToolExecutionCancelled &&
                JsonStr(terminal_event->payload, "phase") == "before_started" &&
                JsonStr(terminal_event->payload, "reason") == kOwnedDeadlineReason && item.policy.deadline_ms > 0;
            facts.command_not_invoked = command_not_invoked || deadline_before_start;
            const bool result_complete = known_unstarted || known_start_failure || facts.command_not_invoked || (!item.business_result_ref.empty() &&
                item.result_ref == item.business_result_ref && post_confirmed);
            const bool matches = (job.state == "succeeded" && facts.execution_state == "done") ||
                (job.state == "failed" && (facts.execution_state == "failed" || facts.execution_state == "rejected")) ||
                (job.state == "cancelled" && facts.execution_state == "cancelled");
            if (facts.execution_state == "unknown" || job.state == "unknown")
                facts.knowledge = JobRecoveryKnowledge::ExecutionUnconfirmed;
            else if (terminal || observed)
                facts.knowledge = observed && terminal && matches && result_complete &&
                    (item.admission_complete || known_unstarted) ? JobRecoveryKnowledge::TerminalConfirmed
                                                               : JobRecoveryKnowledge::TerminalDeliveryGap;
            else if (facts.execution_started || item.dispatched_count)
                facts.knowledge = JobRecoveryKnowledge::ExecutionUnconfirmed;
            else facts.knowledge = JobRecoveryKnowledge::KnownNotDispatched;
            item.recovery = facts;
            item.disposition = "owned_hold";
            item.detail = facts.knowledge == JobRecoveryKnowledge::TerminalConfirmed ? "owned_terminal_confirmed" :
                facts.knowledge == JobRecoveryKnowledge::TerminalDeliveryGap ? "owned_terminal_delivery_gap" :
                facts.knowledge == JobRecoveryKnowledge::ExecutionUnconfirmed ? "owned_execution_unconfirmed" :
                "owned_adopted_not_dispatched";
            if (observed) item.terminal_state = job.state;
            plan.items.push_back(std::move(item));
            continue;
        }
        // 接单消息在当前链上?(job_handle 必有;缺则补链不重跑,单 §6。)
        const bool admission_message_done =
            snap != nullptr &&
            std::any_of(snap->message_versions.begin(), snap->message_versions.end(),
                        [](const auto& version) { return version.on_current_chain; });
        item.admission_complete = admission_message_done;
        if (policy == JobRecoveryPolicy::Hold) {
            JobRecoveryFacts facts;
            facts.original_state = job.state;
            facts.turn_id = item.turn_id;
            facts.step_id = item.step_id;
            facts.dispatched_count = item.dispatched_count;
            facts.admission_complete = item.admission_complete;
            if (snap != nullptr) {
                for (auto attempt = snap->attempts.rbegin(); attempt != snap->attempts.rend(); ++attempt) {
                    if (job.mode == "job_handle" && attempt->attempt <= 1) continue;
                    facts.execution_attempt = attempt->attempt;
                    facts.execution_started = attempt->started;
                    facts.execution_state = attempt->status;
                    const auto terminal = terminal_events.find(job.origin_action_id);
                    if (terminal != terminal_events.end() && terminal->second.first == attempt->attempt)
                        facts.execution_terminal_event = terminal->second.second;
                    break;
                }
                for (const auto& version : snap->message_versions) {
                    if (!version.on_current_chain) continue;
                    const auto message = ledger.message_index.find(version.message_id);
                    if (message != ledger.message_index.end())
                        item.admission_text = JsonStr(ledger.messages[message->second].message, "content");
                }
            }
            const bool business_terminal = !facts.execution_terminal_event.empty();
            const bool observed_terminal = IsTerminalJobState(job.state);
            const bool known_unexecuted_cancel = job.state == "cancelled" &&
                item.dispatched_count == 0 && !facts.execution_started && !business_terminal;
            const bool outcome_matches =
                (job.state == "succeeded" && facts.execution_state == "done") ||
                (job.state == "failed" && (facts.execution_state == "failed" || facts.execution_state == "rejected")) ||
                (job.state == "cancelled" && facts.execution_state == "cancelled") || known_unexecuted_cancel;
            // Execution/observation can be confirmed even when storing a
            // successful result failed. That delivery gap is not a complete
            // success receipt. Errors/cancellation have no body-store duty.
            const bool result_complete = job.state != "succeeded" ||
                (!item.business_result_ref.empty() && item.result_ref == item.business_result_ref);
            if (job.mode != "job_handle") facts.knowledge = JobRecoveryKnowledge::UnsupportedMode;
            else if (job.state == "unknown" || facts.execution_state == "unknown")
                facts.knowledge = JobRecoveryKnowledge::ExecutionUnconfirmed;
            else if (observed_terminal || business_terminal)
                facts.knowledge = observed_terminal && item.admission_complete && outcome_matches && result_complete
                    ? JobRecoveryKnowledge::TerminalConfirmed : JobRecoveryKnowledge::TerminalDeliveryGap;
            else if (item.dispatched_count != 0 || facts.execution_started)
                facts.knowledge = JobRecoveryKnowledge::ExecutionUnconfirmed;
            else facts.knowledge = JobRecoveryKnowledge::KnownNotDispatched;
            item.recovery = std::move(facts);
        }
        // 终态观测缺口:业务执行终态(finished)已在账、observed 终态没落。
        const bool execution_terminal_in_ledger =
            job.dispatched && snap != nullptr && !snap->attempts.empty() &&
            snap->attempts.back().status == "done";
        const bool observed_missing = job.state == "running" && execution_terminal_in_ledger;
        const bool terminal_state = IsTerminalJobState(job.state);
        if (terminal_state) item.terminal_state = job.state;
        if (item.prepared_only) {
            item.disposition = "prepared_hold";
            item.detail = "prepared registration has no adoption or dispatch authority";
        } else if (job.mode != "job_handle") {
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
    if (impl_->prepared_context) return 0;
    impl_->ReapFinishedWorkers();
    std::lock_guard<std::mutex> lock(impl_->jobs_mutex);
    if (impl_->closing) return 0;
    if (plan.policy == JobRecoveryPolicy::Hold) {
        if (impl_->writer == nullptr || impl_->writer->session_id() != plan.source_session_id)
            throw std::invalid_argument("job.recovery.foreign_session");
        for (const auto& item : plan.items) {
            if (item.prepared_only || item.disposition == "prepared_hold") continue;
            if (item.job_id.empty() || item.action_id.empty() || item.turn_id.empty() ||
                item.step_id.empty() || item.tool_name.empty() || !item.recovery.has_value() ||
                item.recovery->turn_id != item.turn_id || item.recovery->step_id != item.step_id ||
                item.recovery->dispatched_count != item.dispatched_count ||
                item.recovery->admission_complete != item.admission_complete)
                throw std::invalid_argument("job.recovery.invalid_source");
            // Check every reserved numeric key before publishing any passive
            // record. The legacy adoption path keeps its existing policy.
            if (item.job_id.rfind("job-", 0) == 0 && item.job_id.size() > 4) {
                const auto number = item.job_id.substr(4);
                if (number.find_first_not_of("0123456789") == std::string::npos) {
                    try {
                        if (std::stoull(number) == std::numeric_limits<std::uint64_t>::max())
                            throw std::invalid_argument("job.recovery.invalid_source");
                    } catch (const std::out_of_range&) {
                        throw std::invalid_argument("job.recovery.invalid_source");
                    }
                }
            }
        }
    }
    std::size_t adopted = 0;
    for (const auto& item : plan.items) {
        if (item.prepared_only || item.disposition == "prepared_hold") continue;
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
        if (plan.policy == JobRecoveryPolicy::Hold || item.owned_layout) {
            if (!item.recovery || !impl_->writer || impl_->writer->session_id() != plan.source_session_id)
                throw std::invalid_argument("job.recovery.invalid_source");
            record->mode = item.mode;
            record->recovery = item.recovery;
            record->state = item.recovery->original_state;
            if (item.recovery->knowledge == JobRecoveryKnowledge::ExecutionUnconfirmed ||
                item.recovery->knowledge == JobRecoveryKnowledge::UnsupportedMode ||
                (item.recovery->knowledge == JobRecoveryKnowledge::TerminalDeliveryGap &&
                 !IsTerminalJobState(record->state))) record->state = "unknown";
            record->dispatched = item.dispatched_count != 0;
            record->admission_complete = item.admission_complete;
            record->admission_text = item.admission_text;
            record->result_ref = item.result_ref;
            record->result_version = item.result_version;
            record->cancel_requested = item.cancel_requested;
            impl_->jobs[item.job_id] = std::move(record);
            ++adopted;
            continue;
        }
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
        // 接单事实同理:到过 attempt 2(job_handle)⇒ attempt 1 的接单链
        // 已落账——补链只补消息,不给在跑/无终态的 attempt 伪造终态
        //(流式提前档的恢复缺口正是这形状:事实在、消息缺、工作在跑)。
        job.admission_facts_complete = job.admission_complete;
        if (IsTerminalJobState(item.terminal_state)) {
            job.state = item.terminal_state;
            job.result_ref = item.result_ref;
            job.result_version = item.result_version;
            job.cancel_requested = item.cancel_requested;
        }
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
            } else if (ok && IsTerminalJobState(item.terminal_state)) {
                // Delivery can lag a graceful cancellation or another terminal
                // observation. Repair the model message, preserve the terminal
                // state and keep this job out of the dispatch queue.
                job.state = item.terminal_state;
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
