// Internal project/upsert gate. No CLI configuration, worker launch or SDK write
// permission lives here. All inputs and receipts are owned values.
#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "memory/types.hpp"
#include "platform/atomic_write.hpp"

namespace lubancode::memory {

enum class ProjectCommitState { NotStarted, Committed, Indeterminate };
enum class ProjectCommitStage { Intent, Snapshot, Topic, Cleanup, Catalog, Index, Result };
struct ProjectCommitStageReceipt {
    ProjectCommitStage stage = ProjectCommitStage::Intent;
    platform::WriteOutcome outcome = platform::WriteOutcome::NotCommitted;
};
struct ProjectCommitContext {
    std::filesystem::path project_root;
    std::filesystem::path memory_directory;
    std::filesystem::path lifecycle_root;
    std::string workspace_key;
    std::string operation_id;
    std::string session_id;
    std::string source_event_ref;
};
struct ProjectCommitReceipt {
    ProjectCommitState state = ProjectCommitState::NotStarted;
    std::string operation_id;
    std::string workspace_key;
    std::string request_sha256;
    std::string memory_id;
    std::string memory_path;
    std::string content_sha256;
    std::string committed_at;
    std::vector<ProjectCommitStageReceipt> stages;
    bool duplicate = false;
    std::string error_code;
    std::string error;
};

// Only before mutation may cancellation return NotStarted. A visible topic or
// an unresolved previous intent stays Indeterminate; the gate never replays it.
ProjectCommitReceipt CommitProjectUpsert(const ProjectCommitContext& context,
                                        const SaveRequest& request,
                                        std::stop_token stop = {});
using ProjectCommitCancellation = std::function<bool()>;
// A synchronous host may borrow its existing atomic cancellation flag through
// this predicate. It is invoked only during this call and is never retained.
ProjectCommitReceipt CommitProjectUpsertWithCancellation(const ProjectCommitContext& context,
                                                        const SaveRequest& request,
                                                        ProjectCommitCancellation cancelled);

// CLI completion readers use this only for a new commit_schema receipt. It
// confirms the existing result under the same lock and cannot start an upsert.
ProjectCommitReceipt ConfirmProjectCommitReceipt(const std::filesystem::path& lifecycle_root,
                                                const std::string& operation_id);
// Existing immutable materials only: checks owner/request/stages/snapshot under
// the project lock, but never creates memory/lifecycle/operation directories,
// writes a receipt or a topic. The ordinary lock directory is created/released.
// Result outcome is omitted: this read cannot confirm an earlier result flush.
ProjectCommitReceipt InspectProjectCommitReceipt(const ProjectCommitContext& context,
                                                 const SaveRequest& request);
std::string ProjectCommitRequestSha256(const ProjectCommitContext& context,
                                      const SaveRequest& request);

// Deterministic phase failure tests may replace one write, preserving platform
// outcomes. Production calls the overload above; no global injection state.
namespace commit_testing {
using WriteFile = std::function<std::expected<platform::AtomicWriteReceipt,
                                             platform::AtomicWriteError>(
    const std::filesystem::path&, std::string_view, platform::WriteDurability)>;
ProjectCommitReceipt CommitProjectUpsert(const ProjectCommitContext& context,
                                        const SaveRequest& request, WriteFile write,
                                        std::stop_token stop = {});
}  // namespace commit_testing
}  // namespace lubancode::memory
