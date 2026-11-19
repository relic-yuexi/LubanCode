#pragma once

#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <lubancore/api.hpp>
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::runtime { class SessionService; }
namespace lubancode::tools { class ToolJobCoordinator; }
namespace lubancore::detail {

enum class JobOperationKnowledge { RejectedBeforeWrite, Unconfirmed, CommittedPublicationGap, Bound };
struct JobOperationRegistration {
    JobOperationKnowledge knowledge = JobOperationKnowledge::RejectedBeforeWrite;
    std::optional<lubancode::trajectory::v3::WriteReceipt> receipt;
    std::shared_ptr<const lubancode::trajectory::v3::JobOperationBindingFacts> facts;
    Error error;
    bool bound() const noexcept { return knowledge == JobOperationKnowledge::Bound; }
};

// Internal value owner. Borrowed producers exist only during Bind; snapshots
// contain no live writer/coordinator/tool/cancellation or execution permission.
// Host keeps Service/coordinator/actual Writer alive and excludes external
// writes and Service Close/Clear/Resume until this table's Close drains Bind.
// Retire table first, Coordinator::Shutdown next, Service lifecycle last.
class JobOperations final {
public:
    explicit JobOperations(std::size_t capacity = 64);
    ~JobOperations();
    JobOperations(const JobOperations&) = delete;
    JobOperations& operator=(const JobOperations&) = delete;
    JobOperationRegistration Bind(lubancode::runtime::SessionService& service,
        lubancode::tools::ToolJobCoordinator& coordinator, const std::string& job_id,
        const std::function<void(const lubancode::trajectory::v3::JobOperationBindingFacts&)>& publish = {});
    std::optional<JobOperationRegistration> Snapshot(const std::string& job_id) const;
    Result<void> Close(); // Retire this table, not a public process-cancellation policy.
private:
    struct Record;
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool closing_ = false;
    std::size_t active_ = 0;
    std::optional<std::string> host_key_;
    std::optional<Error> first_error_;
    std::map<std::string, std::shared_ptr<Record>> records_;
};

enum class JobOperationRecoveryState { NotApplicable, PassiveHold, Rejected };
struct JobOperationRecovery {
    JobOperationRecoveryState state = JobOperationRecoveryState::NotApplicable;
    std::vector<lubancode::trajectory::v3::JobOperationBindingFacts> facts;
    Error error;
};
JobOperationRecovery ReadJobOperations(const std::filesystem::path& session_dir,
    const lubancode::trajectory::v3::V3Ledger& ledger);
} // namespace lubancore::detail
