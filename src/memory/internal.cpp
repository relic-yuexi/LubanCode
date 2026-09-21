// internal.hpp 的实现。SV-09 拆分前这些小件住 project_memory.cpp 的匿名
// 命名空间;拆分后多个 TU 共用,搬到这里保持一份写真源(逻辑一字未动)。

#include "memory/internal.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "platform/atomic_write.hpp"    // 统一原子写(审计 P1)
#include "platform/text_encoding.hpp"  // Utf8PrefixBoundary:OneLine 截断
#include "platform/wall_clock.hpp"     // JobStamp 的毫秒钟

namespace lubancode::memory {

namespace {

std::atomic<unsigned long long> g_sequence{0};

}  // namespace

std::filesystem::path Utf8Path(const std::string& utf8) {
    const std::u8string_view value(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(value);
}

std::string PathUtf8(const std::filesystem::path& path) {
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::filesystem::path AbsoluteNormal(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
    }
    std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, ec);
    return (ec ? absolute : canonical).lexically_normal();
}

std::string Trim(std::string value) {
    const auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t begin = 0;
    while (begin < value.size() && whitespace(value[begin])) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && whitespace(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

std::string ReadBounded(const std::filesystem::path& path, std::size_t max_bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open() || max_bytes == 0) {
        return {};
    }
    std::string out(max_bytes, '\0');
    file.read(out.data(), static_cast<std::streamsize>(max_bytes));
    out.resize(static_cast<std::size_t>(file.gcount()));
    return out;
}

std::string LowerAscii(std::string value) {
    for (char& c : value) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x80) {
            c = static_cast<char>(std::tolower(byte));
        }
    }
    return value;
}

std::uint64_t StableHash(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string HexHash(std::string_view value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << StableHash(value);
    return out.str();
}

std::string NowIsoUtc() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::string JobStamp() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(millis) + "-" + std::to_string(g_sequence.fetch_add(1));
}

bool IsValidId(const std::string& id) {
    if (id.empty() || id.size() > 120 || id.front() == '.' || id.back() == '.') {
        return false;
    }
    for (const unsigned char c : id) {
        if (std::isalnum(c) == 0 && c != '.' && c != '-' && c != '_') {
            return false;
        }
    }
    return id.starts_with("fact.") || id.starts_with("preference.") || id.starts_with("feedback.");
}

bool IsSafeRelativePath(const std::string& raw) {
    if (raw.empty()) {
        return false;
    }
    const std::filesystem::path path = Utf8Path(raw);
    if (path.is_absolute() || path.has_root_name()) {
        return false;
    }
    const std::filesystem::path normal = path.lexically_normal();
    if (normal.empty() || normal == ".") {
        return false;
    }
    for (const auto& part : normal) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

std::string OneLine(std::string value, std::size_t max_bytes) {
    for (char& c : value) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    value = Trim(std::move(value));
    if (value.size() > max_bytes) {
        value.resize(lubancode::platform::Utf8PrefixBoundary(value, max_bytes));
        value += "...";
    }
    return value;
}

std::expected<void, std::string> AtomicWrite(const std::filesystem::path& target,
                                             const std::string& content) {
    // 统一原子写(审计 P1):唯一临时名与平台原子替换归 platform 件,替掉
    // 本处自备的 ".tmp-JobStamp" 协议(合同不变:失败清临时件、不动正式件)。
    const auto written = platform::AtomicWriteFile(target, content);
    if (!written.has_value()) {
        return std::unexpected(written.error().message);
    }
    return {};
}

bool IsWithin(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const std::filesystem::path normalized_child = AbsoluteNormal(child);
    const std::filesystem::path normalized_parent = AbsoluteNormal(parent);
    auto child_it = normalized_child.begin();
    for (auto parent_it = normalized_parent.begin(); parent_it != normalized_parent.end();
         ++parent_it, ++child_it) {
        if (child_it == normalized_child.end() || *child_it != *parent_it) return false;
    }
    return true;
}

}  // namespace lubancode::memory
