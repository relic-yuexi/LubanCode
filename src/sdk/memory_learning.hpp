#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <lubancore/core.hpp>
#include "api/types.hpp"
#include "config/config.hpp"
#include "memory/types.hpp"
#include "trajectory/opening.hpp"
#include "workspace/identity.hpp"

namespace lubancode::agent {
class LoopBoundaryRecorder;
struct MemoryExtraction;
} // namespace lubancode::agent

namespace lubancode::runtime {
class TrajectorySessionLedger;
} // namespace lubancode::runtime

namespace lubancore::detail {

// Narrow storage boundary for proposed candidates. Slice 1 provides only the
// local project file implementation; future SPI backends implement this seam
// without changing LearningReport semantics.
class LearningCandidateStore {
public:
    virtual ~LearningCandidateStore() = default;
    virtual std::expected<std::string, std::string> Add(
        const std::filesystem::path& memory_dir,
        lubancode::memory::MemoryCandidate candidate) = 0;
};

// One Session owns this module. All public reports are persisted per operation
// under session_dir/sdk-memory-learnings and cached as pure values so queries
// continue working after Session::Close().
class SessionMemoryLearning : public std::enable_shared_from_this<SessionMemoryLearning> {
public:
    struct TurnMaterials {
        std::string operation_id;
        std::string turn_id;
        bool turn_succeeded = false;
        bool turn_mutated = false;
        bool has_tool_evidence = false;
        std::string user_text;
        std::string assistant_text;
        std::vector<lubancode::api::Message> turn_messages;
        std::vector<std::string> tool_names;
    };

    struct PreparedDispatch {
        bool ready = false;
        bool early_terminal = false;
        memory::v1::LearningReport terminal_report;
        std::string task_type;
        std::string system_prompt;
        std::string transcript;
        std::unique_ptr<lubancode::agent::LoopBoundaryRecorder> bypass;
    };

    static Result<std::shared_ptr<SessionMemoryLearning>> Prepare(
        const std::optional<memory::v1::LearningOptions>& selection,
        std::unique_ptr<Backend>& learning_backend,
        const std::optional<Connection>& learning_connection,
        std::filesystem::path owned_root,
        lubancode::workspace::WorkspaceIdentity identity,
        std::string resume_id);

    ~SessionMemoryLearning();

    lubancode::trajectory::V3OpeningParticipant OpeningParticipant();
    memory::v1::LearningSnapshot Describe() const;

    // Must run on the Session worker while the triggering main turn is still
    // open on the main V3 bridge so bypass bridges capture the exact turn ID.
    PreparedDispatch PrepareTurnDispatch(
        const TurnMaterials& materials,
        lubancode::runtime::TrajectorySessionLedger& ledger);

    // Must run AFTER the main Operation and RecordTurnFinal are persisted so
    // durable trigger-turn confirmation strictly precedes any learning model call
    // and late learning never mutates the main Operation or its Usage.
    void SettleOrStartOperation(
        const Operation& completed,
        PreparedDispatch&& dispatch,
        lubancode::runtime::TrajectorySessionLedger& ledger);

    // Restores or validates saved reports on same-ID OpenSession with zero replay.
    Result<void> LoadOperationReport(
        const std::string& operation_id,
        const Operation& operation,
        const lubancode::trajectory::v3::V3Ledger* ledger,
        bool require_persisted_valid);

    void PopulateNotAttempted(const std::string& operation_id, const std::string& turn_id);
    Result<memory::v1::LearningReport> GetReport(const std::string& operation_id) const;

    void CancelOperation(const std::string& operation_id);
    void RequestClose();
    // Joins any live worker thread outside Session::Impl::mutex before closing
    // the V3 ledger so all bypass leases and candidate writes drain cleanly.
    void JoinAndSettle();
    // Drops the exclusive learning Backend under the caller's lifecycle guard.
    void ReleaseBackend();

private:
    struct WorkerSlot {
        explicit WorkerSlot(std::string op_id) : operation_id(std::move(op_id)) {}
        std::string operation_id;
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
    };

    nlohmann::json Plan(const std::string& session_id) const;
    std::expected<nlohmann::json, std::string> Open(const lubancode::trajectory::V3OpeningContext& context);
    Result<void> CheckBinding(const lubancode::trajectory::v3::V3Ledger& source) const;
    Result<void> CheckSavedReports(const lubancode::trajectory::v3::V3Ledger& source);
    memory::v1::LearningReport EmptyReport(
        const std::string& operation_id,
        const std::string& turn_id,
        memory::v1::LearningStage stage = memory::v1::LearningStage::NotAttempted) const;
    Result<void> PersistReport(const memory::v1::LearningReport& report) const;
    Result<std::optional<memory::v1::LearningReport>> ReadReportFromDisk(const std::string& operation_id) const;
    Result<memory::v1::LearningReport> DecodeReport(const std::string& operation_id, const std::string& bytes) const;
    Result<void> ValidateReport(
        const memory::v1::LearningReport& report,
        const lubancode::trajectory::v3::V3Ledger* source,
        bool require_complete) const;
    void ReapFinishedWorkerLocked();
    std::shared_ptr<Backend> SnapshotBackend() const;

    std::filesystem::path owned_root_, session_dir_, expected_resume_dir_, memory_dir_;
    lubancode::workspace::WorkspaceIdentity identity_;
    memory::v1::LearningSnapshot snapshot_;
    std::string resume_id_, saved_plan_bytes_;
    bool legacy_ = false;
    std::string wire_name_ = "sdk_custom";
    std::string provider_name_ = "sdk";
    std::optional<lubancode::config::Config> backend_config_;
    std::shared_ptr<LearningCandidateStore> candidate_store_;

    mutable std::mutex backend_mutex_;
    std::shared_ptr<Backend> owned_backend_;

    mutable std::mutex reports_mutex_;
    std::map<std::string, Result<memory::v1::LearningReport>> reports_;

    std::mutex worker_mutex_;
    std::shared_ptr<WorkerSlot> active_slot_;
    std::thread worker_;
    std::atomic<bool> closing_{false};
};

} // namespace lubancore::detail
