// SemVer 解析与比较的实现(统一 Package 封装单阶段 1)。手写逐字符扫描:
// 语法就这么几十行,不值得引库;真正的价值在比较语义与 semver.org 对齐,
// 单测逐条钉。
#include "package/semver.hpp"

#include <cctype>

namespace lubancode::package {

namespace {

std::string_view TrimSpace(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool IsDigits(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// SemVer 数字段:非空、全数字、无前导零(除非就是 "0")、装得进 int64。
std::optional<std::int64_t> ParseNumeric(std::string_view s) {
    if (!IsDigits(s)) return std::nullopt;
    if (s.size() > 1 && s[0] == '0') return std::nullopt;  // "01" 不许
    if (s.size() > 18) return std::nullopt;  // int64 余量,免溢出判断分支
    std::int64_t value = 0;
    for (const char c : s) {
        value = value * 10 + (c - '0');
    }
    return value;
}

// 预发布/构建段的字符集:字母数字与连字符(点是分隔符)。
bool IsIdentifierChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

// 点分段(预发布/构建):每段非空、字符是字母数字连字符;纯数字段的前导零
// 只在预发布里禁止(semver.org:build 的数字标识符允许前导零,如 "+001")。
bool IsValidDotSeparated(std::string_view s, bool allow_leading_zero) {
    if (s.empty()) return false;
    std::size_t begin = 0;
    while (begin <= s.size()) {
        const std::size_t dot = s.find('.', begin);
        const std::size_t end = dot == std::string_view::npos ? s.size() : dot;
        const std::string_view part = s.substr(begin, end - begin);
        if (part.empty()) return false;
        for (const char c : part) {
            if (!IsIdentifierChar(c)) return false;
        }
        if (!allow_leading_zero && IsDigits(part) && part.size() > 1 && part[0] == '0') {
            return false;
        }
        if (dot == std::string_view::npos) break;
        begin = dot + 1;
    }
    return true;
}

}  // namespace

std::optional<SemVer> ParseSemVer(std::string_view raw) {
    const std::string_view text = TrimSpace(raw);
    if (text.empty()) return std::nullopt;

    SemVer out;
    out.text = std::string(text);

    // 主.次.补丁 三段按点切,补丁段到 '-'/'+' 为止。前两段后面直接跟
    // '-'/'+'("1.2-rc"、"1-x")是非法形状,按解析失败退。
    const std::size_t dot1 = text.find('.');
    if (dot1 == std::string_view::npos) return std::nullopt;
    const auto major = ParseNumeric(text.substr(0, dot1));
    if (!major.has_value()) return std::nullopt;

    const std::size_t dot2 = text.find('.', dot1 + 1);
    if (dot2 == std::string_view::npos) return std::nullopt;
    const auto minor = ParseNumeric(text.substr(dot1 + 1, dot2 - dot1 - 1));
    if (!minor.has_value()) return std::nullopt;

    const std::size_t patch_end = text.find_first_of("-+", dot2 + 1);
    const std::size_t patch_stop = patch_end == std::string_view::npos ? text.size() : patch_end;
    const auto patch = ParseNumeric(text.substr(dot2 + 1, patch_stop - dot2 - 1));
    if (!patch.has_value()) return std::nullopt;
    out.major = *major;
    out.minor = *minor;
    out.patch = *patch;

    if (patch_end == std::string_view::npos) {
        return out;
    }
    const char tail_kind = text[patch_end];
    std::size_t tail_begin = patch_end + 1;
    std::size_t tail_end = text.size();
    if (tail_kind == '+') {
        // 只有构建段:后面不许再有 '+'。
        if (text.find('+', tail_begin) != std::string_view::npos) return std::nullopt;
    } else {
        // 预发布段:到 '+' 为止;构建段(若有)在其后。
        const std::size_t plus = text.find('+', tail_begin);
        if (plus != std::string_view::npos) {
            tail_end = plus;
            if (text.find('+', plus + 1) != std::string_view::npos) return std::nullopt;
            out.build = std::string(text.substr(plus + 1));
        }
    }
    if (tail_kind == '+') {
        out.build = std::string(text.substr(tail_begin));
        if (!IsValidDotSeparated(out.build, /*allow_leading_zero=*/true)) return std::nullopt;
    } else {
        out.prerelease = std::string(text.substr(tail_begin, tail_end - tail_begin));
        if (!IsValidDotSeparated(out.prerelease, /*allow_leading_zero=*/false)) {
            return std::nullopt;
        }
        if (!out.build.empty() && !IsValidDotSeparated(out.build, /*allow_leading_zero=*/true)) {
            return std::nullopt;
        }
    }
    return out;
}

int CompareSemVer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;

    // 预发布:正式版 > 预发布版;两边都预发布再逐段比。
    if (a.prerelease.empty() != b.prerelease.empty()) {
        return a.prerelease.empty() ? 1 : -1;
    }
    if (a.prerelease.empty()) return 0;

    std::size_t pa = 0;
    std::size_t pb = 0;
    while (pa < a.prerelease.size() || pb < b.prerelease.size()) {
        // 一边先到头:短的小(1.0.0-alpha < 1.0.0-alpha.1)。
        if (pa >= a.prerelease.size()) return -1;
        if (pb >= b.prerelease.size()) return 1;

        std::size_t ea = a.prerelease.find('.', pa);
        std::size_t eb = b.prerelease.find('.', pb);
        if (ea == std::string::npos) ea = a.prerelease.size();
        if (eb == std::string::npos) eb = b.prerelease.size();
        const std::string_view sa(a.prerelease.data() + pa, ea - pa);
        const std::string_view sb(b.prerelease.data() + pb, eb - pb);

        const bool digit_a = IsDigits(sa);
        const bool digit_b = IsDigits(sb);
        if (digit_a != digit_b) {
            return digit_a ? -1 : 1;  // 数字标识符低于字母标识符
        }
        if (digit_a && digit_b) {
            std::int64_t va = 0;
            std::int64_t vb = 0;
            for (const char c : sa) va = va * 10 + (c - '0');
            for (const char c : sb) vb = vb * 10 + (c - '0');
            if (va != vb) return va < vb ? -1 : 1;
        } else {
            const int cmp = sa.compare(sb);
            if (cmp != 0) return cmp < 0 ? -1 : 1;
        }
        pa = ea == a.prerelease.size() ? a.prerelease.size() : ea + 1;
        pb = eb == b.prerelease.size() ? b.prerelease.size() : eb + 1;
    }
    return 0;
}

std::optional<VersionRange> ParseVersionRange(std::string_view raw) {
    VersionRange out;
    out.text = std::string(TrimSpace(raw));
    if (out.text.empty()) return std::nullopt;

    std::size_t pos = 0;
    while (pos < out.text.size()) {
        while (pos < out.text.size() && std::isspace(static_cast<unsigned char>(out.text[pos])) != 0) {
            ++pos;
        }
        if (pos >= out.text.size()) break;
        std::size_t end = pos;
        while (end < out.text.size() && std::isspace(static_cast<unsigned char>(out.text[end])) == 0) {
            ++end;
        }
        const std::string_view token(out.text.data() + pos, end - pos);
        pos = end;

        VersionComparator part;
        std::string_view number = token;
        if (token.starts_with(">=")) {
            part.op = VersionComparator::Op::Ge;
            number = token.substr(2);
        } else if (token.starts_with("<=")) {
            part.op = VersionComparator::Op::Le;
            number = token.substr(2);
        } else if (token.starts_with('>')) {
            part.op = VersionComparator::Op::Gt;
            number = token.substr(1);
        } else if (token.starts_with('<')) {
            part.op = VersionComparator::Op::Lt;
            number = token.substr(1);
        } else if (token.starts_with('=')) {
            part.op = VersionComparator::Op::Eq;
            number = token.substr(1);
        } else {
            part.op = VersionComparator::Op::Eq;  // 裸版本号 = 精确匹配
        }
        const auto version = ParseSemVer(number);
        if (!version.has_value()) return std::nullopt;
        part.version = *version;
        out.parts.push_back(std::move(part));
    }
    if (out.parts.empty()) return std::nullopt;
    return out;
}

bool VersionSatisfies(const SemVer& version, const VersionRange& range) {
    for (const auto& part : range.parts) {
        const int cmp = CompareSemVer(version, part.version);
        const bool ok = part.op == VersionComparator::Op::Ge ? cmp >= 0
                       : part.op == VersionComparator::Op::Gt ? cmp > 0
                       : part.op == VersionComparator::Op::Le ? cmp <= 0
                       : part.op == VersionComparator::Op::Lt ? cmp < 0
                                                              : cmp == 0;
        if (!ok) return false;
    }
    return true;
}

}  // namespace lubancode::package
