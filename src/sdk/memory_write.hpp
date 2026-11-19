#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>

#include <lubancore/core.hpp>
#include "memory/memory_tool.hpp"
#include "trajectory/opening.hpp"
#include "workspace/identity.hpp"

namespace lubancode::runtime { class TrajectorySessionLedger; }
namespace lubancore::detail {

// One Session owns this module and its registry target. BuildTool borrows that
// Session's ledger; the registry and current worker must close before the ledger.
// Public queries read copied reports cached by the SDK core, not this live target.
class SessionMemoryWrite : public std::enable_shared_from_this<SessionMemoryWrite> {
public:
    static Result<std::shared_ptr<SessionMemoryWrite>> Prepare(
        const std::optional<memory::v1::WriteOptions>& selection,
        std::filesystem::path owned_root, lubancode::workspace::WorkspaceIdentity identity,
        std::string resume_id);
    lubancode::trajectory::V3OpeningParticipant OpeningParticipant();
    memory::v1::WriteSnapshot Describe() const { return snapshot_; }
    std::unique_ptr<lubancode::tools::Tool> BuildTool(lubancode::runtime::TrajectorySessionLedger& ledger);
    // Called before the durable SDK operation final. Even zero-call operations
    // get a durable empty report. Failure must revoke complete/result_persisted.
    Result<void> FinalizeOperation(const std::string& operation_id, const std::string& turn_id);
    Result<std::vector<memory::v1::SaveReport>> ReadReports(const std::string& operation_id) const;
    Result<void> ValidateReports(const std::string& operation_id, const std::string& turn_id,
        const std::vector<memory::v1::SaveReport>& reports,
        const lubancode::trajectory::v3::V3Ledger& source, bool require_adopted = false) const;
    bool HasIndeterminate() const { return indeterminate_; } // worker-owned; read after join/run

private:
    nlohmann::json Plan(const std::string& session_id) const;
    std::expected<nlohmann::json, std::string> Open(const lubancode::trajectory::V3OpeningContext& context);
    Result<void> CheckBinding(const lubancode::trajectory::v3::V3Ledger& source) const;
    Result<void> CheckSavedReports(const lubancode::trajectory::v3::V3Ledger& source);
    Result<std::vector<memory::v1::SaveReport>> DecodeReports(const std::string& operation_id,
        const std::string& bytes) const;
    Result<void> PersistReports(const std::string& operation_id, const std::string& turn_id) const;
    lubancode::tools::Tool::Result Save(std::expected<lubancode::memory::SaveRequest, std::string> request,
        const lubancode::tools::ToolExecutionContext& context,
        lubancode::runtime::TrajectorySessionLedger& ledger);
    std::filesystem::path owned_root_, session_dir_, expected_resume_dir_;
    lubancode::workspace::WorkspaceIdentity identity_;
    memory::v1::WriteSnapshot snapshot_;
    std::string resume_id_, saved_plan_bytes_;
    bool legacy_ = false, indeterminate_ = false;
    std::map<std::string, std::vector<memory::v1::SaveReport>> reports_;
};
} // namespace lubancore::detail
