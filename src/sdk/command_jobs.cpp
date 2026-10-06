#include "sdk/command_jobs.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "agent/loop.hpp"
#include "approval_mode.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "runtime/session_service.hpp"
#include "runtime/trajectory_turn_bridge.hpp"
#include "sdk/results.hpp"
#include "tools/path_utils.hpp"
#include "tools/run_command.hpp"
#include "tools/schema_check.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/hooks.hpp"

namespace lubancore::detail {
namespace {
namespace rt = lubancode::runtime;
namespace agent = lubancode::agent;
namespace tools = lubancode::tools;
namespace v3 = lubancode::trajectory::v3;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Receipt = agent::OwnedJobAdmissionReceipt;
using Admission = agent::OwnedJobAdmissionState;
Error Failure(std::string code, std::string message = {}) { return {std::move(code), std::move(message)}; }
bool Terminal(const std::string& state) {
    return state == "succeeded" || state == "failed" || state == "cancelled" || state == "unknown";
}
Receipt Refuse(const std::string& error, bool unknown = false) {
    tools::Tool::Result result{error, true}; result.error_code = error;
    result.outcome = unknown ? "unknown_after_start" : "rejected_before_start";
    if (unknown) result.execution_control = tools::ExecutionControl::StopIndeterminate;
    return {unknown ? Admission::Unconfirmed : Admission::Rejected, std::move(result)};
}
std::string Hash(const Json& input) {
    auto canonical = lubancode::trajectory::CanonicalJsonDump(input);
    return canonical ? lubancode::platform::Sha256Hex(*canonical) : std::string();
}
std::string PreviewPrefix(const std::string& text, std::size_t bytes) {
    auto size = std::min(bytes, text.size());
    while (size && size < text.size() && (static_cast<unsigned char>(text[size]) & 0xc0) == 0x80) --size;
    return text.substr(0, size);
}
jobs::v1::Identity Identity(const v3::JobOperationBindingFacts& value) {
    return {value.session_id, value.run_id, value.job_id, value.operation_id,
        value.parent_operation_id, value.turn_id, value.action_id, value.attempt};
}
lubancode::ApprovalMode Mode(ApprovalMode mode) {
    switch (mode) {
    case ApprovalMode::AcceptEdits: return lubancode::ApprovalMode::AcceptEdits;
    case ApprovalMode::DontAsk: return lubancode::ApprovalMode::DontAsk;
    case ApprovalMode::Yolo: return lubancode::ApprovalMode::Yolo;
    default: return lubancode::ApprovalMode::Default;
    }
}
Result<Json> Normalize(const Json& input, const SessionCommandJobPlan& plan) {
    if (!input.is_object() || input.value("execution_mode", Json()) != "session_job" ||
        input.contains("run_in_background") || input.contains("max_runtime_ms"))
        return std::unexpected(Failure("sdk.job.invalid_mode"));
    Json effective = input;
    const auto cwd = effective.find("cwd");
    if (cwd == effective.end() || cwd->is_null() || *cwd == "") effective["cwd"] = plan.cwd();
    else if (!cwd->is_string()) return std::unexpected(Failure("sdk.job.invalid_cwd"));
    else {
        const auto text = cwd->get<std::string>();
        if (text.find('\0') != std::string::npos || !lubancode::platform::IsValidUtf8(text))
            return std::unexpected(Failure("sdk.job.invalid_cwd"));
        auto path = tools::Utf8ToPath(text);
        if (path.is_relative()) path = tools::Utf8ToPath(plan.cwd()) / path;
        path = path.lexically_normal();
        if (path != tools::Utf8ToPath(plan.cwd())) return std::unexpected(Failure("sdk.job.cwd_mismatch"));
        effective["cwd"] = plan.cwd();
    }
    const auto& caps = *plan.options();
    for (const auto& [field, cap] : std::vector<std::pair<const char*, std::uint64_t>>{
        {"job_budget_ms", caps.registration_timeout_ms}, {"timeout_ms", caps.command_timeout_ms}}) {
        const auto found = effective.find(field);
        if (found == effective.end()) continue;
        if (!found->is_number_integer() || (found->is_number_integer() && !found->is_number_unsigned() && found->get<std::int64_t>() <= 0))
            return std::unexpected(Failure("sdk.job.invalid_budget"));
        const auto value = found->get<std::uint64_t>();
        if (!value || value > cap) return std::unexpected(Failure("sdk.job.invalid_budget"));
    }
    return effective;
}

Result<jobs::v1::Preview> ReadPreview(const fs::path& directory, const v3::V3Ledger& ledger,
    const v3::JobOperationBindingFacts& binding, std::shared_ptr<lubancode::trajectory::NamedResultCapability> named_results) {
    auto index = IndexCommandJobResult(ledger, binding);
    if (!index) return std::unexpected(index.error());
    // This local artifact reader needs a session identity, not an outbound
    // permission. Its temporary Snapshot is consumed here; no policy seal is
    // exported, and jobs::Preview may not be sent without ResultProjector.
    auto saved = ReadIndexedToolResult(directory, *index, {binding.session_id, results::v1::Mode::Preview, 1}, {8 * 1024 * 1024}, std::move(named_results));
    if (!saved) return std::unexpected(saved.error());
    jobs::v1::Preview value;
    value.identity = Identity(binding); value.persisted_event_id = index->summary.identity.persisted_event_id;
    value.result_id = index->summary.identity.result_id;
    const auto& data = saved->result();
    const auto artifact_gap = [](results::v1::ArtifactState state) {
        switch (state) {
        case results::v1::ArtifactState::Missing: return "sdk.job.artifact_missing";
        case results::v1::ArtifactState::Corrupt: return "sdk.job.artifact_corrupt";
        case results::v1::ArtifactState::TooLarge: return "sdk.job.artifact_too_large";
        case results::v1::ArtifactState::Unreadable: return "sdk.job.artifact_unreadable";
        default: return "sdk.job.artifact_unavailable";
        }
    };
    if (data.metadata_state != results::v1::ArtifactState::Verified) {
        value.artifact_gap = artifact_gap(data.metadata_state); return value;
    }
    const auto combined = std::find_if(data.channels.begin(), data.channels.end(), [](const auto& channel) {
        return channel.channel == "combined";
    });
    if (combined == data.channels.end() || !combined->artifact_verified || !combined->text) {
        value.artifact_gap = combined == data.channels.end() ? "sdk.job.capture_unavailable" : artifact_gap(combined->state);
        return value;
    }
    value.capture_complete = combined->capture_complete;
    value.text = PreviewPrefix(*combined->text, 4096);
    value.preview_truncated = value.text.size() < combined->text->size();
    return value;
}
} // namespace

struct SessionCommandJobs::Record {
    Json original_input;
    jobs::v1::JobView view;
    Result<jobs::v1::Preview> preview = std::unexpected(Failure("sdk.job.result_unavailable"));
    jobs::v1::ApprovalScope approval;
    std::shared_ptr<std::atomic<bool>> parent_stop;
    std::shared_ptr<const tools::PreparedJobFacts> facts;
    std::optional<v3::JobOperationBindingFacts> binding;
    std::optional<Receipt> admission;
    std::optional<rt::OwnedJobParentCommit> parent_commit;
    std::string admission_gap;
    bool live = false, preview_frozen = false;
    std::atomic<bool> prepared{false}, bound{false};
};

SessionCommandJobs::SessionCommandJobs(std::shared_ptr<SessionCommandJobPlan> plan)
    : plan_(std::move(plan)), operations_(plan_->options()->max_registered_jobs) {}

Result<std::shared_ptr<SessionCommandJobs>> SessionCommandJobs::Build(
    std::shared_ptr<SessionCommandJobPlan> plan, rt::SessionService& service, SessionApprovals& approvals,
    ApprovalMode mode, std::chrono::milliseconds approval_timeout, Publisher publish) {
    if (!plan || !plan->enabled() || !service.runtime() || !service.trajectory() ||
        !service.trajectory()->v3_main_writer()) return std::unexpected(Failure("sdk.job.owner_unavailable"));
    auto module = std::shared_ptr<SessionCommandJobs>(new SessionCommandJobs(std::move(plan)));
    module->service_ = &service; module->approvals_ = &approvals; module->approval_mode_ = mode;
    module->approval_timeout_ = approval_timeout; module->publisher_ = std::move(publish);
    rt::AsyncToolRuntime::Hooks hooks;
    hooks.writer = service.trajectory()->v3_main_writer(); hooks.writer_mutex = service.trajectory()->v3_tool_results_mutex();
    hooks.named_results = service.trajectory()->named_result_capability();
    const auto weak = std::weak_ptr<SessionCommandJobs>(module);
    hooks.owned_selected = [](const auto& call) {
        return call.name == "run_command" && call.input.is_object() && call.input.value("execution_mode", Json()) == "session_job";
    };
    hooks.owned_admission = [weak](auto& bridge, const auto& call, const auto& context) {
        auto owner = weak.lock(); return owner ? owner->Admit(bridge, call, context) : agent::MissingOwnedJobAdmission();
    };
    hooks.owned_pump = [weak] { if (auto owner = weak.lock()) owner->PumpAndPublish(); };
    rt::AsyncToolRuntimeOptions options;
    options.admission_mode = agent::JobAdmissionMode::OwnedRequired;
    options.recovery_policy = tools::JobRecoveryPolicy::Hold;
    options.coordinator.prepared_registration = tools::PreparedRegistrationContext{
        hooks.writer_mutex, service.trajectory()->workspace_key(), tools::Utf8ToPath(module->plan_->cwd())};
    options.coordinator.limits.session_running = module->plan_->options()->max_running;
    options.coordinator.limits.per_tool = module->plan_->options()->max_running;
    options.coordinator.limits.queued_max = module->plan_->options()->max_registered_jobs;
    auto runtime = rt::AsyncToolRuntime::Create(std::move(hooks), std::move(options));
    if (!runtime) return std::unexpected(Failure("sdk.job.runtime_unavailable"));
    module->coordinator_ = runtime->coordinator();
    auto owner = module->coordinator_->PreparedOwner();
    if (!owner) return std::unexpected(Failure("sdk.job.owner_unavailable"));
    module->owner_ = *owner;
    auto restored = module->Restore(); if (!restored) return std::unexpected(restored.error());
    service.runtime()->AttachAsyncToolRuntime(std::move(runtime));
    return module;
}

std::unique_ptr<rt::assembly::SessionResourceAttachment> SessionCommandJobs::Attachment() {
    struct Anchor final : rt::assembly::SessionResourceAttachment {
        std::shared_ptr<SessionCommandJobs> owner;
        explicit Anchor(std::shared_ptr<SessionCommandJobs> value) : owner(std::move(value)) {}
    };
    return std::make_unique<Anchor>(shared_from_this());
}

void SessionCommandJobs::SetParent(std::string operation_id, std::string turn_id) {
    std::lock_guard lock(mutex_);
    active_parent_ = std::move(operation_id); active_turn_ = std::move(turn_id);
    parent_stops_.try_emplace(active_parent_, std::make_shared<std::atomic<bool>>(closing_));
}

void SessionCommandJobs::Freeze(const std::string& why) {
    indeterminate_.store(true, std::memory_order_release);
    { std::lock_guard lock(mutex_); if (!first_error_) first_error_ = Failure(why); }
    if (service_) service_->trajectory()->BlockV3Execution(why);
    RequestClose();
}

Receipt SessionCommandJobs::Admit(rt::TrajectoryTurnBridge& bridge,
    const lubancode::api::ToolUseBlock& call, const agent::OwnedToolAdmissionContext& context) {
    const auto origin = bridge.V3DeclaredCallOrigin(call.id);
    if (!origin || origin->turn_id.empty() || bridge.v3_writer() != service_->trajectory()->v3_main_writer() ||
        context.wiring.subordinate_stream || call.name != "run_command") return Refuse("sdk.job.main_declaration_required");
    auto record = std::make_shared<Record>();
    record->original_input = call.input;
    const auto key = origin->turn_id + ":" + origin->action_id;
    {
        std::lock_guard lock(mutex_);
        const auto known = by_call_.find(key);
        if (known != by_call_.end()) return known->second->original_input == call.input
            ? known->second->admission.value_or(Refuse("sdk.job.admission_incomplete", true))
            : Refuse("sdk.job.declaration_changed", true);
        if (closing_ || indeterminate_ || active_turn_ != origin->turn_id || active_parent_.empty()) return Refuse("sdk.job.closed");
        if (records_.size() >= plan_->options()->max_registered_jobs) return Refuse("sdk.job.registration_limit");
        record->parent_stop = parent_stops_.at(active_parent_);
        record->approval = {owner_.session_id, owner_.run_id, active_parent_, origin->turn_id,
            origin->action_id, call.id, origin->message_id, plan_->cwd(), {}};
        by_call_.emplace(key, record);
    }
    const auto done = [&](Receipt value) {
        std::lock_guard lock(mutex_);
        record->admission = value;
        if (value.state == Admission::Unconfirmed && record->admission_gap.empty()) {
            record->admission_gap = value.result.error_code;
            record->view.gap = record->admission_gap;
            if (!record->live) { record->view.state = "unknown"; record->view.terminal = true; }
        }
        if (!record->facts && value.state == Admission::Rejected) by_call_.erase(key);
        return value;
    };
    const auto stopped = [&] { return record->parent_stop->load(std::memory_order_acquire) ||
        (context.cancel && context.cancel->load(std::memory_order_acquire)); };
    if (stopped()) return done(Refuse("sdk.job.parent_stopped"));
    auto normalized = Normalize(call.input, *plan_);
    if (!normalized) return done(Refuse(normalized.error().code));
    const auto* registration = context.registry.RegistrationOf(call.name);
    const auto* tool = context.registry.Find(call.name);
    if (!registration || !tool || registration->source_kind != tools::ToolSourceKind::Builtin ||
        registration->source_instance.empty() || registration->version_or_digest.empty())
        return done(Refuse("sdk.job.tool_identity_unavailable"));
    if (const auto error = tools::ValidateInputAgainstSchema(*normalized, tool->input_schema()))
        return done(Refuse("sdk.job.invalid_input:" + *error));
    auto wiring = context.wiring; // Only this synchronous host stack owns the copy.
    wiring.on_permission_evaluate = [&](const auto&, const auto&, auto kind, const Json& arguments, const auto& pre) {
        record->approval.effective_input_sha256 = Hash(arguments);
        rt::PermissionContext policy; policy.mode = Mode(approval_mode_);
        return rt::EvaluatePermission(policy, pre, kind, call.name, arguments); // no parent AllowedTools
    };
    wiring.on_tool_confirm_async = {};
    wiring.on_tool_confirm_scoped = [&](const rt::ApprovalRequest& request) {
        Approval approval;
        approval.request_id = owner_.session_id + ":" + origin->turn_id + ":" + origin->action_id + ":job";
        approval.operation_id = record->approval.parent_operation_id; approval.tool_call_id = call.id;
        approval.tool_name = call.name; approval.input_json = request.input.dump(); approval.cwd = plan_->cwd();
        approval.reason = request.reason; approval.job = record->approval;
        auto lease = approvals_->RegisterJobScoped(record->approval, std::move(approval), approval_timeout_,
            [this](const Approval& ticket) {
                if (publisher_) { Event event; event.kind = "approval_requested"; event.operation_id = ticket.operation_id;
                    event.turn_id = ticket.job->turn_id; event.approval = ticket; publisher_(std::move(event)); }
            });
        return lease ? std::move(*lease) : rt::ApprovalLease{};
    };
    auto effective_call = call; effective_call.input = *normalized;
    auto prepared = agent::PrepareOwnedToolInput(context.registry, effective_call, wiring,
        context.tool_filter, context.filter_denial, context.trace, context.cancel, nullptr,
        context.turn_gate, context.turn_gate_denial);
    if (!prepared) {
        approvals_->CloseJobScope(record->approval);
        return done({prepared.error().execution_control == tools::ExecutionControl::StopIndeterminate
            ? Admission::Unconfirmed : Admission::Rejected, prepared.error(), true});
    }
    auto checked = Normalize(prepared->effective_input, *plan_);
    if (!checked || *checked != prepared->effective_input || prepared->source_instance != registration->source_instance ||
        prepared->source_kind != registration->source_kind || Hash(prepared->effective_input) != record->approval.effective_input_sha256 || stopped()) {
        approvals_->CloseJobScope(record->approval);
        return done(Refuse("sdk.job.effective_scope_changed"));
    }
    record->prepared.store(true, std::memory_order_release);
    tools::PreparedJobRequest request;
    request.owner = owner_; request.provider_tool_call_id = call.id; request.assistant_message_ref = origin->message_id;
    request.parent_action_id = origin->action_id; request.turn_id = origin->turn_id; request.step_id = origin->step_id;
    request.tool_name = call.name; request.original_input = call.input; request.effective_input = prepared->effective_input;
    request.tool_identity = {call.name, registration->source_instance, registration->version_or_digest, plan_->cwd()};
    request.policy.allow_background = true; request.policy.side_effect_class = "external";
    request.policy.deadline_ms = request.effective_input.value("job_budget_ms", plan_->options()->registration_timeout_ms);
    request.policy.max_output_bytes = plan_->options()->max_output_bytes;
    // One short host transaction: no model, approval or external observer runs
    // between Register/Adopt/Bind/the original bridge/Confirm.
    std::lock_guard serial(*bridge.v3_shared_mutex());
    const auto registration_result = coordinator_->RegisterPreparedJob(request);
    if (registration_result.facts && registration_result.state != tools::PreparedJobRegistrationState::Rejected) {
        record->facts = registration_result.facts;
        std::lock_guard lock(mutex_);
        record->view.identity = {owner_.session_id, owner_.run_id, record->facts->job_id, {},
            record->approval.parent_operation_id, record->facts->turn_id, record->facts->action_id, record->facts->attempt};
        record->view.state = "registered";
        records_.emplace(record->facts->job_id, record);
    }
    if (registration_result.state != tools::PreparedJobRegistrationState::Registered || !record->facts) {
        const bool unknown = registration_result.state == tools::PreparedJobRegistrationState::Unconfirmed;
        if (unknown) Freeze(registration_result.error_code);
        return done(Refuse(registration_result.error_code, unknown));
    }
    tools::OwnedJobCapability capability;
    capability.command = std::make_shared<tools::RunCommandTool>();
    capability.command_limits = {std::min(plan_->options()->command_timeout_ms, request.policy.deadline_ms),
        plan_->options()->max_output_bytes};
    const auto weak = weak_from_this();
    const auto named_results = service_->trajectory()->named_result_capability();
    capability.scope_gate = [weak, record, facts = record->facts, named_results](const auto& scope, const Json& input,
        const auto& identity, const auto& policy) {
        auto module = weak.lock();
        const bool matching = module && record->prepared.load(std::memory_order_acquire) &&
            !record->parent_stop->load(std::memory_order_acquire) && !module->indeterminate() &&
            scope.owner == facts->owner && scope.job_id == facts->job_id && scope.action_id == facts->action_id &&
            scope.turn_id == facts->turn_id && scope.step_id == facts->step_id && scope.attempt == facts->attempt &&
            scope.parent_action_id == facts->parent_action_id && scope.provider_tool_call_id == facts->provider_tool_call_id &&
            input == facts->effective_input && identity.ToJson() == facts->tool_identity.ToJson() && policy.ToJson() == facts->policy.ToJson();
        if (!matching) return tools::JobAuthDecision{false, false, "sdk.job.scope_revoked"};
        // Settlement can seal this shared owner earlier in the same pump, before
        // the module sees its terminal view. Do not start the next queued command.
        if (named_results && named_results->HasUnconfirmedPublication())
            return tools::JobAuthDecision{false, false, "sdk.named_results.publication_unconfirmed"};
        if (record->bound.load(std::memory_order_acquire) &&
            !module->approvals_->JobAllowed(record->approval, Identity(*record->binding)))
            return tools::JobAuthDecision{false, false, "sdk.job.approval_scope_closed"};
        return tools::JobAuthDecision{true, false, {}};
    };
    capability.live_post = [weak](const auto& completion, const auto& invocation) {
        if (auto module = weak.lock()) return module->Post(completion, invocation);
        v3::WriteReceipt failed; failed.error_code = "sdk.job.post_owner_unavailable"; return failed;
    };
    const auto adopted = coordinator_->AdoptPreparedJob(owner_, record->facts->job_id, std::move(capability));
    if (adopted.state != tools::OwnedJobAdoptionState::Adopted) {
        if (adopted.state == tools::OwnedJobAdoptionState::Unconfirmed) Freeze(adopted.error_code);
        return done(Refuse(adopted.error_code, adopted.state == tools::OwnedJobAdoptionState::Unconfirmed));
    }
    { std::lock_guard lock(mutex_); record->live = true; record->view.owner_available = true; }
    const auto bound = operations_.Bind(*service_, *coordinator_, record->facts->job_id);
    if (!bound.bound() || !bound.facts) {
        coordinator_->RequestOwnedCancellation(owner_, record->facts->job_id, "binding_unconfirmed");
        Freeze(bound.error.code.empty() ? "sdk.job.binding_unconfirmed" : bound.error.code);
        return done(Refuse("sdk.job.binding_unconfirmed", true));
    }
    record->binding = *bound.facts;
    { std::lock_guard lock(mutex_); record->view.identity = Identity(*bound.facts); }
    if (!approvals_->BindJobScope(record->approval, Identity(*bound.facts)) || stopped()) {
        coordinator_->RequestOwnedCancellation(owner_, record->facts->job_id, "parent_stopped");
        return done(Refuse("sdk.job.parent_stopped"));
    }
    record->bound.store(true, std::memory_order_release);
    record->parent_commit = bridge.CommitOwnedJobAdmission(call.id, adopted, *bound.facts);
    if (!record->parent_commit->committed) {
        coordinator_->RequestOwnedCancellation(owner_, record->facts->job_id, "parent_admission_unconfirmed");
        Freeze(record->parent_commit->error);
        return done(Refuse(record->parent_commit->error, true));
    }
    if (stopped()) coordinator_->RequestOwnedCancellation(owner_, record->facts->job_id, "parent_stopped");
    const auto confirmed = coordinator_->ConfirmParentAdmission(owner_, record->facts->job_id, record->parent_commit->refs, true);
    if (!confirmed.confirmed) {
        coordinator_->RequestOwnedCancellation(owner_, record->facts->job_id, "confirmation_unconfirmed");
        Freeze(confirmed.error_code);
        return done(Refuse(confirmed.error_code, true));
    }
    if (stopped()) coordinator_->RequestOwnedCancellation(owner_, record->facts->job_id, "parent_stopped");
    pending_.store(true, std::memory_order_release);
    tools::Tool::Result accepted{adopted.admission_content, false};
    return done({Admission::Accepted, std::move(accepted)});
}


v3::WriteReceipt SessionCommandJobs::Post(const tools::OwnedJobCompletion& completion,
    const tools::OwnedJobPostInvocation& invocation) {
    const auto invalid = [](std::string error) { v3::WriteReceipt out; out.error_code = std::move(error); return out; };
    auto actual = coordinator_->CheckOwnedPostInvocation(invocation);
    if (!actual || !actual->facts || !service_) return invalid("sdk.job.post_invocation_invalid");
    const auto& scope = completion.scope;
    if (actual->completion.scope.owner != scope.owner || actual->completion.scope.job_id != scope.job_id ||
        actual->completion.scope.action_id != scope.action_id || actual->completion.scope.attempt != scope.attempt ||
        actual->completion.scope.turn_id != scope.turn_id || actual->completion.scope.step_id != scope.step_id ||
        actual->completion.scope.parent_action_id != scope.parent_action_id ||
        actual->completion.scope.provider_tool_call_id != scope.provider_tool_call_id)
        return invalid("sdk.job.post_scope_mismatch");
    auto* writer = service_->trajectory()->v3_main_writer();
    const v3::HookHandlerSpec handler{"sdk.command_job.verify.v1",
        lubancode::platform::Sha256Hex("sdk.command_job.verify.v1"), "builtin", 0, "block"};
    const auto id = writer->NewHookDispatchId();
    auto dispatch = v3::HookDispatchSession::Dispatch(*writer, id, "PostAction", scope.turn_id, scope.step_id,
        scope.action_id, {handler}, Json{{"executionEventRef", completion.terminal_receipt.id},
            {"resultEventRef", completion.persisted_receipt.id}});
    auto started = dispatch.BeginInvocation(*writer, "invocation-" + id, handler);
    if (started.status != v3::WriteReceipt::Status::Committed) return started;
    const auto fail = [&](const char* error) { return dispatch.FailInvocation(*writer, error, 0); };
    const auto bound = operations_.Snapshot(scope.job_id);
    if (!bound || !bound->bound() || !bound->facts || !bound->receipt)
        return fail("sdk.job.post_binding_unavailable");
    const auto& binding = *bound->facts;
    if (binding.session_id != scope.owner.session_id || binding.run_id != scope.owner.run_id ||
        binding.action_id != scope.action_id || binding.turn_id != scope.turn_id || binding.step_id != scope.step_id ||
        binding.job_id != scope.job_id || binding.attempt != scope.attempt ||
        binding.adopted_event_id != actual->adopted_receipt.id)
        return fail("sdk.job.post_binding_mismatch");
    const auto ledger = v3::ReadV3Ledger(writer->path());
    if (!ledger) return fail("sdk.job.post_invalid_ledger");
    const auto bindings = v3::ReadJobOperationBindings(*ledger);
    if (!bindings || std::none_of(bindings->begin(), bindings->end(), [&](const auto& saved) {
        return saved.event_id == binding.event_id && saved.seq == binding.seq && saved.line_hash == binding.line_hash &&
            saved.operation_id == binding.operation_id && saved.parent_operation_id == binding.parent_operation_id &&
            saved.action_id == scope.action_id && saved.attempt == scope.attempt && saved.job_id == scope.job_id;
    })) return fail("sdk.job.post_binding_invalid");
    const auto matches = [&](const v3::WriteReceipt& receipt, const v3::WriteReceipt& captured) {
        const auto* event = ledger->FindEvent(receipt.id);
        return event && receipt.status == v3::WriteReceipt::Status::Committed && captured.id == receipt.id &&
            captured.seq == receipt.seq && captured.line_hash == receipt.line_hash && event->seq == receipt.seq &&
            event->line_hash == receipt.line_hash && event->session_id == binding.session_id &&
            event->run_id == binding.run_id && event->turn_id == binding.turn_id && event->action_id == binding.action_id;
    };
    if (!matches(completion.started_receipt, actual->completion.started_receipt) ||
        !matches(completion.terminal_receipt, actual->completion.terminal_receipt) ||
        !matches(completion.persisted_receipt, actual->completion.persisted_receipt) ||
        !(binding.seq < completion.started_receipt.seq && completion.started_receipt.seq < completion.terminal_receipt.seq &&
          completion.terminal_receipt.seq < completion.persisted_receipt.seq && completion.persisted_receipt.seq < started.seq))
        return fail("sdk.job.post_completion_mismatch");
    return dispatch.CompleteInvocation(*writer, "allow", std::nullopt, 0);
}

Result<void> SessionCommandJobs::Restore() {
    const auto ledger = v3::ReadV3Ledger(service_->trajectory()->v3_main_writer()->path());
    if (!ledger) return std::unexpected(Failure("sdk.job.recovery_invalid", ledger.error()));
    const auto bindings = ReadJobOperations(service_->trajectory()->session_dir(), *ledger);
    if (bindings.state == JobOperationRecoveryState::Rejected) return std::unexpected(bindings.error);
    const auto adoptions = v3::ReadOwnedJobAdoptions(*ledger);
    if (!adoptions) return std::unexpected(Failure("sdk.job.recovery_invalid", adoptions.error()));
    const auto plan = tools::ToolJobCoordinator::PlanRecovery(*ledger, tools::JobRecoveryPolicy::Hold);
    for (const auto& item : plan.items) {
        if (!item.prepared_only && !item.owned_layout) continue;
        if (records_.size() >= plan_->options()->max_registered_jobs)
            return std::unexpected(Failure("sdk.job.registration_limit", "historical registrations exceed the frozen budget"));
        const auto registration = v3::ReadOwnedJobRegistration(*ledger, item.job_id);
        if (!registration || registration->job_id != item.job_id || registration->action_id != item.action_id ||
            registration->turn_id != item.turn_id || registration->step_id != item.step_id ||
            registration->attempt != item.attempt || registration->tool_identity.value("logicalName", Json()) != "run_command")
            return std::unexpected(Failure("sdk.job.recovery_invalid", "historical Job registration scope is missing or conflicting"));
        const auto* adopted = v3::FindOwnedJobAdoption(*adoptions, item.job_id);
        if (item.owned_layout && (!adopted || adopted->session_id != registration->session_id ||
            adopted->run_id != registration->run_id || adopted->action_id != registration->action_id ||
            adopted->attempt != registration->attempt || adopted->registered_event_id != registration->registered_event_id))
            return std::unexpected(Failure("sdk.job.recovery_invalid", "historical adoption differs from its registration"));
        auto record = std::make_shared<Record>();
        record->view.identity = {registration->session_id, registration->run_id, item.job_id, {}, {},
            registration->turn_id, registration->action_id, registration->attempt};
        const auto binding = std::find_if(bindings.facts.begin(), bindings.facts.end(), [&](const auto& value) {
            return value.job_id == item.job_id;
        });
        if (binding != bindings.facts.end()) { record->binding = *binding; record->view.identity = Identity(*binding); }
        record->view.state = item.recovery ? item.recovery->original_state : item.terminal_state;
        if (record->view.state.empty()) record->view.state = "unknown";
        record->view.execution_state = item.recovery ? item.recovery->execution_state : std::string();
        record->view.cancel_requested = item.cancel_requested;
        record->view.command_not_invoked = item.recovery && item.recovery->command_not_invoked;
        record->view.owner_available = false;
        record->view.recovery_knowledge = item.disposition;
        record->view.gap = item.detail;
        if (!record->binding && record->view.gap.empty()) record->view.gap = "sdk.job.binding_unavailable";
        record->view.terminal = Terminal(record->view.state);
        if (record->binding && record->view.terminal)
            record->preview = ReadPreview(service_->trajectory()->session_dir(), *ledger, *record->binding, service_->trajectory()->named_result_capability());
        record->preview_frozen = record->view.terminal;
        if (!records_.emplace(item.job_id, std::move(record)).second)
            return std::unexpected(Failure("sdk.job.recovery_invalid", "duplicate historical Job identity"));
    }
    // Never AdoptRecovery: an old clock, process or ownership lease cannot be
    // reconstructed from these values. New Jobs use only this live coordinator.
    return {};
}

void SessionCommandJobs::PumpAndPublish() {
    std::shared_ptr<tools::ToolJobCoordinator> coordinator;
    std::vector<std::shared_ptr<Record>> records;
    {
        std::lock_guard lock(mutex_);
        if (finalized_ || !service_) return;
        coordinator = coordinator_;
        for (const auto& [id, record] : records_) { (void)id; if (record->live) records.push_back(record); }
    }
    if (!coordinator) return;
    coordinator->PumpOwnedJobs(); // Only this host worker (or joined Close) drives writes.
    std::vector<Event> notifications;
    bool pending = false;
    std::string live_gap;
    {
        std::lock_guard serial(*service_->trajectory()->v3_tool_results_mutex());
        std::optional<v3::V3Ledger> ledger;
        for (const auto& record : records) {
            const auto status = coordinator->SnapshotOwnedJob(owner_, record->facts->job_id);
            jobs::v1::JobView value;
            Result<jobs::v1::Preview> preview = std::unexpected(Failure("sdk.job.result_unavailable"));
            bool need_preview;
            {
                std::lock_guard lock(mutex_);
                value = record->view; need_preview = !record->preview_frozen;
            }
            value.state = status.state.empty() ? "unknown" : status.state;
            value.execution_state = status.execution_state; value.cancel_requested = status.cancel_requested;
            value.gap = !record->admission_gap.empty() ? record->admission_gap :
                status.access_denied ? "sdk.job.owner_mismatch" : status.gap;
            value.command_not_invoked = status.command_not_invoked;
            value.terminal = Terminal(value.state); value.owner_available = !coordinator->shutdown_complete();
            if (value.state == "unknown" && live_gap.empty())
                live_gap = value.gap.empty() ? "sdk.job.execution_unknown" : value.gap;
            pending |= !value.terminal;
            if (value.terminal && need_preview && record->binding) {
                if (!ledger) {
                    auto read = v3::ReadV3Ledger(service_->trajectory()->v3_main_writer()->path());
                    if (read) ledger = std::move(*read);
                }
                preview = ledger ? ReadPreview(service_->trajectory()->session_dir(), *ledger, *record->binding, service_->trajectory()->named_result_capability())
                    : Result<jobs::v1::Preview>(std::unexpected(Failure("sdk.job.result_invalid", "Job journal is unavailable")));
            }
            {
                std::lock_guard lock(mutex_);
                const bool changed = value.state != record->view.state || value.execution_state != record->view.execution_state ||
                    value.cancel_requested != record->view.cancel_requested || value.gap != record->view.gap ||
                    value.owner_available != record->view.owner_available;
                record->view = value;
                if (value.terminal && need_preview) { record->preview = std::move(preview); record->preview_frozen = true; }
                if (changed) {
                    Event event; event.kind = "job_updated"; event.operation_id = value.identity.operation_id;
                    event.turn_id = value.identity.turn_id;
                    event.payload_json = Json{{"jobId", value.identity.job_id}, {"operationId", value.identity.operation_id},
                        {"parentOperationId", value.identity.parent_operation_id}, {"state", value.state},
                        {"executionState", value.execution_state}, {"cancelRequested", value.cancel_requested},
                        {"ownerAvailable", value.owner_available}, {"gap", value.gap}}.dump();
                    notifications.push_back(std::move(event));
                }
            }
            if (value.terminal && approvals_) approvals_->CloseJobScope(record->approval);
        }
        pending |= coordinator->running_count() != 0;
    }
    pending_.store(pending, std::memory_order_release);
    cv_.notify_all();
    if (!live_gap.empty() && !indeterminate()) Freeze(live_gap);
    // The writer serial and cache mutex have both been released. Delivery may
    // overflow or fail without changing a Job's native execution facts.
    if (publisher_) for (auto& event : notifications) publisher_(std::move(event));
}

void SessionCommandJobs::StopParent(const std::string& operation_id, const std::string& reason) {
    std::vector<std::string> ids;
    std::shared_ptr<tools::ToolJobCoordinator> coordinator;
    {
        std::lock_guard lock(mutex_);
        const auto [it, inserted] = parent_stops_.try_emplace(operation_id, std::make_shared<std::atomic<bool>>(true));
        (void)inserted; it->second->store(true, std::memory_order_release);
        coordinator = coordinator_;
        for (const auto& [id, record] : records_)
            if (record->live && record->view.identity.parent_operation_id == operation_id) ids.push_back(id);
    }
    if (coordinator) for (const auto& id : ids) coordinator->RequestOwnedCancellation(owner_, id, reason);
    cv_.notify_all();
}

void SessionCommandJobs::RequestClose() {
    std::vector<std::string> ids;
    std::shared_ptr<tools::ToolJobCoordinator> coordinator;
    {
        std::lock_guard lock(mutex_);
        closing_ = true; coordinator = coordinator_;
        for (const auto& [id, stopped] : parent_stops_) { (void)id; stopped->store(true, std::memory_order_release); }
        for (const auto& [id, record] : records_) if (record->live) ids.push_back(id);
    }
    if (coordinator) for (const auto& id : ids) coordinator->RequestOwnedCancellation(owner_, id, "session_close");
    cv_.notify_all();
}

void SessionCommandJobs::EndParent(const std::string& operation_id) {
    std::lock_guard lock(mutex_);
    parent_stops_.erase(operation_id); // each actual Job retains its own parent latch
    if (active_parent_ == operation_id) { active_parent_.clear(); active_turn_.clear(); }
}

Result<void> SessionCommandJobs::RetireBindings() {
    RequestClose();
    const auto closed = operations_.Close();
    if (!closed) { std::lock_guard lock(mutex_); if (!first_error_) first_error_ = closed.error(); }
    return closed;
}

Result<void> SessionCommandJobs::Finalize() {
    {
        std::lock_guard lock(mutex_);
        if (finalized_) return first_error_ ? Result<void>(std::unexpected(*first_error_)) : Result<void>{};
    }
    RequestClose();
    auto retired = RetireBindings();
    const bool settled = service_ && service_->runtime() && service_->runtime()->ShutdownAsyncTools();
    PumpAndPublish();
    Publisher retired_publisher;
    std::shared_ptr<tools::ToolJobCoordinator> retired_coordinator;
    {
        std::lock_guard lock(mutex_);
        for (auto& [id, record] : records_) { (void)id; record->view.owner_available = false; }
        if (!settled && !first_error_) first_error_ = Failure("sdk.job.close_unconfirmed");
        finalized_ = true; pending_.store(false, std::memory_order_release);
        retired_publisher.swap(publisher_); retired_coordinator.swap(coordinator_);
        service_ = nullptr; approvals_ = nullptr;
        cv_.notify_all();
    }
    // Capability/publisher destructors run without the query or writer mutex.
    std::lock_guard lock(mutex_);
    return first_error_ ? Result<void>(std::unexpected(*first_error_)) : retired;
}

Result<std::vector<jobs::v1::JobView>> SessionCommandJobs::List(std::optional<std::string> parent) const {
    std::lock_guard lock(mutex_);
    std::vector<jobs::v1::JobView> result;
    for (const auto& [id, record] : records_) {
        (void)id;
        if (!parent || record->view.identity.parent_operation_id == *parent) result.push_back(record->view);
    }
    return result;
}
Result<jobs::v1::JobView> SessionCommandJobs::Read(const jobs::v1::Identity& identity) const {
    std::lock_guard lock(mutex_);
    const auto found = records_.find(identity.job_id);
    if (found == records_.end() || found->second->view.identity != identity)
        return std::unexpected(Failure("sdk.job.not_found"));
    return found->second->view;
}
Result<jobs::v1::JobView> SessionCommandJobs::Wait(const jobs::v1::Identity& identity, std::chrono::milliseconds timeout) const {
    if (timeout.count() < 0) return std::unexpected(Failure("sdk.timeout.invalid"));
    std::unique_lock lock(mutex_);
    const auto found = records_.find(identity.job_id);
    if (found == records_.end() || found->second->view.identity != identity)
        return std::unexpected(Failure("sdk.job.not_found"));
    auto record = found->second;
    if (record->view.terminal) return record->view;
    if (!record->live || finalized_) return std::unexpected(Failure("sdk.job.owner_unavailable"));
    if (!cv_.wait_for(lock, timeout, [&] { return record->view.terminal || closing_ || finalized_; }))
        return std::unexpected(Failure("sdk.wait.timeout"));
    if (!record->view.terminal) return std::unexpected(Failure("sdk.job.owner_unavailable"));
    return record->view;
}
Result<void> SessionCommandJobs::Cancel(const jobs::v1::Identity& identity) {
    std::shared_ptr<tools::ToolJobCoordinator> coordinator;
    {
        std::lock_guard lock(mutex_);
        const auto found = records_.find(identity.job_id);
        if (found == records_.end() || found->second->view.identity != identity)
            return std::unexpected(Failure("sdk.job.not_found"));
        if (!found->second->live || finalized_) return std::unexpected(Failure("sdk.job.owner_unavailable"));
        coordinator = coordinator_;
    }
    const auto cancelled = coordinator->RequestOwnedCancellation(owner_, identity.job_id, "sdk_cancel_job");
    if (!cancelled.ok) return std::unexpected(Failure("sdk.job.cancel_failed", cancelled.error));
    if (cancelled.status == "cancel_requested") {
        std::lock_guard lock(mutex_);
        const auto found = records_.find(identity.job_id);
        if (found != records_.end()) found->second->view.cancel_requested = true;
        cv_.notify_all();
    }
    return {};
}
Result<jobs::v1::Preview> SessionCommandJobs::Preview(const jobs::v1::Identity& identity, std::size_t max_bytes) const {
    if (!max_bytes || max_bytes > 4096) return std::unexpected(Failure("sdk.job.preview_limit_invalid"));
    std::lock_guard lock(mutex_);
    const auto found = records_.find(identity.job_id);
    if (found == records_.end() || found->second->view.identity != identity)
        return std::unexpected(Failure("sdk.job.not_found"));
    if (!found->second->preview) return std::unexpected(found->second->preview.error());
    auto value = *found->second->preview;
    auto text = PreviewPrefix(value.text, max_bytes);
    value.preview_truncated |= text.size() < value.text.size(); value.text = std::move(text);
    return value;
}

} // namespace lubancore::detail
