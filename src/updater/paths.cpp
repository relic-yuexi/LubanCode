// 实现与合同注释见 paths.hpp(四方契约,口径出处 install_plan.py)。
#include "updater/paths.hpp"

#include <array>

namespace lubancode::updater {
namespace {

// install_plan.py L55-59 WINDOWS_RESERVED:CON/PRN/AUX/NUL + COM1-9/LPT1-9,
// 大小写不敏感,整段命中或 stem(第一个 '.' 前)命中都拒。
constexpr std::array<std::string_view, 22> kWindowsReserved = {
    "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5",
    "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
    "LPT6", "LPT7", "LPT8", "LPT9",
};

// seg 全 ASCII 大写化后是否等于保留名之一(python seg.upper() 的 ASCII 域)。
bool UpperEqualsReserved(std::string_view seg) {
    for (const std::string_view reserved : kWindowsReserved) {
        if (seg.size() != reserved.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t i = 0; i < seg.size(); ++i) {
            const char c = seg[i];
            const char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
            if (upper != reserved[i]) {
                same = false;
                break;
            }
        }
        if (same) {
            return true;
        }
    }
    return false;
}

// 保留名判定:整段大写化命中,或 stem(第一个 '.' 前)大写化命中
// (install_plan.py L83-85:com1.md 在 Windows 上照样惹祸)。
bool HitsWindowsReserved(std::string_view seg) {
    if (UpperEqualsReserved(seg)) {
        return true;
    }
    const std::size_t dot = seg.find('.');
    if (dot != std::string_view::npos) {
        return UpperEqualsReserved(seg.substr(0, dot));
    }
    return false;
}

// install_plan.py L60 INVALID_CHARS = <>:"|?*;控制字符(ord < 0x20)一并拒
// (generate_manifest.py L77-79)。非 ASCII 字节(unsigned >= 0x80)放行,
// 与 python 的 codepoint 口径一致。
bool IsInvalidChar(unsigned char c) {
    return c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' ||
           c == '?' || c == '*';
}

}  // namespace

bool ValidRelpath(std::string_view path) {
    if (path.empty() || path.size() > 512) {
        return false;
    }
    if (path.front() == '/' || path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const std::size_t end = path.find('/', pos);
        const std::size_t seg_end = (end == std::string_view::npos) ? path.size() : end;
        const std::string_view seg = path.substr(pos, seg_end - pos);
        if (seg.empty() || seg == "." || seg == "..") {
            return false;
        }
        if (seg.back() == '.' || seg.back() == ' ') {
            return false;
        }
        if (HitsWindowsReserved(seg)) {
            return false;
        }
        for (const char ch : seg) {
            if (IsInvalidChar(static_cast<unsigned char>(ch))) {
                return false;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        pos = end + 1;
    }
    return true;
}

std::string FoldKey(std::string_view path) {
    std::string key(path);
    if (kCaseFoldingActive) {
        for (char& c : key) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
    }
    return key;
}

std::optional<std::string> NormalizeMemberName(std::string_view name) {
    std::string normalized(name);
    for (char& c : normalized) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (normalized.starts_with('/')) {
        return std::nullopt;
    }
    if (normalized.size() > 512) {
        return std::nullopt;
    }
    std::string rebuilt;
    rebuilt.reserve(normalized.size());
    std::size_t pos = 0;
    while (pos <= normalized.size()) {
        const std::size_t end = normalized.find('/', pos);
        const std::size_t seg_end = (end == std::string::npos) ? normalized.size() : end;
        const std::string_view seg(normalized.data() + pos, seg_end - pos);
        if (!seg.empty() && seg != ".") {
            if (seg == "..") {
                return std::nullopt;
            }
            if (!rebuilt.empty()) {
                rebuilt += '/';
            }
            rebuilt.append(seg.data(), seg.size());
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    if (rebuilt.empty()) {
        return std::nullopt;
    }
    if (!ValidRelpath(rebuilt)) {
        return std::nullopt;
    }
    return rebuilt;
}

bool IsUserDataPath(std::string_view rel) {
    if (rel == "config.toml" || rel == ".env") {
        return true;
    }
    if (rel == ".lubancode" || rel.starts_with(".lubancode/")) {
        return true;
    }
    if (rel == ".agents" || rel.starts_with(".agents/")) {
        return true;
    }
    return false;
}

}  // namespace lubancode::updater
