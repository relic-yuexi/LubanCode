#include "insights/redaction.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace lubancode::insights {
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

namespace {

// allowlist 路径树:叶子=true 即许可。数组元素不进 path,同名键同裁。
// 与 report_model/session_summary/finding 的 ToJson 一一对应;schema 之外
// 的字段不许出现。
bool WalkAllowlist(const std::vector<std::string>& path) {
    static const std::vector<std::vector<std::string>> kAllowed = {
        {"schema"},
        {"schema_version"},
        {"analyzer_version"},
        {"generated_at"},
        {"analysis_mode"},
        {"scope", "workspace_key"},
        {"scope", "since"},
        {"scope", "until"},
        {"scope", "all_workspaces"},
        {"coverage", "sessions_found"},
        {"coverage", "sessions_verified"},
        {"coverage", "sessions_analyzed"},
        {"coverage", "sessions_pending"},
        {"coverage", "sessions_excluded"},
        {"coverage", "runs_total"},
        {"coverage", "runs_analyzed"},
        {"coverage", "requests_total"},
        {"coverage", "requests_with_usage"},
        {"coverage", "outcomes_assessed"},
        {"usage", "requests_total"},
        {"usage", "requests_with_usage"},
        {"usage", "requests_unknown"},
        {"usage", "input_tokens"},
        {"usage", "cache_read_tokens"},
        {"usage", "cache_creation_tokens"},
        {"usage", "output_tokens"},
        {"usage", "reasoning_tokens"},
        {"usage", "cost"},
        {"usage", "cost", "status"},
        {"usage", "cost", "currency"},
        {"usage", "cost", "micros"},
        {"usage", "cost", "price_table_id"},
        {"source", "session_id"},
        {"source", "stream_terminal_hashes"},
        {"source", "integrity"},
        {"work", "turns"},
        {"work", "tool_calls"},
        {"work", "files_touched"},
        {"work", "verifications"},
        {"work", "outcome"},
        {"prompt_findings"},
        {"prompt_findings", "finding_id"},
        {"prompt_findings", "category"},
        {"prompt_findings", "severity"},
        {"prompt_findings", "confidence"},
        {"prompt_findings", "scope"},
        {"prompt_findings", "evidence"},
        {"prompt_findings", "evidence", "session_id"},
        {"prompt_findings", "evidence", "event_id"},
        {"prompt_findings", "evidence", "metric"},
        {"prompt_findings", "evidence", "value"},
        {"prompt_findings", "counter_evidence"},
        {"prompt_findings", "counter_evidence", "session_id"},
        {"prompt_findings", "counter_evidence", "event_id"},
        {"prompt_findings", "counter_evidence", "metric"},
        {"prompt_findings", "counter_evidence", "value"},
        {"prompt_findings", "summary"},
        {"prompt_findings", "recommendation"},
        {"prompt_findings", "origin"},
        {"prompt_findings", "rule_version"},
        {"friction_events"},
        {"feature_signals"},
        {"sessions"},
        {"sessions", "schema"},
        {"sessions", "schema_version"},
        {"sessions", "analyzer_version"},
        {"sessions", "source", "session_id"},
        {"sessions", "source", "stream_terminal_hashes"},
        {"sessions", "source", "integrity"},
        {"sessions", "coverage", "runs_total"},
        {"sessions", "coverage", "runs_analyzed"},
        {"sessions", "coverage", "requests_total"},
        {"sessions", "coverage", "requests_with_usage"},
        {"sessions", "coverage", "outcomes_assessed"},
        {"sessions", "work", "turns"},
        {"sessions", "work", "tool_calls"},
        {"sessions", "work", "files_touched"},
        {"sessions", "work", "verifications"},
        {"sessions", "work", "outcome"},
        {"sessions", "usage", "requests_total"},
        {"sessions", "usage", "requests_with_usage"},
        {"sessions", "usage", "input_tokens"},
        {"sessions", "usage", "cache_read_tokens"},
        {"sessions", "usage", "cache_creation_tokens"},
        {"sessions", "usage", "output_tokens"},
        {"sessions", "usage", "reasoning_tokens"},
        {"sessions", "usage", "cost", "status"},
        {"sessions", "usage", "cost", "currency"},
        {"sessions", "usage", "cost", "micros"},
        {"sessions", "usage", "cost", "price_table_id"},
        {"sessions", "prompt_findings", "finding_id"},
        {"sessions", "prompt_findings", "category"},
        {"sessions", "prompt_findings", "severity"},
        {"sessions", "prompt_findings", "confidence"},
        {"sessions", "prompt_findings", "scope"},
        {"sessions", "prompt_findings", "evidence", "session_id"},
        {"sessions", "prompt_findings", "evidence", "event_id"},
        {"sessions", "prompt_findings", "evidence", "metric"},
        {"sessions", "prompt_findings", "evidence", "value"},
        {"sessions", "prompt_findings", "counter_evidence", "session_id"},
        {"sessions", "prompt_findings", "counter_evidence", "event_id"},
        {"sessions", "prompt_findings", "counter_evidence", "metric"},
        {"sessions", "prompt_findings", "counter_evidence", "value"},
        {"sessions", "prompt_findings", "summary"},
        {"sessions", "prompt_findings", "recommendation"},
        {"sessions", "prompt_findings", "origin"},
        {"sessions", "prompt_findings", "rule_version"},
        {"sessions", "friction_events"},
        {"sessions", "feature_signals"},
        {"findings"},
        {"findings", "finding_id"},
        {"findings", "category"},
        {"findings", "severity"},
        {"findings", "confidence"},
        {"findings", "scope"},
        {"findings", "evidence"},
        {"findings", "evidence", "session_id"},
        {"findings", "evidence", "event_id"},
        {"findings", "evidence", "metric"},
        {"findings", "evidence", "value"},
        {"findings", "counter_evidence"},
        {"findings", "counter_evidence", "session_id"},
        {"findings", "counter_evidence", "event_id"},
        {"findings", "counter_evidence", "metric"},
        {"findings", "counter_evidence", "value"},
        {"findings", "summary"},
        {"findings", "recommendation"},
        {"findings", "origin"},
        {"findings", "rule_version"},
    };
    for (const auto& allowed : kAllowed) {
        if (allowed == path) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool IsAllowedReportFieldPath(const std::vector<std::string>& path) {
    return WalkAllowlist(path);
}

}  // namespace lubancode::insights
