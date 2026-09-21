// 共享密钥扫描实现(FD-06 自 insights/redaction.cpp 原样搬来,算法
// 一字未动;搬动史与边界见 secret_scan.hpp 文件头)。
#include "privacy/secret_scan.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace lubancode::privacy {
namespace {

// 小写 ASCII 判定(避免 locale)。
bool IsLowerAscii(char c) {
    return c >= 'a' && c <= 'z';
}
char ToLowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
bool IsAlnum(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0 || IsLowerAscii(ToLowerAscii(c));
}
bool IsWordChar(char c) {
    return IsAlnum(c) || c == '_' || c == '-';
}
std::size_t FindCaseInsensitive(std::string_view haystack, std::string_view needle,
                                std::size_t from) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return std::string_view::npos;
    }
    for (std::size_t i = from; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (ToLowerAscii(haystack[i + j]) != ToLowerAscii(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return i;
        }
    }
    return std::string_view::npos;
}

// 命中行(到 '\n' 或串尾)。
std::size_t LineEnd(std::string_view text, std::size_t at) {
    const std::size_t newline = text.find('\n', at);
    return newline == std::string_view::npos ? text.size() : newline;
}

struct Rule {
    SecretKind kind;
    const char* marker;  // 大小写不敏感的锚点
    bool whole_line;     // 命中即盖整行(header 类)
    std::size_t min_tail;  // 锚点后至少还要这么长的尾巴
};

constexpr Rule kRules[] = {
    {SecretKind::AuthorizationHeader, "authorization:", true, 1},
    {SecretKind::CookieHeader, "cookie:", true, 1},
    {SecretKind::ContextToken, "context-token:", true, 1},
    {SecretKind::EnvAssignment, "api_key=", false, 1},
    {SecretKind::EnvAssignment, "apikey=", false, 1},
    {SecretKind::EnvAssignment, "secret=", false, 1},
    {SecretKind::EnvAssignment, "password=", false, 1},
    {SecretKind::EnvAssignment, "access_token=", false, 1},
    {SecretKind::EnvAssignment, "auth_token=", false, 1},
    {SecretKind::PrivateKey, "-----begin", false, 0},
};

// 长十六进制/base62 令牌前缀(sk-/ghp_/gho_/AKIA/xoxb-/xoxp-)。
struct PrefixRule {
    SecretKind kind;
    const char* prefix;
    std::size_t min_tail;
};
constexpr PrefixRule kPrefixRules[] = {
    {SecretKind::ApiKey, "sk-", 8},
    {SecretKind::ApiKey, "sk-ant-", 8},
    {SecretKind::ApiKey, "ghp_", 8},
    {SecretKind::ApiKey, "gho_", 8},
    {SecretKind::ApiKey, "AKIA", 12},
    {SecretKind::ApiKey, "xoxb-", 8},
    {SecretKind::ApiKey, "xoxp-", 8},
    {SecretKind::ApiKey, "r8_", 8},
};

}  // namespace

const char* SecretKindName(SecretKind kind) {
    switch (kind) {
        case SecretKind::AuthorizationHeader:
            return "authorization_header";
        case SecretKind::CookieHeader:
            return "cookie_header";
        case SecretKind::ApiKey:
            return "api_key";
        case SecretKind::PrivateKey:
            return "private_key";
        case SecretKind::ConnectionString:
            return "connection_string";
        case SecretKind::AccessToken:
            return "access_token";
        case SecretKind::EnvAssignment:
            return "env_assignment";
        case SecretKind::ContextToken:
            return "context_token";
    }
    return "";
}

std::vector<SecretHit> ScanSecrets(std::string_view text) {
    std::vector<SecretHit> hits;
    const auto overlaps = [&](std::size_t offset, std::size_t length) {
        for (const auto& hit : hits) {
            if (offset < hit.offset + hit.length && hit.offset < offset + length) {
                return true;
            }
        }
        return false;
    };
    const auto add = [&](SecretKind kind, std::size_t offset, std::size_t length) {
        if (length == 0 || offset >= text.size() || overlaps(offset, length)) {
            return;
        }
        hits.push_back(SecretHit{kind, offset, length});
    };

    for (const auto& rule : kRules) {
        std::size_t at = FindCaseInsensitive(text, rule.marker, 0);
        while (at != std::string_view::npos) {
            const std::size_t tail = at + std::string_view(rule.marker).size();
            if (rule.whole_line) {
                add(rule.kind, at, LineEnd(text, at) - at);
            } else if (text.size() > tail + rule.min_tail) {
                add(rule.kind, at, LineEnd(text, at) - at);
            }
            at = FindCaseInsensitive(text, rule.marker, at + 1);
        }
    }
    for (const auto& rule : kPrefixRules) {
        const std::string_view prefix(rule.prefix);
        std::size_t at = text.find(rule.prefix);
        while (at != std::string_view::npos) {
            std::size_t end = at + prefix.size();
            while (end < text.size() && IsWordChar(text[end])) {
                ++end;
            }
            if (end - at >= prefix.size() + rule.min_tail) {
                add(rule.kind, at, end - at);
            }
            at = text.find(rule.prefix, at + 1);
        }
    }
    // 连接串:scheme://user:pass@host(scheme 认 postgres/mysql/mongodb/
    // redis/amqp/ftp/postgresql)。行内见 '@' 才算带凭据;没 @ 的是裸
    // URL,不报。
    for (const char* scheme : {"postgres", "postgresql", "mysql", "mongodb", "redis", "amqp",
                               "ftp"}) {
        const std::string scheme_prefix = std::string(scheme) + "://";
        std::size_t at = FindCaseInsensitive(text, scheme_prefix, 0);
        while (at != std::string_view::npos) {
            const std::size_t scheme_end = at + std::string_view(scheme).size() + 3;
            const std::size_t at_sign = text.find('@', scheme_end);
            const std::size_t line_end = LineEnd(text, at);
            if (at_sign != std::string_view::npos && at_sign < line_end) {
                add(SecretKind::ConnectionString, at, line_end - at);
            }
            at = FindCaseInsensitive(text, scheme_prefix, at + 1);
        }
    }
    std::sort(hits.begin(), hits.end(), [](const SecretHit& a, const SecretHit& b) {
        return a.offset < b.offset;
    });
    return hits;
}

std::string RedactSecrets(std::string_view text) {
    const std::vector<SecretHit> hits = ScanSecrets(text);
    if (hits.empty()) {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size());
    std::size_t cursor = 0;
    for (const auto& hit : hits) {
        if (hit.offset < cursor) {
            continue;  // 已被更长命中盖掉
        }
        out.append(text.substr(cursor, hit.offset - cursor));
        out.append("[REDACTED:");
        out.append(SecretKindName(hit.kind));
        out.append("]");
        cursor = hit.offset + hit.length;
    }
    out.append(text.substr(cursor));
    return out;
}

}  // namespace lubancode::privacy
