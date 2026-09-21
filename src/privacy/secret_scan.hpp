// 共享密钥扫描(FD-06:自 insights/redaction.cpp 下沉的中立件)。
//
// 这里只放凭据检测与替换的纯算法,零业务依赖——insights(报告渲染前
// 防御性脱敏)、telemetry(D1 redactor 的 secret 道闸)、trajectory
// (导出件 privacy.secret.<kind> 稳定码)三家共用,谁也不许反向被
// include。算法自 insights 冻结的模式表(Token 账本单 §13/§15.6 A0)
// 原样搬来,下沉不改行为——命中范围、类型名与替换字节逐字节不动,
// 合同见 tests/unit/privacy/test_secret_scan_contract.cpp(迁移前后同
// 语料同输出)。
//
// 边界(检测与处置分家,不做万能 Redactor):
//   - 报告字段允许表留 insights(IsAllowedReportFieldPath);
//   - URL/路径假名化与文本长度帽留 telemetry;
//   - 导出处置策略(丢行还是替换、findings 记什么码)留 trajectory;
//   - skills::RedactSecrets(workflow_recorder 一系)的敏感键词表统一
//     归 SV-12,本件不收编、不改词。
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lubancode::privacy {

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

}  // namespace lubancode::privacy
