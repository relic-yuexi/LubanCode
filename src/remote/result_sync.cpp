#include "remote/result_sync.hpp"

#include <algorithm>
#include <utility>

#include "platform/text_encoding.hpp"
#include "platform/sha256.hpp"
#include <set>
#include "privacy/secret_scan.hpp"
#include "runtime/secret_resolver.hpp"

namespace lubancode::remote {
namespace {

bool ValidIdentity(std::string_view value) {
    if (value.empty() || value.size() > 200) {
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
        case ResultSyncError::SessionMismatch: return "result_sync_session_mismatch";
        case ResultSyncError::InvalidFrozenRecord: return "result_sync_invalid_frozen_record";
    }
    return "result_sync_invalid_error";
}

NodeResultSyncPolicy::NodeResultSyncPolicy(bool allow_full, std::size_t preview_max_bytes,
                                         std::string version)
    : allow_full_(allow_full), preview_max_bytes_(preview_max_bytes), version_(std::move(version)) {}

std::expected<NodeResultSyncPolicy, ResultSyncError> ParseNodeResultSyncPolicy(
    const nlohmann::json& node_config, std::string policy_version) {
    if (!node_config.is_object()) {
        return std::unexpected(ResultSyncError::InvalidConfig);
    }
    for (auto it = node_config.begin(); it != node_config.end(); ++it) {
        if (it.key() != "allow_full_tool_results" && it.key() != "preview_max_bytes") {
            return std::unexpected(ResultSyncError::InvalidConfig);
        }
    }
    if (!ValidIdentity(policy_version)) {
        return std::unexpected(ResultSyncError::InvalidIdentity);
    }
    bool allow_full = false;
    if (const auto it = node_config.find("allow_full_tool_results"); it != node_config.end()) {
        if (!it->is_boolean()) return std::unexpected(ResultSyncError::InvalidConfig);
        allow_full = it->get<bool>();
    }
    std::size_t preview_bytes = kDefaultResultPreviewBytes;
    if (const auto it = node_config.find("preview_max_bytes"); it != node_config.end()) {
        if (!it->is_number_integer() || *it < 1 || *it > kMaxResultPreviewBytes) {
            return std::unexpected(ResultSyncError::InvalidPreviewLimit);
        }
        preview_bytes = it->get<std::size_t>();
    }
    return NodeResultSyncPolicy(allow_full, preview_bytes, std::move(policy_version));
}

FrozenToolResult::FrozenToolResult(nlohmann::json payload, ResultSyncMode mode)
    : payload_(std::move(payload)), mode_(mode) {}

std::expected<nlohmann::json, ResultSyncError> FrozenToolResult::ForTransmission(
    const NodeResultSyncPolicy& current_policy,
    const SessionResultSyncPolicy& current_session,
    const runtime::SecretRedactor& current_secrets) const {
    if (mode_ == ResultSyncMode::Full &&
        (!current_policy.allow_full_tool_results() || current_session.mode != ResultSyncMode::Full)) {
        return std::unexpected(ResultSyncError::FullSyncDisabled);
    }
    if (current_session.session_id != payload_["sessionId"])
        return std::unexpected(ResultSyncError::SessionMismatch);
    if (current_session.version == 0 ||
        (current_session.mode != ResultSyncMode::Preview && current_session.mode != ResultSyncMode::Full))
        return std::unexpected(ResultSyncError::InvalidConfig);
    if (payload_["sessionPolicyVersion"] != current_session.version ||
        mode_ != current_session.mode)
        return std::unexpected(ResultSyncError::PolicyChanged);
    if (payload_["policyVersion"] != current_policy.version()) {
        return std::unexpected(ResultSyncError::PolicyChanged);
    }
    if (payload_["previewMaxBytes"] != current_policy.preview_max_bytes()) {
        if (current_policy.preview_max_bytes() < payload_["previewMaxBytes"].get<std::size_t>())
            return std::unexpected(ResultSyncError::PolicyRestricted);
        return std::unexpected(ResultSyncError::PolicyChanged);
    }
    if (payload_["nodeAllowsFull"] != current_policy.allow_full_tool_results())
        return std::unexpected(ResultSyncError::PolicyChanged);
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
    for (const char* key : {"sessionId", "toolCallId", "resultId", "operationId", "turnId", "persistedEventId", "policyVersion"}) {
        if (SensitiveIdentity(payload_[key].get_ref<const std::string&>(), current_secrets)) {
            return std::unexpected(ResultSyncError::SensitiveIdentity);
        }
    }
    if (payload_.dump().size() > kMaxFullResultBytes)
        return std::unexpected(ResultSyncError::ResultTooLarge);
    return payload_;
}

std::expected<FrozenToolResult, ResultSyncError> ProjectSavedToolResult(
    const NodeResultSyncPolicy& policy, const SessionResultSyncPolicy& session,
    const ResultSyncIdentity& identity,
    const SavedToolResult& saved_result, const runtime::SecretRedactor& known_secrets) {
    if (session.session_id != identity.session_id)
        return std::unexpected(ResultSyncError::SessionMismatch);
    if (session.version == 0 ||
        (session.mode != ResultSyncMode::Preview && session.mode != ResultSyncMode::Full))
        return std::unexpected(ResultSyncError::InvalidConfig);
    if (session.mode == ResultSyncMode::Full && !policy.allow_full_tool_results())
        return std::unexpected(ResultSyncError::FullSyncDisabled);
    for (const std::string* value : {&identity.session_id, &identity.tool_call_id,
                                    &identity.result_id, &identity.operation_id, &identity.turn_id,
                                    &identity.persisted_event_id, &policy.version()}) {
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
    if (session.mode == ResultSyncMode::Full && !saved_result.capture_complete) {
        return std::unexpected(ResultSyncError::ResultIncomplete);
    }
    nlohmann::json payload{
        {"sessionId", identity.session_id}, {"toolCallId", identity.tool_call_id},
        {"resultId", identity.result_id}, {"policyVersion", policy.version()},
        {"schemaVersion", 2}, {"operationId", identity.operation_id}, {"turnId", identity.turn_id},
        {"persistedEventId", identity.persisted_event_id},
        {"sessionPolicyVersion", session.version}, {"nodeAllowsFull", policy.allow_full_tool_results()},
        {"previewMaxBytes", policy.preview_max_bytes()},
        {"mode", session.mode == ResultSyncMode::Full ? "full" : "preview"},
        {"contentKind", saved_result.kind == ResultContentKind::Text ? "text" : "binary"},
        {"captureComplete", saved_result.capture_complete},
        {"originalBytes", saved_result.original_bytes.has_value()
                              ? nlohmann::json(*saved_result.original_bytes) : nlohmann::json(nullptr)},
    };
    if (saved_result.kind == ResultContentKind::Binary) {
        if (session.mode == ResultSyncMode::Full) {
            return std::unexpected(ResultSyncError::ResultNotText);
        }
        payload["status"] = "metadata_only";
        payload["truncated"] = true;
        return FrozenToolResult(std::move(payload), session.mode);
    }
    if (saved_result.text.size() > kMaxResultInputBytes) {
        return std::unexpected(ResultSyncError::ResultTooLarge);
    }
    if (saved_result.original_bytes.has_value() &&
        (*saved_result.original_bytes < saved_result.text.size() ||
         (saved_result.capture_complete && *saved_result.original_bytes != saved_result.text.size()))) {
        return std::unexpected(ResultSyncError::InvalidSource);
    }
    if (!saved_result.capture_complete) {
        // 本地捕获本就截断时，尾部可能只剩某枚秘密的前半截，任何按完整
        // 值识别的扫描都可能失手。preview 不发布这段正文；full 已在前面拒绝。
        payload["status"] = "capture_incomplete";
        payload["truncated"] = true;
        return FrozenToolResult(std::move(payload), session.mode);
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
    if (session.mode == ResultSyncMode::Full && redacted.size() > kMaxFullResultBytes) {
        return std::unexpected(ResultSyncError::ResultTooLarge);
    }
    const bool preview_cut = session.mode == ResultSyncMode::Preview &&
                             redacted.size() > policy.preview_max_bytes();
    payload["status"] = "ready";
    payload["text"] = session.mode == ResultSyncMode::Preview
                          ? platform::TruncateUtf8Prefix(redacted, policy.preview_max_bytes()) : redacted;
    payload["truncated"] = preview_cut || !saved_result.capture_complete;
    payload["redacted"] = redacted != raw_text;
    if (saved_result.capture_complete && !saved_result.original_bytes.has_value()) {
        payload["originalBytes"] = raw_text.size();
    }
    return FrozenToolResult(std::move(payload), session.mode);
}

std::expected<std::string, ResultSyncError> FrozenToolResult::SerializeForStorage() const {
    nlohmann::json record{{"schema", "lubancore.result-projection.native.v2"},
                          {"payload", payload_}, {"sha256", platform::Sha256Hex(payload_.dump())}};
    const auto bytes = record.dump();
    if (bytes.size() > kMaxFullResultBytes) return std::unexpected(ResultSyncError::ResultTooLarge);
    return bytes;
}

std::expected<FrozenToolResult, ResultSyncError> FrozenToolResult::RestoreSavedProjection(
    std::string_view storage, const NodeResultSyncPolicy& node,
    const SessionResultSyncPolicy& session, const ResultSyncIdentity& expected_identity,
    const runtime::SecretRedactor& known_secrets) {
    if (storage.size() > kMaxFullResultBytes) return std::unexpected(ResultSyncError::ResultTooLarge);
    const auto bad = [] { return std::unexpected(ResultSyncError::InvalidFrozenRecord); };
    const auto record = nlohmann::json::parse(storage, nullptr, false);
    if (!record.is_object() || record.size() != 3 ||
        record.value("schema", nlohmann::json()) != "lubancore.result-projection.native.v2" ||
        !record.contains("payload") || !record.contains("sha256") || !record["sha256"].is_string()) return bad();
    const auto& p = record["payload"];
    if (!p.is_object() || platform::Sha256Hex(p.dump()) != record["sha256"]) return bad();
    std::set<std::string> keys;
    for (auto it = p.begin(); it != p.end(); ++it) keys.insert(it.key());
    std::set<std::string> expected{"schemaVersion", "sessionId", "operationId", "turnId", "toolCallId",
        "persistedEventId", "resultId", "policyVersion", "sessionPolicyVersion", "nodeAllowsFull",
        "previewMaxBytes", "mode", "contentKind", "captureComplete", "originalBytes", "status", "truncated"};
    if (p.contains("text")) { expected.insert("text"); expected.insert("redacted"); }
    if (keys != expected || !p["schemaVersion"].is_number_integer() || p["schemaVersion"] != 2) return bad();
    for (const char* key : {"sessionId", "operationId", "turnId", "toolCallId", "persistedEventId",
                            "resultId", "policyVersion"}) {
        if (!p[key].is_string() || !ValidIdentity(p[key].get_ref<const std::string&>())) return bad();
    }
    if (!p["sessionPolicyVersion"].is_number_unsigned() || p["sessionPolicyVersion"] == 0 ||
        !p["previewMaxBytes"].is_number_unsigned() || p["previewMaxBytes"] < 1 ||
        p["previewMaxBytes"] > kMaxResultPreviewBytes || !p["nodeAllowsFull"].is_boolean() ||
        !p["captureComplete"].is_boolean() || !p["truncated"].is_boolean() ||
        !(p["originalBytes"].is_null() || p["originalBytes"].is_number_unsigned()) ||
        (p["mode"] != "preview" && p["mode"] != "full") ||
        (p["contentKind"] != "text" && p["contentKind"] != "binary")) return bad();
    if (p["sessionId"] != expected_identity.session_id || p["operationId"] != expected_identity.operation_id ||
        p["turnId"] != expected_identity.turn_id || p["toolCallId"] != expected_identity.tool_call_id ||
        p["persistedEventId"] != expected_identity.persisted_event_id || p["resultId"] != expected_identity.result_id)
        return std::unexpected(ResultSyncError::SessionMismatch);
    if (p.contains("text")) {
        if (!p["text"].is_string() || !p["redacted"].is_boolean() || p["status"] != "ready" ||
            p["contentKind"] != "text" || p["captureComplete"] != true) return bad();
        const auto& text = p["text"].get_ref<const std::string&>();
        if (!platform::IsValidUtf8(text) || HasBinaryControls(text) || text.size() > kMaxFullResultBytes ||
            (p["mode"] == "preview" && text.size() > p["previewMaxBytes"].get<std::size_t>()) ||
            (p["mode"] == "full" && p["truncated"] != false)) return bad();
    } else if (p["mode"] != "preview" || p["truncated"] != true ||
        !((p["status"] == "metadata_only" && p["contentKind"] == "binary") ||
          (p["status"] == "capture_incomplete" && p["contentKind"] == "text" && p["captureComplete"] == false))) return bad();
    FrozenToolResult frozen(p, p["mode"] == "full" ? ResultSyncMode::Full : ResultSyncMode::Preview);
    auto checked = frozen.ForTransmission(node, session, known_secrets);
    if (!checked) return std::unexpected(checked.error());
    return frozen;
}

}  // namespace lubancode::remote
