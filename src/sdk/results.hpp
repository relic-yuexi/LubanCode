#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <lubancore/results.hpp>

#include "trajectory/v3/reader.hpp"

namespace lubancore::detail {

// Only a verified, quiescent V3 ledger creates this index. It contains no live
// writer, Agent, callback or original output bytes.
struct ToolResultArtifact {
    std::string id;
    std::string kind;
    std::string path;
    std::string sha256;
    std::uint64_t bytes = 0;
    std::string media_type;
};
struct ToolResultIndexEntry {
    results::v1::ToolResultSummary summary;
    std::string execution_event_id;
    std::vector<ToolResultArtifact> artifacts;
};
struct OperationToolResultIndex {
    std::vector<ToolResultIndexEntry> entries;
};

Result<OperationToolResultIndex> IndexToolResults(
    const lubancode::trajectory::v3::V3Ledger& ledger,
    const std::string& session_id, const std::string& operation_id,
    const std::string& turn_id);

Result<results::v1::SavedSnapshot> ReadIndexedToolResult(
    const std::filesystem::path& session_dir, const ToolResultIndexEntry& entry,
    const results::v1::SessionResultPolicy& policy,
    results::v1::ToolResultReadOptions options);

Result<results::v1::SessionResultPolicy> FreezeResultPolicy(
    const std::filesystem::path& session_dir, const std::string& session_id,
    const std::optional<results::v1::SessionResultOptions>& requested, bool resume);

} // namespace lubancore::detail
