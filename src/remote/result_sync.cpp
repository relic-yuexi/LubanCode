#include "remote/result_sync.hpp"

#include <algorithm>
#include <utility>

#include "platform/text_encoding.hpp"
#include "privacy/secret_scan.hpp"
#include "runtime/secret_resolver.hpp"

namespace lubancode::remote {
namespace {

bool ValidIdentity(std::string_view value) {
    if (value.empty() || value.size() > 128) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == ':';
    });
}

bool SensitiveIdentity(std::string_view value, const runtime::SecretRedactor& secrets) {
    return secrets.Redact(value) != value || !privacy::ScanSecrets(value).empty();
}

bool HasBinaryControls(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char ch) {
        return (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') || ch == 0x7f;
    });
}

std::string RedactResult(std::string_view text, const runtime::SecretRedactor& secrets) {
    // 共享扫描器的 PrivateKey 命中只覆盖头行。远端出口不能只藏 PEM
    // 头而放出后续密钥正文；这一类整份结果遮掉，不另造 PEM 解码器。
    const auto hits = privacy::ScanSecrets(text);
    if (std::any_of(hits.begin(), hits.end(), [](const privacy::SecretHit& hit) {
            return hit.kind == privacy::SecretKind::PrivateKey;
        })) {
        return "[REDACTED:private_key]";
    }
    const std::string known_redacted = secrets.Redact(text);
    if (!hits.empty() && known_redacted != text) {
        // 两种规则都命中时不能串行替换：先盖已知值可能剪碎模式密钥，
        // 先盖模式又可能剪碎更长的已知值。现有 Redactor 不暴露原始
        // 命中区间，故此处保守遮掉整份文本，不把残余凭据送出去。
        return "[REDACTED]";
    }
    return hits.empty() ? known_redacted : privacy::RedactSecrets(text);
}

}  // namespace

std::string_view ResultSyncErrorCode(ResultSyncError error) {
    switch (error) {
        case ResultSyncError::InvalidConfig: return "result_sync_invalid_config";
        case ResultSyncError::InvalidMode: return "result_sync_invalid_mode";
        case ResultSyncError::InvalidPreviewLimit: return "result_sync_invalid_preview_limit";
        case ResultSyncError::InvalidIdentity: return "result_sync_invalid_identity";
        case ResultSyncError::SensitiveIdentity: return "result_sync_sensitive_identity";
        case ResultSyncError::InvalidSource: return "result_sync_invalid_source";
        case ResultSyncError::ResultMissing: return "result_unavailable";
        case ResultSyncError::ResultIncomplete: return "result_capture_incomplete";
        case ResultSyncError::ResultTooLarge: return "result_too_large";
        case ResultSyncError::ResultNotText: return "result_not_text";
        case ResultSyncError::InvalidUtf8: return "result_invalid_utf8";
        case ResultSyncError::FullSyncDisabled: return "full_result_sync_disabled";
        case ResultSyncError::PolicyRestricted: return "result_sync_policy_restricted";
        case ResultSyncError::PolicyChanged: return "result_sync_policy_changed";
        case ResultSyncError::RedactionChanged: return "result_sync_redaction_changed";
    }
    return "result_sync_invalid_error";
}

NodeResultSyncPolicy::NodeResultSyncPolicy(ResultSyncMode mode, std::size_t preview_max_bytes,
                                         std::string version)
    : mode_(mode), preview_max_bytes_(preview_max_bytes), version_(std::move(version)) {}

std::expected<NodeResultSyncPolicy, ResultSyncError> ParseNodeResultSyncPolicy(
    const nlohmann::json& node_config, std::string policy_version) {
    if (!node_config.is_object()) {
        return std::unexpected(ResultSyncError::InvalidConfig);
    }
    for (auto it = node_config.begin(); it != node_config.end(); ++it) {
        if (it.key() != "tool_result_sync" && it.key() != "preview_max_bytes") {
            return std::unexpected(ResultSyncError::InvalidConfig);
        }
    }
    if (!ValidIdentity(policy_version)) {
        return std::unexpected(ResultSyncError::InvalidIdentity);
    }
    ResultSyncMode mode = ResultSyncMode::Preview;
    if (const auto it = node_config.find("tool_result_sync"); it != node_config.end()) {
        if (!it->is_string()) {
            return std::unexpected(ResultSyncError::InvalidMode);
        }
        const auto& value = it->get_ref<const std::string&>();
        if (value == "full") {
            mode = ResultSyncMode::Full;
        } else if (value != "preview") {
            return std::unexpected(ResultSyncError::InvalidMode);
        }
    }
    std::size_t preview_bytes = kDefaultResultPreviewBytes;
    if (const auto it = node_config.find("preview_max_bytes"); it != node_config.end()) {
        if (!it->is_number_integer() || *it < 1 || *it > kMaxResultPreviewBytes) {
            return std::unexpected(ResultSyncError::InvalidPreviewLimit);
        }
        preview_bytes = it->get<std::size_t>();
    }
    return NodeResultSyncPolicy(mode, preview_bytes, std::move(policy_version));
}

FrozenToolResult::FrozenToolResult(nlohmann::json payload, ResultSyncMode mode)
    : payload_(std::move(payload)), mode_(mode) {}

std::expected<nlohmann::json, ResultSyncError> FrozenToolResult::ForTransmission(
    const NodeResultSyncPolicy& current_policy,
    const runtime::SecretRedactor& current_secrets) const {
    if (mode_ == ResultSyncMode::Full && current_policy.mode() != ResultSyncMode::Full) {
        return std::unexpected(ResultSyncError::FullSyncDisabled);
    }
    if (payload_["policyVersion"] != current_policy.version()) {
        return std::unexpected(ResultSyncError::PolicyChanged);
    }
    if (payload_.contains("text")) {
        const auto& text = payload_["text"].get_ref<const std::string&>();
        if (mode_ == ResultSyncMode::Preview && text.size() > current_policy.preview_max_bytes()) {
            return std::unexpected(ResultSyncError::PolicyRestricted);
        }
        // 不重投影旧记录。新秘密命中时要求上层显式换代/重置待发记录，
        // 否则相同 uiSeq 会装两份正文。
        if (RedactResult(text, current_secrets) != text) {
            return std::unexpected(ResultSyncError::RedactionChanged);
        }
    }
    for (const char* key : {"sessionId", "toolCallId", "resultId", "policyVersion"}) {
        if (SensitiveIdentity(payload_[key].get_ref<const std::string&>(), current_secrets)) {
            return std::unexpected(ResultSyncError::SensitiveIdentity);
        }
    }
    return payload_;
}

std::expected<FrozenToolResult, ResultSyncError> ProjectSavedToolResult(
    const NodeResultSyncPolicy& policy, const ResultSyncIdentity& identity,
    const SavedToolResult& saved_result, const runtime::SecretRedactor& known_secrets) {
    for (const std::string* value : {&identity.session_id, &identity.tool_call_id,
                                    &identity.result_id, &policy.version()}) {
        if (!ValidIdentity(*value)) {
            return std::unexpected(ResultSyncError::InvalidIdentity);
        }
        if (SensitiveIdentity(*value, known_secrets)) {
            return std::unexpected(ResultSyncError::SensitiveIdentity);
        }
    }
    if (!saved_result.available) {
        return std::unexpected(ResultSyncError::ResultMissing);
    }
    if (saved_result.kind != ResultContentKind::Text &&
        saved_result.kind != ResultContentKind::Binary) {
        return std::unexpected(ResultSyncError::InvalidSource);
    }
    if (policy.mode() == ResultSyncMode::Full && !saved_result.capture_complete) {
        return std::unexpected(ResultSyncError::ResultIncomplete);
    }
    nlohmann::json payload{
        {"sessionId", identity.session_id}, {"toolCallId", identity.tool_call_id},
        {"resultId", identity.result_id}, {"policyVersion", policy.version()},
        {"mode", policy.mode() == ResultSyncMode::Full ? "full" : "preview"},
        {"contentKind", saved_result.kind == ResultContentKind::Text ? "text" : "binary"},
        {"captureComplete", saved_result.capture_complete},
        {"originalBytes", saved_result.original_bytes.has_value()
                              ? nlohmann::json(*saved_result.original_bytes) : nlohmann::json(nullptr)},
    };
    if (saved_result.kind == ResultContentKind::Binary) {
        if (policy.mode() == ResultSyncMode::Full) {
            return std::unexpected(ResultSyncError::ResultNotText);
        }
        payload["status"] = "metadata_only";
        payload["truncated"] = true;
        return FrozenToolResult(std::move(payload), policy.mode());
    }
    if (saved_result.text.size() > kMaxResultInputBytes) {
        return std::unexpected(ResultSyncError::ResultTooLarge);
    }
    if (saved_result.original_bytes.has_value() &&
        (*saved_result.original_bytes < saved_result.text.size() ||
         (saved_result.capture_complete && *saved_result.original_bytes != saved_result.text.size()))) {
        return std::unexpected(ResultSyncError::InvalidSource);
    }
    const std::string raw_text(saved_result.text);
    if (!platform::IsValidUtf8(raw_text)) {
        return std::unexpected(ResultSyncError::InvalidUtf8);
    }
    if (HasBinaryControls(raw_text)) {
        return std::unexpected(ResultSyncError::ResultNotText);
    }
    const std::string redacted = RedactResult(raw_text, known_secrets);
    // SecretRedactor 接受字节值；若某枚登记值切进多字节字符，不能把
    // 脱敏后产生的坏 UTF-8 交给 JSON 或用截断掩盖它。
    if (!platform::IsValidUtf8(redacted)) {
        return std::unexpected(ResultSyncError::InvalidUtf8);
    }
    if (policy.mode() == ResultSyncMode::Full && redacted.size() > kMaxFullResultBytes) {
        return std::unexpected(ResultSyncError::ResultTooLarge);
    }
    const bool preview_cut = policy.mode() == ResultSyncMode::Preview &&
                             redacted.size() > policy.preview_max_bytes();
    payload["status"] = "ready";
    payload["text"] = policy.mode() == ResultSyncMode::Preview
                          ? platform::TruncateUtf8Prefix(redacted, policy.preview_max_bytes()) : redacted;
    payload["truncated"] = preview_cut || !saved_result.capture_complete;
    payload["redacted"] = redacted != raw_text;
    if (saved_result.capture_complete && !saved_result.original_bytes.has_value()) {
        payload["originalBytes"] = raw_text.size();
    }
    return FrozenToolResult(std::move(payload), policy.mode());
}

}  // namespace lubancode::remote
