// D1 allowlist-first Redactor 的实现。合同见 redactor.hpp 文件头。
#include "telemetry/redactor.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include "insights/redaction.hpp"

namespace lubancode::telemetry {
namespace {

// URL sanitizer:保留 scheme 与 host,打掉 userinfo 与 query/fragment
// (§10.1:API endpoint 里的 query/userinfo/secret 不发)。
std::string SanitizeUrl(std::string_view text) {
    const std::size_t scheme_end = text.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return std::string(text);
    }
    const std::size_t authority_begin = scheme_end + 3;
    const std::size_t path_begin = text.find_first_of("/?#", authority_begin);
    const std::size_t authority_end =
        path_begin == std::string_view::npos ? text.size() : path_begin;
    const std::string authority(text.substr(authority_begin, authority_end - authority_begin));
    // userinfo@host:port -> host:port(userinfo 是凭证,整段丢)。
    const std::size_t at = authority.find('@');
    const std::string host = at == std::string::npos ? authority : authority.substr(at + 1);
    return std::string(text.substr(0, authority_begin)) + host;
}

// 路径假名化(D1 = none 档:绝对路径整段替成占位符,不做相对化)。
// 不只认串首——错误文本里夹的路径一样是泄漏(§28.1)。认三类:
//   1) 盘符路径:X:\ 或 X:/ 出现在任意位置;
//   2) UNC:\\ 开头;
//   3) POSIX 绝对:/ 开头,或句中出现 /home /Users /root /tmp /var /mnt
//      /opt /srv /data 一类常见绝对根(裸 "/" 不认——D1 枚举值里
//      "read/write" 这种斜杠不算路径,误伤比漏伤伤得轻,但也不能瞎替)。
// 命中处起替到词边界(空白或串尾),整段占位。
constexpr std::string_view kPathPlaceholder = "[REDACTED:path]";

// 词边界:空白或串尾。
std::size_t TokenEnd(std::string_view text, std::size_t at) {
    const std::size_t space = text.find_first_of(" \t\r\n", at);
    return space == std::string_view::npos ? text.size() : space;
}

// 下一处疑似路径起点的候选位置(扫描用)。
std::size_t NextPathCandidate(std::string_view text, std::size_t from,
                              std::size_t* length) {
    for (std::size_t i = from; i < text.size(); ++i) {
        // 盘符路径:X:\ 或 X:/。盘符前必须是词首(非字母数字),免把
        // "https://" 的 scheme 尾字母当盘符。
        if (i + 2 < text.size() && text[i + 1] == ':' &&
            (text[i + 2] == '\\' || text[i + 2] == '/')) {
            const char drive = text[i] | 0x20;
            const bool word_start = i == 0 || !std::isalnum(static_cast<unsigned char>(text[i - 1])) ||
                                    text[i - 1] == ':';
            if (drive >= 'a' && drive <= 'z' && word_start) {
                *length = TokenEnd(text, i) - i;
                return i;
            }
        }
        // UNC。
        if (i + 1 < text.size() && text[i] == '\\' && text[i + 1] == '\\') {
            *length = TokenEnd(text, i) - i;
            return i;
        }
        // POSIX 常见绝对根:/home/ /Users/ /root/ /tmp/ /var/ /mnt/ /opt/
        // /srv/ /data/(句中出现才算;串首裸 '/' 在调用方整段判)。
        if (text[i] == '/') {
            for (const char* root : {"home/", "Users/", "root/", "tmp/", "var/",
                                     "mnt/", "opt/", "srv/", "data/"}) {
                const std::string_view candidate(root);
                if (text.substr(i + 1, candidate.size()) == candidate) {
                    *length = TokenEnd(text, i) - i;
                    return i;
                }
            }
        }
    }
    return std::string_view::npos;
}

std::string PseudonymizePaths(std::string_view text, bool* replaced) {
    std::string out;
    out.reserve(text.size());
    std::size_t cursor = 0;
    std::size_t length = 0;
    std::size_t at = NextPathCandidate(text, 0, &length);
    // 串首裸 POSIX 绝对路径:/ 开头整段替。
    if (!text.empty() && text.front() == '/') {
        at = 0;
        length = TokenEnd(text, 0);
    }
    while (at != std::string_view::npos) {
        out.append(text.substr(cursor, at - cursor));
        out.append(kPathPlaceholder);
        *replaced = true;
        cursor = at + length;
        at = NextPathCandidate(text, cursor, &length);
    }
    out.append(text.substr(cursor));
    return out;
}

}  // namespace

nlohmann::json RedactionManifest::ToJson() const {
    return nlohmann::json{{"policy_version", policy_version},
                          {"data_class", DataClassName(data_class)},
                          {"removed_fields", removed_fields},
                          {"truncated_fields", truncated_fields},
                          {"path_mode", path_mode},
                          {"content_included", content_included}};
}

std::string SanitizeText(std::string_view text, RedactionManifest* manifest) {
    // 1) secret key/name rules + token/credential detector:复用 insights
    //    层的既有扫描器,命中片段整段替 [REDACTED:<kind>]。
    std::string value = lubancode::insights::RedactSecrets(text);

    // 2) URL sanitizer:打掉 userinfo 与 query/fragment。
    value = SanitizeUrl(value);

    // 3) path pseudonymizer(D1 none 档:绝对路径整段占位,含句中夹的)。
    bool path_replaced = false;
    value = PseudonymizePaths(value, &path_replaced);
    if (path_replaced && manifest != nullptr) {
        manifest->removed_fields += 1;
    }

    // 4) content length cap。
    if (value.size() > kD1TextCap) {
        if (manifest != nullptr) {
            manifest->truncated_fields += 1;
        }
        value.resize(kD1TextCap);
        value += "[TRUNCATED]";
    }
    return value;
}

bool LooksLikeSecret(std::string_view text) {
    return !lubancode::insights::ScanSecrets(text).empty();
}

RedactionResult RedactAttributes(const nlohmann::json& attributes, DataClass data_class,
                                 AttributeDomain domain) {
    RedactionResult result;
    result.manifest.data_class = data_class;
    if (!attributes.is_object()) {
        // 结构不合:整包置空并计一笔(不猜字段级)。
        result.manifest.removed_fields = 1;
        return result;
    }
    nlohmann::json out = nlohmann::json::object();
    for (auto it = attributes.begin(); it != attributes.end(); ++it) {
        // 1) field allowlist:表外键整键删。
        const bool allowed = domain == AttributeDomain::Span
                                 ? IsAllowedSpanAttributeKey(it.key())
                                 : IsAllowedResourceAttributeKey(it.key());
        if (!allowed) {
            result.manifest.removed_fields += 1;
            continue;
        }
        const nlohmann::json& value = it.value();
        if (value.is_string()) {
            std::string sanitized = SanitizeText(value.get<std::string>(), &result.manifest);
            // 5) final denylist scan:allowlist 值里仍扫出 secret 形状,
            //    整值再替一遍(secret 命中替出的占位符不含原文,幂等)。
            if (LooksLikeSecret(sanitized)) {
                sanitized = "[REDACTED:apikey]";
                result.manifest.removed_fields += 1;
            }
            out.emplace(it.key(), std::move(sanitized));
            continue;
        }
        if (value.is_boolean() || value.is_number_integer() || value.is_number_unsigned()) {
            out.emplace(it.key(), value);
            continue;
        }
        // 数组/对象/浮点:span/resource 合同只产标量,结构不合按删计。
        result.manifest.removed_fields += 1;
    }
    result.attributes = std::move(out);
    return result;
}

}  // namespace lubancode::telemetry
