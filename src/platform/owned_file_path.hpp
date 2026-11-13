#pragma once

#include <array>
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
    fs::path current_base;
#ifdef _WIN32
    std::array<fs::path, 2> drive_names, drive_bases;
    std::size_t drive_count = 0;
#endif
    const auto resolve = [&](const fs::path& value) -> fs::path {
        // MSVC absolute() normalizes even already-absolute input. Keep every
        // caller component until the owned-domain check has inspected it.
        if (value.is_absolute()) return value;
        fs::path base;
#ifdef _WIN32
        if (value.has_root_name()) {
            const auto drive = value.root_name();
            std::size_t index = 0;
            for (; index < drive_count; ++index) if (drive_names[index] == drive) break;
            if (index == drive_count) {
                if (drive_count == drive_names.size()) return {};
                // C: denotes that drive's cwd, not its root. Resolve only the
                // drive name; never give the raw user tail to GetFullPathNameW.
                base = fs::absolute(drive, error);
                if (error || !base.is_absolute()) return {};
                drive_names[index] = drive; drive_bases[index] = base; ++drive_count;
            } else base = drive_bases[index];
        } else
#endif
        {
            if (current_base.empty()) {
                current_base = fs::current_path(error);
                if (error || !current_base.is_absolute()) return {};
            }
            base = current_base;
        }
        if (value.has_root_directory()) base = base.root_path();
        auto absolute = base / value.relative_path();
        return absolute.is_absolute() ? absolute : fs::path{};
    };
    auto root = resolve(owned_directory);
    if (root.empty()) return false;
    while (root != root.root_path() && (root.filename().empty() || root.filename() == "."))
        root = root.parent_path();
    auto path = resolve(requested_path);
    if (path.empty()) return false;
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
