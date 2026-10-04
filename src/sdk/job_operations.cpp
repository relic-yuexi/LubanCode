#include "sdk/job_operations.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "platform/paths.hpp"
#include "runtime/session_service.hpp"
#include "runtime/trajectory_session.hpp"
#include "sdk/operation_ledger.hpp"
#include "tools/tool_job_coordinator.hpp"

namespace lubancore::detail {
namespace {
namespace v3 = lubancode::trajectory::v3;
thread_local const JobOperations* current_binding = nullptr;
Error Failure(const std::string& code, const std::string& message = {}) { return {code, message}; }
bool SamePrefix(const v3::V3Ledger& ledger, const v3::V3Writer& writer) {
    const auto last = ledger.LastEntry();
    if (!last || last->seq == std::numeric_limits<std::uint64_t>::max() || last->seq + 1 != writer.next_seq()) return false;
    return writer.last_line_hash() == (last->is_message ? ledger.messages[last->index].line_hash
                                                      : ledger.events[last->index].line_hash);
}
bool HasParentSource(OperationTurnMaterialState state) {
    return state == OperationTurnMaterialState::Validated || state == OperationTurnMaterialState::Incomplete;
}
struct BindingScope {
    const JobOperations* previous = current_binding;
    explicit BindingScope(const JobOperations* owner) { current_binding = owner; }
    ~BindingScope() { current_binding = previous; }
};
}
struct JobOperations::Record { JobOperationRegistration value; };
JobOperations::JobOperations(std::size_t capacity) : capacity_(capacity) {
    if (!capacity) throw std::invalid_argument("sdk.job_operation.invalid_capacity");
}
JobOperations::~JobOperations() { (void)Close(); }

JobOperationRegistration JobOperations::Bind(lubancode::runtime::SessionService& service,
    lubancode::tools::ToolJobCoordinator& coordinator, const std::string& job_id,
    const std::function<void(const v3::JobOperationBindingFacts&)>& publish) {
    JobOperationRegistration out;
    out.error = Failure("sdk.job_operation.rejected");
    if (current_binding) { out.error = Failure("sdk.job_operation.reentrant"); return out; }
    {
        std::lock_guard lock(mutex_);
        if (closing_) { out.error = Failure("sdk.job_operation.closed"); return out; }
        ++active_;
    }
    struct Retire {
        JobOperations& owner;
        ~Retire() { std::lock_guard lock(owner.mutex_); --owner.active_; owner.cv_.notify_all(); }
    } retire{*this};
    BindingScope scope(this);
    std::shared_ptr<Record> record;
    std::shared_ptr<v3::JobOperationBindingFacts> facts;
    try {
        auto* trajectory = service.trajectory();
        auto* writer = trajectory ? trajectory->v3_main_writer() : nullptr;
        if (!writer) { out.error = Failure("sdk.job_operation.writer_unavailable"); return out; }
        const auto consumed = coordinator.WithOwnedJobBindingSource(*writer, job_id, [&](const auto& source) {
            auto ledger = v3::ReadV3Ledger(writer->path());
            if (!ledger || !SamePrefix(*ledger, *writer)) {
                out.error = Failure("sdk.job_operation.invalid_prefix"); return;
            }
            const auto parent = CheckMainOperationTurnBindings(trajectory->session_dir(), *ledger);
            const auto adoptions = v3::ReadOwnedJobAdoptions(*ledger);
            const auto* job = adoptions ? v3::FindOwnedJobAdoption(*adoptions, job_id) : nullptr;
            const auto* actual = ledger->FindEvent(source.adopted_receipt.id);
            if (!HasParentSource(parent.state) || !job || !actual ||
                actual->seq != source.adopted_receipt.seq || actual->line_hash != source.adopted_receipt.line_hash ||
                job->adopted_event_id != actual->event_id || job->action_id != source.facts->action_id ||
                job->session_id != source.facts->owner.session_id || job->run_id != source.facts->owner.run_id ||
                job->original_input != source.facts->original_input || job->effective_input != source.facts->effective_input) {
                out.error = Failure("sdk.job_operation.invalid_source"); return;
            }
            const auto anchor = std::find_if(parent.facts.begin(), parent.facts.end(), [&](const auto& item) {
                return item.turn_id == job->turn_id && item.session_id == job->session_id && item.run_id == job->run_id;
            });
            const auto* assistant = ledger->FindMessage(job->assistant_message_ref);
            if (anchor == parent.facts.end() || !assistant || anchor->seq >= assistant->seq) {
                out.error = Failure("sdk.job_operation.parent_unbound"); return;
            }
            const auto existing = v3::ReadJobOperationBindings(*ledger);
            if (!existing || std::any_of(existing->begin(), existing->end(), [&](const auto& item) {
                    return item.job_id == job_id && item.run_id == job->run_id;
                })) { out.error = Failure("sdk.job_operation.already_bound"); return; }
            std::error_code ec;
            const auto directory = std::filesystem::canonical(trajectory->session_dir(), ec);
            if (ec) { out.error = Failure("sdk.job_operation.invalid_directory"); return; }
            const auto host = lubancode::platform::PathToUtf8(directory) + "\n" +
                std::to_string(source.facts->owner.coordinator_id) + "\n" + std::to_string(source.facts->owner.epoch);
            facts = std::make_shared<v3::JobOperationBindingFacts>();
            facts->session_id = job->session_id; facts->run_id = job->run_id; facts->turn_id = job->turn_id;
            facts->step_id = job->step_id; facts->action_id = job->action_id; facts->job_id = job_id;
            facts->parent_operation_id = anchor->operation_id; facts->parent_input_id = anchor->input_id;
            facts->parent_payload_hash = anchor->payload_hash; facts->parent_operation_event_id = anchor->event_id;
            facts->adopted_event_id = job->adopted_event_id;
            facts->original_input_sha256 = job->original_input_sha256; facts->effective_input_sha256 = job->effective_input_sha256;
            v3::EventDraft draft;
            draft.kind = v3::EventKindV3::SdkJobOperationBound;
            draft.turn_id = job->turn_id; draft.step_id = job->step_id; draft.action_id = job->action_id;
            draft.payload = {{"layout", v3::kSdkJobOperationLayout}, {"version", 1}, {"jobId", job_id},
                {"tool_call_id", job->action_id}, {"attempt", 1}, {"originalInputSha256", job->original_input_sha256},
                {"effectiveInputSha256", job->effective_input_sha256}};
            for (const auto& [name, id] : std::vector<std::pair<std::string, std::string>>{
                    {"parentOperationRef", anchor->event_id}, {"assistantMessageRef", job->assistant_message_ref},
                    {"sourcePendingEventRef", job->source_pending_event_id}, {"sourceAdmissionEventRef", job->source_admission_event_id},
                    {"preparedPendingEventRef", job->prepared_pending_event_id}, {"registeredEventRef", job->registered_event_id},
                    {"adoptionEventRef", job->adopted_event_id}}) {
                const auto ref = v3::MakeOwnedJobReference(*ledger, id);
                if (!ref) { out.error = Failure("sdk.job_operation.invalid_source"); return; }
                draft.payload[name] = *ref;
            }
            auto candidate = std::make_shared<Record>();
            candidate->value.error = Failure("sdk.job_operation.unconfirmed");
            candidate->value.knowledge = JobOperationKnowledge::Unconfirmed;
            {
                std::lock_guard lock(mutex_);
                if (closing_) { out.error = Failure("sdk.job_operation.closed"); return; }
                if (host_key_ && *host_key_ != host) { out.error = Failure("sdk.job_operation.foreign_host"); return; }
                if (records_.contains(job_id)) { out.error = Failure("sdk.job_operation.already_bound"); return; }
                if (records_.size() >= capacity_) { out.error = Failure("sdk.job_operation.capacity"); return; }
                if (!host_key_) host_key_ = host;
                records_.emplace(job_id, candidate);
                record = std::move(candidate);
            }
            // Retain the first returned native receipt before any publication.
            auto native = writer->AppendEvent(std::move(draft), v3::Durability::PowerLoss);
            {
                std::lock_guard lock(mutex_);
                record->value.receipt.emplace(std::move(native));
                if (record->value.receipt->status != v3::WriteReceipt::Status::Committed || writer->broken()) {
                    if (!writer->broken() && record->value.receipt->status == v3::WriteReceipt::Status::Rejected)
                        record->value.knowledge = JobOperationKnowledge::RejectedBeforeWrite;
                    record->value.error = Failure(record->value.receipt->error_code, record->value.receipt->error_message);
                    return;
                }
                record->value.knowledge = JobOperationKnowledge::CommittedPublicationGap;
                facts->seq = record->value.receipt->seq; facts->event_id = record->value.receipt->id;
                facts->line_hash = record->value.receipt->line_hash;
                facts->operation_id = "jobop-" + facts->event_id;
                record->value.facts = facts;
            }
        });
        if (!consumed) out.error = Failure(consumed.error());
        // All producer/writer serial locks have exited before publication.
        if (record && record->value.knowledge == JobOperationKnowledge::CommittedPublicationGap && record->value.facts) {
            if (publish) publish(*facts);
            std::lock_guard lock(mutex_);
            record->value.knowledge = JobOperationKnowledge::Bound;
            record->value.error = {};
        }
    } catch (const std::exception& error) {
        try {
            std::lock_guard lock(mutex_);
            if (record) record->value.error = Failure("sdk.job_operation.unconfirmed", error.what());
            else out.error = Failure("sdk.job_operation.rejected", error.what());
        } catch (...) {}
    } catch (...) {}
    if (record) {
        std::lock_guard lock(mutex_);
        if (!first_error_ && (record->value.knowledge == JobOperationKnowledge::Unconfirmed ||
                            record->value.knowledge == JobOperationKnowledge::CommittedPublicationGap))
            first_error_ = record->value.error;
        return record->value;
    }
    return out;
}
std::optional<JobOperationRegistration> JobOperations::Snapshot(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) return std::nullopt;
    return found->second->value;
}
Result<void> JobOperations::Close() {
    if (current_binding) return std::unexpected(Failure("sdk.job_operation.reentrant"));
    std::unique_lock lock(mutex_);
    closing_ = true;
    cv_.wait(lock, [&] { return active_ == 0; });
    if (first_error_) return std::unexpected(*first_error_);
    for (const auto& [id, record] : records_) {
        (void)id;
        if (record->value.knowledge == JobOperationKnowledge::Unconfirmed ||
            record->value.knowledge == JobOperationKnowledge::CommittedPublicationGap)
            return std::unexpected(record->value.error);
    }
    return {};
}
JobOperationRecovery ReadJobOperations(const std::filesystem::path& dir, const v3::V3Ledger& ledger) {
    JobOperationRecovery out;
    const auto bindings = v3::ReadJobOperationBindings(ledger);
    if (!bindings) { out.state = JobOperationRecoveryState::Rejected; out.error = Failure("sdk.resume.job_operation_invalid", bindings.error()); return out; }
    if (bindings->empty()) return out;
    const auto parent = CheckMainOperationTurnBindings(dir, ledger);
    if (!HasParentSource(parent.state)) {
        out.state = JobOperationRecoveryState::Rejected; out.error = parent.error; return out;
    }
    out.state = JobOperationRecoveryState::PassiveHold; out.facts = *bindings;
    return out;
}
} // namespace lubancore::detail
