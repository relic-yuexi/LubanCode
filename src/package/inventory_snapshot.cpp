#include "package/inventory_snapshot.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>
#include <system_error>
#include <utility>

#include "package/inventory.hpp"
#include "platform/bounded_read.hpp"
#include "platform/dir_fingerprint.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"

namespace lubancode::package {
namespace {
namespace fs = std::filesystem;
using Error = InventorySnapshotError;
template<class T> using Result = std::expected<T, Error>;

std::unexpected<Error> Fail(const char* code, std::string detail) {
    return std::unexpected(Error{code, std::move(detail)});
}
std::unexpected<Error> Changed() { return Fail("sdk.package.source_changed", "Package tree changed during bounded observation"); }

bool Text(const std::string& text, std::size_t cap) {
    return !text.empty() && text.size() <= cap && text.find('\0') == std::string_view::npos &&
           platform::IsValidUtf8(text);
}
bool Relative(std::string_view text) {
    if (CheckPackageRelativePath(text) != PathIssue::None || text.find_first_of("\\\t\r\n") != std::string_view::npos)
        return false;
    const auto path = platform::Utf8ToPath(std::string(text));
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory() || path.lexically_normal() != path)
        return false;
    for (const auto& part : path) if (part.empty() || part == "." || part == "..") return false;
    return true;
}

struct ObservedEntry {
    std::string relative_path;
    bool directory = false;
    std::uintmax_t bytes = 0;
    fs::file_time_type modified;
    bool operator==(const ObservedEntry&) const = default;
};

Result<ObservedEntry> Inspect(const fs::path& path, std::string relative_path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec) return Fail("sdk.package.read_failed", "Cannot inspect package entry");
    if (fs::is_symlink(status)) return Fail("sdk.package.linked_source", "Package entry is a symbolic link");
#ifdef _WIN32
    const auto native = platform::FileIoPath(path);
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return Fail("sdk.package.read_failed", "Cannot inspect package reparse attributes");
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return Fail("sdk.package.linked_source", "Package entry is a reparse point or junction");
#endif
    ObservedEntry entry;
    entry.relative_path = std::move(relative_path);
    entry.directory = fs::is_directory(status);
    if (!entry.directory && !fs::is_regular_file(status))
        return Fail("sdk.package.nonregular_source", "Package entry is not a directory or regular file");
    if (!entry.directory) {
        entry.bytes = fs::file_size(path, ec);
        if (ec) return Fail("sdk.package.read_failed", "Cannot read package file size");
    }
    entry.modified = fs::last_write_time(path, ec);
    if (ec) return Fail("sdk.package.read_failed", "Cannot read package modification time");
    return entry;
}

Result<std::vector<ObservedEntry>> Enumerate(const fs::path& root, InventorySnapshotLimits limits) {
    std::error_code ec;
    fs::recursive_directory_iterator iterator(root, fs::directory_options::none, ec), end;
    if (ec) return Fail("sdk.package.read_failed", "Cannot enumerate package root");
    std::vector<ObservedEntry> entries;
    while (iterator != end) {
        if (entries.size() >= limits.entries || static_cast<std::size_t>(iterator.depth()) >= limits.depth)
            return Fail("sdk.package.limit_exceeded", "Package entry count or depth exceeds its limit");
        const auto path = iterator->path();
        const auto relative = path.lexically_relative(root).generic_u8string();
        const std::string name(reinterpret_cast<const char*>(relative.data()), relative.size());
        if (name.size() > limits.path_bytes) return Fail("sdk.package.limit_exceeded", "Package path exceeds its limit");
        if (!Text(name, limits.path_bytes)) return Fail("sdk.package.invalid_input", "Package path must be nonempty UTF-8 without NUL");
        if (!Relative(name)) return Fail("sdk.package.invalid_input", "Package relative path is unsafe or not normalized");
        auto entry = Inspect(path, name);
        if (!entry) return std::unexpected(std::move(entry.error()));
        if (!entry->directory && entry->bytes > (name == "package.yaml" ? (std::min)(limits.file_bytes, limits.manifest_bytes) : limits.file_bytes))
            return Fail("sdk.package.limit_exceeded", "Package file exceeds its byte limit");
        entries.push_back(std::move(*entry));
        iterator.increment(ec);
        if (ec) return Fail("sdk.package.read_failed", "Package enumeration failed before completion");
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.relative_path < right.relative_path;
    });
    return entries;
}

Result<void> Mapping(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root);
    auto cursor = root;
    for (auto part = relative.begin(); part != relative.end(); ++part) {
        cursor /= *part;
        auto entry = Inspect(cursor, {});
        if (!entry || (std::next(part) != relative.end() && !entry->directory)) return Changed();
    }
    std::error_code ec;
    if (fs::canonical(path, ec) != path || ec) return Changed();
    return {};
}

void Describe(InventorySnapshot& snapshot) {
    using Kind = InventoryComponentKind;
    using IssueKind = InventorySnapshotIssueKind;
    std::set<std::string> files;
    for (const auto& file : snapshot.files) files.insert(file.relative_path);
    for (const auto& dir : snapshot.directories) {
        for (const auto& layout : kInventoryDirectoryLayouts) {
            const std::string prefix = std::string(layout.directory) + "/";
            if (!dir.starts_with(prefix)) continue;
            const auto local = dir.substr(prefix.size());
            if (local.empty() || local.find('/') != std::string::npos) continue;
            if (files.contains(dir + "/" + layout.entry_file))
                snapshot.components.push_back({layout.kind, local, dir});
            else snapshot.issues.push_back({IssueKind::Warning, dir, "missing_component_entry",
                std::string("Regular component entry is missing: ") + layout.entry_file});
        }
        constexpr std::string_view profiles = "prompts/profiles/";
        if (dir.starts_with(profiles)) {
            const auto local = dir.substr(profiles.size());
            if (!local.empty() && local.find('/') == std::string::npos)
                snapshot.components.push_back({Kind::PromptProfile, local, dir});
        }
    }
    for (const auto& file : snapshot.files) {
        constexpr std::string_view agents = "agents/";
        if (file.relative_path.starts_with(agents) && file.relative_path.ends_with(".yaml")) {
            const auto name = file.relative_path.substr(agents.size());
            const auto local = name.substr(0, name.size() - 5);
            if (!local.empty() && name.find('/') == std::string::npos)
                snapshot.components.push_back({Kind::Agent, local, file.relative_path});
        }
    }
    std::sort(snapshot.components.begin(), snapshot.components.end(), [](const auto& left, const auto& right) {
        if (left.kind != right.kind) return left.kind < right.kind;
        return left.relative_path < right.relative_path;
    });
    const auto reserved_vector = ReservedTopLevelNames();
    const std::set<std::string> reserved(reserved_vector.begin(), reserved_vector.end());
    const auto top = [&](const std::string& path, bool directory) {
        if (path.find('/') != std::string::npos) return;
        if (reserved.contains(path)) {
            const bool root_file = path == "package.yaml" || path == "README.md" || path == "LICENSE";
            if (root_file == directory) snapshot.issues.push_back({IssueKind::Error, path,
                "invalid_top_level_type", "Reserved package entry has the wrong type"});
        } else {
            const auto near_miss = NearMissStandardDir(path);
            snapshot.issues.push_back({near_miss.empty() ? IssueKind::Info : IssueKind::Warning, path,
                near_miss.empty() ? "unknown_top_level" : "near_miss_directory",
                near_miss.empty() ? "Unknown top-level entry is retained in inventory" : "Possible misspelling of " + near_miss});
        }
    };
    for (const auto& dir : snapshot.directories) top(dir, true);
    for (const auto& file : snapshot.files) top(file.relative_path, false);
    std::sort(snapshot.issues.begin(), snapshot.issues.end(), [](const auto& left, const auto& right) {
        if (left.path != right.path) return left.path < right.path;
        return left.code < right.code;
    });
}
} // namespace

std::expected<InventorySnapshot, InventorySnapshotError> CaptureInventoryRoot(
    const fs::path& declared_root, InventorySnapshotLimits limits, InventoryObservation* observation) {
    if (!limits.entries || !limits.depth || !limits.path_bytes || !limits.file_bytes || !limits.total_bytes || !limits.manifest_bytes)
        return Fail("sdk.package.invalid_limits", "Package inventory limits must be positive");
    try {
        if (!declared_root.is_absolute() || declared_root.lexically_normal() != declared_root || !Text(platform::PathToUtf8(declared_root), limits.path_bytes))
            return Fail("sdk.package.invalid_input", "Package root must be absolute, normalized and valid UTF-8");
        // A trailing separator can make POSIX symlink_status follow a directory
        // link. Inspect its actual leaf, while preserving filesystem root paths.
        auto root = declared_root;
        while (root != root.root_path() && root.filename().empty()) root = root.parent_path();
        auto source = Inspect(root, {});
        if (!source) return std::unexpected(std::move(source.error()));
        if (!source->directory) return Fail("sdk.package.invalid_input", "Package root must be a directory");
        std::error_code ec;
        InventorySnapshot snapshot;
        snapshot.canonical_root = fs::canonical(root, ec);
        if (ec) return Fail("sdk.package.read_failed", "Cannot resolve explicit package root");
        if (!Text(platform::PathToUtf8(snapshot.canonical_root), limits.path_bytes))
            return Fail("sdk.package.limit_exceeded", "Canonical package root exceeds its path limit");
        auto root_stamp = Inspect(snapshot.canonical_root, {});
        if (!root_stamp) return std::unexpected(std::move(root_stamp.error()));
        auto entries = Enumerate(snapshot.canonical_root, limits);
        if (!entries) return std::unexpected(std::move(entries.error()));
        if (observation) observation->After(InventoryObservationPoint::AfterEnumeration, snapshot.canonical_root, {});
        std::vector<platform::LedgerFile> ledger;
        for (const auto& entry : *entries) {
            if (entry.directory) { snapshot.directories.push_back(entry.relative_path); continue; }
            const auto path = snapshot.canonical_root / platform::Utf8ToPath(entry.relative_path);
            auto mapping = Mapping(snapshot.canonical_root, path);
            if (!mapping) return std::unexpected(std::move(mapping.error()));
            auto before = Inspect(path, entry.relative_path);
            if (!before || *before != entry) return Changed();
            auto cap = (std::min)(limits.file_bytes, limits.total_bytes - snapshot.total_bytes);
            if (entry.relative_path == "package.yaml") cap = (std::min)(cap, limits.manifest_bytes);
            auto bytes = platform::ReadBoundedRegularFile(platform::FileIoPath(path), cap);
            if (!bytes) return Fail(bytes.error() == "read.limit_exceeded" ? "sdk.package.limit_exceeded" : "sdk.package.read_failed",
                                    "Cannot complete bounded package file read: " + bytes.error());
            if (observation) observation->After(InventoryObservationPoint::AfterFileRead, snapshot.canonical_root, entry.relative_path);
            auto after = Inspect(path, entry.relative_path);
            mapping = Mapping(snapshot.canonical_root, path);
            if (!after || *after != entry || !mapping || bytes->size() != entry.bytes) return Changed();
            const auto sha256 = platform::Sha256Hex(*bytes);
            snapshot.files.push_back({entry.relative_path, bytes->size(), sha256, InventoryFileIsCodeBearing(entry.relative_path)});
            snapshot.total_bytes += bytes->size();
            ledger.push_back({entry.relative_path, bytes->size(), sha256});
            if (entry.relative_path == "package.yaml") snapshot.manifest_bytes = std::move(*bytes);
        }
        if (observation) observation->After(InventoryObservationPoint::BeforeFinalEnumeration, snapshot.canonical_root, {});
        const auto final_root = Inspect(root, {});
        if (!final_root || !final_root->directory || fs::canonical(root, ec) != snapshot.canonical_root || ec) return Changed();
        const auto final_stamp = Inspect(snapshot.canonical_root, {});
        if (!final_stamp || *final_stamp != *root_stamp) return Changed();
        const auto final_entries = Enumerate(snapshot.canonical_root, limits);
        if (!final_entries || *final_entries != *entries) return Changed();
        snapshot.content_sha256 = platform::PackageLedgerFingerprintV1(ledger);
        Describe(snapshot);
        return snapshot;
    } catch (const fs::filesystem_error&) {
        return Fail("sdk.package.read_failed", "Package filesystem operation failed");
    }
}

} // namespace lubancode::package
