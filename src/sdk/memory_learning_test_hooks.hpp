#pragma once
// Private testing-only SDK seam. Never installed or compiled into an ordinary SDK.

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <lubancore/api.hpp>
#include "memory/types.hpp"
#include "platform/atomic_write.hpp"

namespace lubancore::detail::testing {

using LearningWorkerStartHook = std::function<void()>;
using LearningWorkerStartHookHandle = std::shared_ptr<const LearningWorkerStartHook>;

using LearningWorkerExecutionHook = std::function<void(const std::string& operation_id)>;
using LearningWorkerExecutionHookHandle = std::shared_ptr<const LearningWorkerExecutionHook>;

using LearningCandidateStoreHook = std::function<std::expected<std::string, std::string>(
    const std::filesystem::path& memory_dir, lubancode::memory::MemoryCandidate candidate)>;
using LearningCandidateStoreHookHandle = std::shared_ptr<const LearningCandidateStoreHook>;

using LearningCommitWriteHook = std::function<std::expected<
    lubancode::platform::AtomicWriteReceipt, lubancode::platform::AtomicWriteError>(
    const std::filesystem::path& path,
    std::string_view content,
    lubancode::platform::WriteDurability durability)>;
using LearningCommitWriteHookHandle = std::shared_ptr<const LearningCommitWriteHook>;

LUBANCORE_API LearningWorkerStartHookHandle ReplaceLearningWorkerStartHook(
    LearningWorkerStartHookHandle replacement) noexcept;
LearningWorkerStartHookHandle SnapshotLearningWorkerStartHook() noexcept;

LUBANCORE_API LearningWorkerExecutionHookHandle ReplaceLearningWorkerExecutionHook(
    LearningWorkerExecutionHookHandle replacement) noexcept;
LearningWorkerExecutionHookHandle SnapshotLearningWorkerExecutionHook() noexcept;

LUBANCORE_API LearningCandidateStoreHookHandle ReplaceLearningCandidateStoreHook(
    LearningCandidateStoreHookHandle replacement) noexcept;
LearningCandidateStoreHookHandle SnapshotLearningCandidateStoreHook() noexcept;

LUBANCORE_API LearningCommitWriteHookHandle ReplaceLearningCommitWriteHook(
    LearningCommitWriteHookHandle replacement) noexcept;
LearningCommitWriteHookHandle SnapshotLearningCommitWriteHook() noexcept;

} // namespace lubancore::detail::testing
