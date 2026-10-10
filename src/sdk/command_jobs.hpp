#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <lubancore/core.hpp>
#include "agent/async_tool_seam.hpp"
#include "runtime/assembly/session_resources.hpp"
#include "runtime/async_tool_runtime.hpp"
#include "sdk/approval.hpp"
#include "sdk/command_jobs_opening.hpp"
#include "sdk/job_operations.hpp"

namespace lubancode::runtime { class SessionService; }
namespace lubancore::detail {

// One Session's owner. External query methods touch only owned cache values.
// Admission/Pump/Post/Finalize run on the host worker or its joined Close path.
class SessionCommandJobs final : public std::enable_shared_from_this<SessionCommandJobs> {
public:
    using Publisher = std::function<void(Event)>;
    static Result<std::shared_ptr<SessionCommandJobs>> Build(
        std::shared_ptr<SessionCommandJobPlan>, lubancode::runtime::SessionService&,
        SessionApprovals&, ApprovalMode, std::chrono::milliseconds approval_timeout, Publisher);
    std::unique_ptr<lubancode::runtime::assembly::SessionResourceAttachment> Attachment();
    void SetParent(std::string operation_id, std::string turn_id);
    void StopParent(const std::string& operation_id, const std::string& reason);
    void EndParent(const std::string& operation_id);
    void PumpAndPublish();
    bool HasPending() const noexcept { return pending_.load(std::memory_order_acquire); }
    bool indeterminate() const noexcept { return indeterminate_.load(std::memory_order_acquire); }
    void RequestClose();
    Result<void> RetireBindings();
    Result<void> Finalize(); // after host worker joined; joins coordinator before Service Close
    Result<std::vector<jobs::v1::JobView>> List(std::optional<std::string>) const;
    Result<jobs::v1::JobView> Read(const jobs::v1::Identity&) const;
    Result<jobs::v1::JobView> Wait(const jobs::v1::Identity&, std::chrono::milliseconds) const;
    Result<void> Cancel(const jobs::v1::Identity&);
    Result<jobs::v1::Preview> Preview(const jobs::v1::Identity&, std::size_t max_bytes) const;
private:
    struct Record;
    SessionCommandJobs(std::shared_ptr<SessionCommandJobPlan> plan);
    lubancode::agent::OwnedJobAdmissionReceipt Admit(lubancode::runtime::TrajectoryTurnBridge&,
        const lubancode::api::ToolUseBlock&, const lubancode::agent::OwnedToolAdmissionContext&);
    lubancode::trajectory::v3::WriteReceipt Post(
        const lubancode::tools::OwnedJobCompletion&, const lubancode::tools::OwnedJobPostInvocation&);
    Result<void> Restore();
    void Freeze(const std::string& why);
    std::shared_ptr<SessionCommandJobPlan> plan_;
    lubancode::runtime::SessionService* service_ = nullptr; // owner-thread, revoked before Service destruction
    SessionApprovals* approvals_ = nullptr;
    ApprovalMode approval_mode_ = ApprovalMode::Confirm;
    std::chrono::milliseconds approval_timeout_{0};
    Publisher publisher_;
    std::shared_ptr<lubancode::tools::ToolJobCoordinator> coordinator_;
    lubancode::tools::PreparedJobOwner owner_;
    JobOperations operations_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::map<std::string, std::shared_ptr<Record>> records_;
    std::map<std::string, std::shared_ptr<Record>> by_call_;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> parent_stops_;
    std::string active_parent_, active_turn_;
    bool closing_ = false, finalized_ = false;
    std::atomic<bool> pending_{false}, indeterminate_{false};
    std::optional<Error> first_error_;
};

} // namespace lubancore::detail
