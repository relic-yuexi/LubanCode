// 敏感键词表唯一来源(SV-12:架构审查 2026-09-21)。
//
// 同一组凭据键词,过去在三处各养一份:skills::RedactSecrets 的文本键
// 前缀匹配、SanitizeToolInput 的 JSON 键包含匹配、evolution::
// ScanTextForSecrets 的候选扫描。添一类凭据键要改三处,漏一处,"录制
// 先打码、候选再拒绝明文"的覆盖集合就漂移。这里只收"哪些键算敏感"
// 这一件事实,零业务依赖;排列按长度降序冻结——前缀匹配方首个命中即
// 最长("client_secret" 压过 "secret"),包含匹配方与顺序无关。
//
// 边界(只共事实,不共策略):
//   - 各入口的判定策略原地不动:JSON 键包含匹配留 skills 的
//     ContainsAnyKeyword,文本键前缀+词边界匹配留 skills::RedactSecrets
//     与 evolution::ScanTextForSecrets,占位值豁免留 eval——词表合一
//     不抹平策略差异;
//   - 凭据"模式"扫描(锚点/前缀/连接串)是另一件事,归
//     privacy/secret_scan(FD-06),与这张键词表互不收编。
#pragma once

#include <string_view>

namespace lubancode::privacy {

inline constexpr std::string_view kSensitiveKeyWords[] = {
    "authorization", "client_secret", "private_key", "access_key", "session_key",
    "api_key",       "apikey",        "password",    "passwd",     "cookie",
    "token",         "secret",
};

}  // namespace lubancode::privacy
