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

} // namespace lubancore::packages::v1
