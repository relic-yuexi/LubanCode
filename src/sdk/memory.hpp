#pragma once

#include <filesystem>
#include <memory>
#include <optional>

#include <lubancore/core.hpp>
#include "memory/project_memory.hpp"
#include "runtime/memory_ledger_bridge.hpp"
#include "trajectory/opening.hpp"
#include "workspace/identity.hpp"

namespace lubancore::detail {

class SessionMemory : public std::enable_shared_from_this<SessionMemory> {
public:
    static Result<std::shared_ptr<SessionMemory>> Prepare(
        const std::optional<memory::v1::RecallOptions>& selection,
        std::filesystem::path owned_root, lubancode::workspace::WorkspaceIdentity identity,
        std::string resume_id, std::filesystem::path cwd);
    lubancode::trajectory::V3OpeningParticipant OpeningParticipant();
    memory::v1::Snapshot Describe() const { return snapshot_; }
    struct Recall {
        std::string context;
        memory::v1::RecallReport report;
        std::vector<lubancode::memory::InjectedMemoryRecord> records;
    };
    Result<Recall> BuildRecall(const std::string& query, const std::string& operation_id,
                              const std::string& turn_id) const;
    Result<void> PersistReport(const memory::v1::RecallReport& report) const;
    Result<memory::v1::RecallReport> ReadReport(const std::string& operation_id,
        std::size_t* actual_bytes = nullptr, std::size_t byte_limit = 524288) const;
    Result<void> ValidateReport(const memory::v1::RecallReport& report,
                                const lubancode::trajectory::v3::V3Ledger& source) const;
    memory::v1::RecallReport EmptyReport(const std::string& operation_id, const std::string& turn_id) const;

private:
    nlohmann::json Plan(const std::string& session_id) const;
    std::expected<nlohmann::json, std::string> Open(const lubancode::trajectory::V3OpeningContext& context);
    Result<void> CheckBinding(const lubancode::trajectory::v3::V3Ledger& source) const;
    Result<void> CheckSavedReports(const lubancode::trajectory::v3::V3Ledger& source) const;
    std::filesystem::path owned_root_, session_dir_, expected_resume_dir_, cwd_;
    lubancode::workspace::WorkspaceIdentity identity_;
    memory::v1::Snapshot snapshot_;
    std::string resume_id_, saved_plan_bytes_;
    bool legacy_ = false;
    std::shared_ptr<lubancode::trajectory::MemoryCapability> memory_capability_;
};

} // namespace lubancore::detail
