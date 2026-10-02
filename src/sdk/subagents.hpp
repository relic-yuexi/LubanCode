#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <lubancore/api.hpp>
#include <lubancore/subagents.hpp>

#include "runtime/assembly/session_resources.hpp"
#include "tools/agent_tool.hpp"
#include "trajectory/opening.hpp"

namespace lubancore::detail {

// No writer/backend borrow. The strict inspector owns its bounded historical
// check; SDK adds only the accepted operation/session envelope.
Result<std::vector<subagents::v1::Report>> ReadSubagentReports(
    const lubancode::trajectory::v3::V3Ledger& ledger, const std::filesystem::path& session_dir,
    const std::string& session_id, const std::string& operation_id,
    const std::string& turn_id, bool require_complete, bool allow_unconsumed = false);
subagents::v1::LiveTerminalReceipt CopyChildReceipt(const lubancode::runtime::SubagentTerminalReceipt& receipt);

// Initialization-only value owner. Its opening callback runs under the existing
// SessionLock, before old system adoption or any public execution can start.
class SessionSubagentPlan final : public std::enable_shared_from_this<SessionSubagentPlan> {
public:
    static Result<std::shared_ptr<SessionSubagentPlan>> Prepare(
        const std::optional<subagents::v1::Options>& options,
        std::filesystem::path root, std::string workspace_key, std::string resume_id,
        std::string cwd, std::string parent_model, std::string permission_floor,
        int parent_max_steps, int parent_max_wall_seconds = 0);
    Result<void> BindTools(const lubancode::tools::ToolRegistry& registry);
    bool enabled() const { return snapshot_.enabled; }
    subagents::v1::Snapshot Describe() const { return snapshot_; }
    lubancode::trajectory::V3OpeningParticipant OpeningParticipant();

private:
    using Json = nlohmann::json;
    Json Plan(const std::string& session_id) const;
    Result<void> CheckBinding(const lubancode::trajectory::v3::V3Ledger& ledger) const;
    Result<void> CheckRecoveredChildren(const lubancode::trajectory::v3::V3Ledger& ledger) const;
    std::expected<Json, std::string> Open(const lubancode::trajectory::V3OpeningContext& context);
    std::filesystem::path root_, resume_dir_;
    std::string resume_id_, saved_bytes_;
    Json saved_plan_, tool_facts_;
    bool legacy_ = false, bound_ = false;
    int parent_max_steps_ = 0, parent_max_wall_seconds_ = 0;
    subagents::v1::Snapshot snapshot_;
};

// The attachment owns one coordinator/facade and two exact role overlays. It
// borrows this Session's backend/tools only until its resource graph is retired.
// Main foreground only: parent and child never call the backend concurrently.
class SessionSubagents final : public lubancode::runtime::assembly::SessionResourceAttachment {
public:
    static Result<std::unique_ptr<SessionSubagents>> Build(
        std::shared_ptr<SessionSubagentPlan> plan, lubancode::api::Backend& backend,
        lubancode::tools::ToolRegistry& registry, lubancode::agent::AgentProfile profile);
    ~SessionSubagents() override;
    std::unique_ptr<lubancode::tools::Tool> BuildTool();
    void BindTurn(lubancode::tools::AgentTool::Hooks hooks,
                  std::string session_id, std::string operation_id, std::string turn_id);
    void ClearTurn() noexcept;
    void RequestClose() noexcept;
    lubancode::tools::Tool::Result Execute(const nlohmann::json& input,
        const lubancode::tools::ToolExecutionContext& context);
    nlohmann::json InputSchema() const;

private:
    SessionSubagents() = default;
    std::shared_ptr<SessionSubagentPlan> plan_;
    lubancode::agent::AgentProfile parent_profile_;
    std::unique_ptr<lubancode::tools::ToolRegistry> general_, explore_;
    std::unique_ptr<lubancode::tools::AgentTool> agent_;
    // Owner-thread facts. Cross-thread shutdown changes only this atomic and the
    // coordinator's own close latch; it never clears live borrowed callbacks.
    std::string session_id_, operation_id_, turn_id_;
    std::atomic<bool> closing_{false};
};

} // namespace lubancore::detail
