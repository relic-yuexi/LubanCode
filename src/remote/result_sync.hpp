// 节点工具结果同步的第一块纯组件；尚无 Node、网络或持久 outbox 接线。
//
// 策略只从节点自有配置构造，不能把网页参数、模型输出交给配置解析器。
// 投影只吃一份已保存且不再变化的结果，不接 tail/offset、原始事件 JSON、
// 文件路径或附件读取器。调用方须登记本场已解析的秘密，再生成一次记录。
// 实时/历史/重试复用这份记录；后续 outbox 必须持久保存它，不能重新投影
// 日志尾部。本件只保证返回值不可变，不冒充跨重启去重或身份授权。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lubancode::runtime {
class SecretRedactor;
}

namespace lubancode::remote {

inline constexpr std::size_t kDefaultResultPreviewBytes = 4096;
inline constexpr std::size_t kMaxResultPreviewBytes = 64 * 1024;
// 首批只处理有界、已落盘结果的文本快照。更大原件须后续流式读取/脱敏
// 合同；这里明拒，不偷截一段并称作完整。full 的帽按脱敏后正文计。
// 这是字节帽，不是 CPU 时限：共享秘密扫描器在密集命中时可能退化。
// Node 接线前须补扫描/调度配额，不能在网络读线程直接调用本投影。
inline constexpr std::size_t kMaxResultInputBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kMaxFullResultBytes = 1024 * 1024;

enum class ResultSyncMode { Preview, Full };
enum class ResultContentKind { Text, Binary };

// 错误只返回固定码，不拼输入、路径、配置值或工具错误正文。
enum class ResultSyncError {
    InvalidConfig,
    InvalidMode,
    InvalidPreviewLimit,
    InvalidIdentity,
    SensitiveIdentity,
    InvalidSource,
    ResultMissing,
    ResultIncomplete,
    ResultTooLarge,
    ResultNotText,
    InvalidUtf8,
    FullSyncDisabled,
    PolicyRestricted,
    PolicyChanged,
    RedactionChanged,
};
std::string_view ResultSyncErrorCode(ResultSyncError error);

class NodeResultSyncPolicy {
public:
    ResultSyncMode mode() const { return mode_; }
    std::size_t preview_max_bytes() const { return preview_max_bytes_; }
    const std::string& version() const { return version_; }

private:
    NodeResultSyncPolicy(ResultSyncMode mode, std::size_t preview_max_bytes, std::string version);
    ResultSyncMode mode_;
    std::size_t preview_max_bytes_;
    std::string version_;

    friend std::expected<NodeResultSyncPolicy, ResultSyncError> ParseNodeResultSyncPolicy(
        const nlohmann::json&, std::string);
};

// 输入仅为本组件的节点配置段：tool_result_sync、preview_max_bytes。
// 空对象 => preview/4096；null、未知键、错误类型、越界数字均明拒。
// policy_version 由可信节点配置管理器发放；变更配置/脱敏规则须换版本。
std::expected<NodeResultSyncPolicy, ResultSyncError> ParseNodeResultSyncPolicy(
    const nlohmann::json& node_config, std::string policy_version);

struct ResultSyncIdentity {
    std::string session_id;
    std::string tool_call_id;
    std::string result_id;
};

struct SavedToolResult {
    ResultContentKind kind = ResultContentKind::Text;
    bool available = true;
    bool capture_complete = true;
    // 只借用至 ProjectSavedToolResult 返回；投影不会持有原文。
    // text 是节点保存的文本结果，不是任意原生事件/附件的 JSON dump。
    std::string_view text;
    // 完整文本给值时必须等于 text.size()；捕获截断时可以未知。
    // binary 只允许元数据，不读 text，不取文件，也不做 base64。
    std::optional<std::uint64_t> original_bytes;
};

class FrozenToolResult {
public:
    // 发出前再查当前节点策略与已知秘密。降为 preview 后，旧 full 不许
    // 继续发送；缩小预算/更换策略版本时也明拒，不修改旧记录或悄悄换内容。
    // 已知秘密集合变化必须换 policyVersion，截短正文无法自行验证一个
    // 新秘密是否横跨旧预览刀口。更换版本后的重置/历史审批不属本组件。
    // 返回 JSON 副本，调用方修改副本不会改变后续重投内容。
    std::expected<nlohmann::json, ResultSyncError> ForTransmission(
        const NodeResultSyncPolicy& current_policy,
        const runtime::SecretRedactor& current_secrets) const;

private:
    explicit FrozenToolResult(nlohmann::json payload, ResultSyncMode mode);
    nlohmann::json payload_;
    ResultSyncMode mode_;

    friend std::expected<FrozenToolResult, ResultSyncError> ProjectSavedToolResult(
        const NodeResultSyncPolicy&, const ResultSyncIdentity&, const SavedToolResult&,
        const runtime::SecretRedactor&);
};

// 固定前缀预览：整段核编码/脱敏后截断，不提供滚动窗口或分页口。
// 二进制 preview 只报类型/大小；full 明报 result_not_text，不顺引用取附件。
// 捕获不完整的 preview 也只报元数据：原件刀口可能留下半枚秘密，不能
// 仅凭截断标记就让它出网；full 则明报 result_capture_incomplete。
// 不能用这个 DTO 裁审批材料；审批有自己的参数绑定与展示合同。
std::expected<FrozenToolResult, ResultSyncError> ProjectSavedToolResult(
    const NodeResultSyncPolicy& policy, const ResultSyncIdentity& identity,
    const SavedToolResult& saved_result, const runtime::SecretRedactor& known_secrets);

}  // namespace lubancode::remote
