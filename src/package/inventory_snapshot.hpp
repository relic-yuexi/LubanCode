#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::package {

// Presence/layout data only. This header never includes the component loaders.
enum class InventoryComponentKind { Agent, PromptProfile, Skill, Workflow, Plugin, McpServer, Channel };
struct InventoryDirectoryLayout {
    const char* directory;
    const char* entry_file;
    InventoryComponentKind kind;
};
inline constexpr std::array<InventoryDirectoryLayout, 5> kInventoryDirectoryLayouts{{
    {"skills", "SKILL.md", InventoryComponentKind::Skill},
    {"workflows", "workflow.yaml", InventoryComponentKind::Workflow},
    {"plugins", "plugin.json", InventoryComponentKind::Plugin},
    {"mcp", "mcp.yaml", InventoryComponentKind::McpServer},
    {"channels", "channel.yaml", InventoryComponentKind::Channel},
}};

inline bool InventoryFileIsCodeBearing(std::string_view relative_path) {
    static constexpr std::array<std::string_view, 12> extensions{
        ".dll", ".so", ".dylib", ".exe", ".lua", ".py", ".js", ".mjs", ".cmd", ".bat", ".ps1", ".sh"};
    const auto dot = relative_path.rfind('.');
    if (dot != std::string_view::npos) {
        const auto extension = relative_path.substr(dot);
        for (const auto candidate : extensions) if (extension == candidate) return true;
    }
    return relative_path.rfind("plugins/", 0) == 0 || relative_path.rfind("mcp/", 0) == 0 ||
           relative_path.rfind("channels/", 0) == 0;
}

struct InventorySnapshotLimits {
    std::size_t entries;
    std::size_t depth;
    std::size_t path_bytes;
    std::size_t file_bytes;
    std::size_t total_bytes;
    std::size_t manifest_bytes;
};

struct InventorySnapshotFile {
    std::string relative_path;
    std::size_t bytes = 0;
    std::string sha256;
    bool code_bearing = false;
};
struct InventorySnapshotComponent {
    InventoryComponentKind kind;
    std::string local_id;
    std::string relative_path;
};
enum class InventorySnapshotIssueKind { Info, Warning, Error };
struct InventorySnapshotIssue {
    InventorySnapshotIssueKind kind;
    std::string path;
    std::string code;
    std::string detail;
};
struct InventorySnapshot {
    std::filesystem::path canonical_root;
    std::vector<std::string> directories;
    std::vector<InventorySnapshotFile> files;
    std::vector<InventorySnapshotComponent> components;
    std::optional<std::string> manifest_bytes;
    std::vector<InventorySnapshotIssue> issues;
    std::string content_sha256;
    std::size_t total_bytes = 0;
};
struct InventorySnapshotError { std::string code; std::string detail; };

enum class InventoryObservationPoint { AfterEnumeration, AfterFileRead, BeforeFinalEnumeration };
// Trusted internal native fixture seam. It observes the real capture and may
// change the real tree; it cannot substitute a receipt or manufacture an error.
// Arguments are borrowed only for the synchronous call. Public SDK passes null.
class InventoryObservation {
public:
    virtual ~InventoryObservation() = default;
    virtual void After(InventoryObservationPoint point, const std::filesystem::path& root,
                       std::string_view relative_path) = 0;
};

std::expected<InventorySnapshot, InventorySnapshotError> CaptureInventoryRoot(
    const std::filesystem::path& root, InventorySnapshotLimits limits,
    InventoryObservation* observation = nullptr);

} // namespace lubancode::package
