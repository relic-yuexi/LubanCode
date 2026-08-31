// D1 allowlist-first Redactor(端云协同可观测架构与 Telemetry 插件设计单
// §15.2/§15.3,实施分期 T0"Redactor D1 allowlist 与泄漏测试")。
//
// 脱敏顺序照 §15.2 钉死,不许倒:
//   field allowlist -> secret key/name rules -> URL sanitizer
//   -> path pseudonymizer -> token/credential detector -> content length cap
//   -> final denylist scan -> redaction manifest
//
// 先 allowlist 后 denylist:只靠正则找 secret 不够(§15.2)。secret 扫描
// 与打码复用 insights 层的既有扫描器(Token 账本单 A0 冻结的那套),不在
// telemetry 里重造第二份模式表。
//
// T0 只落 D1(metadata):任何字符串值过完管道后不含 secret、不含绝对路
// 径、URL 只剩 scheme+host、超长截断。D2/D3 的门(consent/window/scope)
// 属于 T2 之后的 exporter/service 层,本件只认 data_class 并如实记账。
#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "telemetry/contract.hpp"

namespace lubancode::telemetry {

// 每批带的脱敏账(§15.3)。不带被删内容的 hash——不做字典反查的口粮。
struct RedactionManifest {
    std::string policy_version{kRedactionPolicyVersion};
    DataClass data_class = DataClass::Metadata;
    int removed_fields = 0;      // allowlist 掉的键 + 结构不合掉的值
    int truncated_fields = 0;    // 超长截断的字符串
    std::string path_mode = "none";  // none|workspace_relative_hash;D1 无路径
    bool content_included = false;   // D1 恒 false

    nlohmann::json ToJson() const;
};

struct RedactionResult {
    nlohmann::json attributes = nlohmann::json::object();
    RedactionManifest manifest;
};

// 键集:span attribute 走 contract 的封闭白名单;resource attribute 走
// resource 白名单。表外键整键删除并计数(manifest.removed_fields)。
// 字符串值过 §15.2 管道;布尔/整数原样过(它们装不下正文)。
enum class AttributeDomain { Span, Resource };
RedactionResult RedactAttributes(const nlohmann::json& attributes, DataClass data_class,
                                 AttributeDomain domain);

// 单值文本管道(§15.2 第 2-6 步):secret 打码 -> URL 只留 scheme+host
// -> 绝对路径打码 -> 长度帽。manifest 可空(单测直调时不记账)。
std::string SanitizeText(std::string_view text, RedactionManifest* manifest);

// 单条字符串是否还含可疑 secret(末道 denylist 扫描的公开口,测试与
// doctor 复用;命中返回 true)。
bool LooksLikeSecret(std::string_view text);

// 字符串长度帽(D1 元数据不需要长文本;错误全文走 D2)。
inline constexpr std::size_t kD1TextCap = 256;

}  // namespace lubancode::telemetry
