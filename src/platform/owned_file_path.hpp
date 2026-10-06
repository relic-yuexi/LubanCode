#pragma once

#include <filesystem>
#include <system_error>

#include "platform/paths.hpp"
#include "platform/secure_file.hpp"

namespace lubancode::platform {

// The host keeps ancestors above the owned directory stable. Check that
// directory itself and every descendant without resolving away any link.
// This is a path check, not an adversarial-parent race fence or OS sandbox.
inline bool IsUnlinkedOwnedPath(const std::filesystem::path& owned_directory,
    const std::filesystem::path& requested_path) {
    namespace fs = std::filesystem;
    if (owned_directory.empty() || requested_path.empty()) return false;
    std::error_code error;
    auto root = fs::absolute(owned_directory, error);
    if (error) return false;
    while (root != root.root_path() && (root.filename().empty() || root.filename() == "."))
        root = root.parent_path();
    auto path = fs::absolute(requested_path, error);
    if (error) return false;
    while (path != path.root_path() && path.filename().empty()) path = path.parent_path();

    // Compare components before walking upward. An escape must not make us
    // inspect outside the owned boundary or pass after lexical normalization.
    auto root_part = root.begin();
    auto path_part = path.begin();
    for (; root_part != root.end(); ++root_part, ++path_part)
        if (path_part == path.end() || *root_part != *path_part) return false;
    for (; path_part != path.end(); ++path_part)
        if (*path_part == "..") return false;

    for (;;) {
        if (!RejectReparsePoint(FileIoPath(path))) return false;
        if (path == root) return true;
        const auto parent = path.parent_path();
        if (parent.empty() || parent == path) return false;
        path = parent;
    }
}

} // namespace lubancode::platform
