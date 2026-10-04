#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "lubancore/api.hpp"

namespace lubancore::packages::v1 {

inline constexpr std::size_t kMaxManifestBytes = 256 * 1024;
inline constexpr std::size_t kMaxHostVersionBytes = 4096;
inline constexpr std::size_t kMaxPlatformBytes = 16;

struct Input {
    std::string yaml;
    // Omission does not consult the library version, platform or environment.
    std::optional<std::string> lubancode_version;
    std::optional<std::string> platform;
};

struct Author { std::string name; std::string url; };

struct Manifest {
    int schema = 1;
    std::string id;
    std::string version;
    std::string name;
    std::string description;
    std::vector<Author> authors;
    std::string license;
    std::string homepage;
    std::string repository;
    std::optional<std::string> compatibility_lubancode;
    std::vector<std::string> compatibility_platforms;
};

struct Issue { std::string field; int line = 0; std::string detail; };
enum class Compatibility { NotDeclared, NotEvaluated, Compatible, Incompatible };

struct Analysis {
    // Check this first: root parsing says nothing about inventory, trust or mount.
    std::optional<Manifest> manifest;
    std::vector<Issue> issues;
    // SHA-256 of the exact input YAML bytes; not publisher authentication.
    std::string input_sha256;
    // Failed parsing leaves both NotEvaluated, never NotDeclared/Compatible.
    Compatibility version_compatibility = Compatibility::NotEvaluated;
    Compatibility platform_compatibility = Compatibility::NotEvaluated;
};

// Synchronous owned text -> owned value. No Session, files, tools or processes.
// UTF-8/NUL/size or explicit host-option failures return Error; manifest parser
// diagnostics return Analysis with no manifest and the original field/line/detail.
LUBANCORE_API Result<Analysis> AnalyzeManifest(Input input);

inline constexpr std::size_t kMaxInventoryEntries = 4096;
inline constexpr std::size_t kMaxInventoryDepth = 32;
inline constexpr std::size_t kMaxInventoryPathBytes = 4096;
inline constexpr std::size_t kMaxInventoryFileBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxInventoryTotalBytes = 64 * 1024 * 1024;

struct InventoryLimits {
    // Positive limits may be lowered, never raised above these defaults.
    std::size_t entries = kMaxInventoryEntries;
    std::size_t depth = kMaxInventoryDepth;
    std::size_t path_bytes = kMaxInventoryPathBytes;
    std::size_t file_bytes = kMaxInventoryFileBytes;
    std::size_t total_bytes = kMaxInventoryTotalBytes;
};

struct InventoryInput {
    // Exactly one absolute normalized UTF-8 package root, not a layer to scan.
    std::string root;
    InventoryLimits limits;
    std::optional<std::string> lubancode_version;
    std::optional<std::string> platform;
};

enum class ComponentKind { Agent, PromptProfile, Skill, Workflow, Plugin, McpServer, Channel };
enum class InventoryIssueKind { Info, Warning, Error };

struct InventoryFile {
    std::string relative_path;
    std::size_t bytes = 0;
    std::string sha256;
    // Static filename/directory evidence, never permission to execute.
    bool code_bearing = false;
};

struct InventoryComponent {
    ComponentKind kind = ComponentKind::Skill;
    std::string local_id;
    std::string relative_path;
    // A display name only; absent when the root manifest did not parse.
    std::optional<std::string> canonical_id;
};

struct InventoryIssue {
    InventoryIssueKind kind = InventoryIssueKind::Info;
    std::string path;
    std::string code;
    std::string detail;
};

struct Inventory {
    std::string root;
    std::string canonical_root;
    std::vector<std::string> directories;
    std::vector<InventoryFile> files;
    std::vector<InventoryComponent> components;
    // Missing package.yaml is distinct from an empty or rejected YAML file.
    std::optional<Analysis> manifest_analysis;
    std::vector<InventoryIssue> issues;
    // Frozen Package v1 material built from actual byte counts and file hashes.
    std::string content_sha256;
    std::size_t total_bytes = 0;
};

// Synchronous bounded observation. The host keeps the root tree still until
// return; detected drift fails the call. This is neither an atomic snapshot nor
// a filesystem sandbox. Success discovers entry shapes, not parsed components,
// resolved references, publisher trust, mounted resources or executable plans.
// All values remain owned after the input and source tree have been destroyed.
LUBANCORE_API Result<Inventory> InventoryExplicitRoot(InventoryInput input);

} // namespace lubancore::packages::v1
