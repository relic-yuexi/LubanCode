// 脱敏与报告字段 allowlist(Token 账本单 §13/§15.6 A0 冻结)。
//
// 脱敏顺序(§13.2):canonical recorder pre-write redaction -> analyzer
// defensive redaction -> report field allowlist -> HTML escape -> 可选
// model-review allowlist。本件落中间两层:secret 扫描/替换与报告字段
// 白名单。命中 secret 的字段整个替成 [REDACTED:<kind>],不保留前后几位。
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lubancode::insights {

// secret 种类:扫描器认得出的类别;报告只落 kind,不落原文。
enum class SecretKind {
    AuthorizationHeader,  // "Authorization: Bearer …" 一行
    CookieHeader,         // "Cookie: …" 一行
    ApiKey,               // sk-… / ghp_… / AKIA… / x-api-key 一类
    PrivateKey,           // -----BEGIN … PRIVATE KEY----- 块头
    ConnectionString,     // scheme://user:pass@host 连接串
    AccessToken,          // "access_token"/"token" 后跟长串
    EnvAssignment,        // API_KEY=… / …TOKEN=… 环境变量赋值
    ContextToken,         // context token 一类会话凭据
};
const char* SecretKindName(SecretKind kind);

struct SecretHit {
    SecretKind kind = SecretKind::ApiKey;
    std::size_t offset = 0;
    std::size_t length = 0;  // 命中片段长度(替换范围)
};

// 扫一段文本里的 secret(只读)。重叠命中取最长。
std::vector<SecretHit> ScanSecrets(std::string_view text);

// 把命中的片段整个替成 "[REDACTED:<kind>]";没命中原样返回。
std::string RedactSecrets(std::string_view text);

// 报告字段 allowlist(§13.2 第三层):report JSON 的合法字段路径树。
// path 是逐级键名(数组下标不进 path,数组元素按同名字段裁)。报告
// schema 之外多出的字段一律不许过——防哪条旁路把正文带进报告。
bool IsAllowedReportFieldPath(const std::vector<std::string>& path);

}  // namespace lubancode::insights
