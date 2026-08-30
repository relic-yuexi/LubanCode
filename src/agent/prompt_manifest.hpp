// PromptManifest 与 request snapshot metadata 合同(Token 账本单 §6.4 A0 冻结)。
//
// 现有 PromptSourceLedger 只管 AssembleSystemPrompt() 里那一段;AgentLoop
// 随后还会叠 deferred tool index、model instructions 与 soul。A1 的
// ResolvedPromptBuilder 会把这些后叠层收进同一次解析,每次拼完最终 system
// prompt 产一份 manifest——manifest 不靠 analyzer 事后拆字符串。
// 本件先冻结数据合同与序列化;不记正文,默认 metadata_only。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::agent {

inline constexpr const char* kPromptManifestSchema = "lubancode.prompt.manifest";
inline constexpr int kPromptManifestSchemaVersion = 1;
inline constexpr const char* kRequestSnapshotSchema = "lubancode.request.snapshot";
inline constexpr int kRequestSnapshotSchemaVersion = 1;

// system prompt 的一段:来源、次序、hash、token、override。
struct PromptSegment {
    std::string segment_id;      // 模块相对路径或段名(如 core/10-identity.md)
    std::string role;            // core/features/platform/runtime/…
    std::string source_kind;     // embedded/user_prompt_dir/package/prompt_profile/agent_inline/host_generated
    std::string source_ref;      // "embedded:<id>" / "file:<path>" / "runtime:<name>"
    std::optional<std::string> source_hash;  // 来源文件 hash;host_generated 可空
    std::string rendered_hash;   // 渲染后正文的 hash
    std::int64_t rendered_tokens_estimated = 0;
    int order = 0;               // 拼装次序(稳定)
    bool volatile_segment = false;  // 动态段(时间/cwd/工具清单),易断 cache
    std::vector<std::string> overrides;  // 本段压掉了哪些下层同 id 段

    nlohmann::json ToJson() const;
    static std::optional<PromptSegment> FromJsonStrict(const nlohmann::json& json,
                                                       std::string* error);
};

// 一次拼装的完整 manifest。
struct PromptManifest {
    std::string assembly_version;  // 如 "prompt-assembler-v1"
    std::string resolved_prompt_hash;
    std::int64_t resolved_prompt_tokens_estimated = 0;
    std::string stable_prefix_hash;  // 空 = 无稳定前缀
    std::vector<PromptSegment> segments;  // 按 order 升序
    std::vector<std::string> layers;      // 参与的层(embedded/user_prompt_dir/…)
    struct SoulLayer {
        std::string name;   // "default" 表示无魂
        std::string hash;
        std::int64_t tokens_estimated = 0;
    } soul;
    struct ModelInstructionsLayer {
        std::string hash;   // 空 = 模型无附加指令
        std::int64_t tokens_estimated = 0;
    } model_instructions;

    nlohmann::json ToJson() const;
    static std::optional<PromptManifest> FromJsonStrict(const nlohmann::json& json,
                                                        std::string* error);
};

// provider-neutral request snapshot 的 metadata 块(§6.4)。只装形状与
// hash,不装正文;正文若要留须另过脱敏与权限闸。
struct RequestSnapshotMetadata {
    struct RequestShape {
        std::string model;
        std::uint64_t message_count = 0;
        std::uint64_t tool_count = 0;
        std::string parameters_hash;
        std::string toolset_hash;
        std::int64_t tool_definition_tokens_estimated = 0;

        nlohmann::json ToJson() const;
        static std::optional<RequestShape> FromJsonStrict(const nlohmann::json& json,
                                                          std::string* error);
    } request_shape;
    PromptManifest prompt_manifest;
    // 内容策略:A0 只有 metadata_only;full snapshot 另立 schema 再说。
    std::string content_policy = "metadata_only";

    nlohmann::json ToJson() const;
    static std::optional<RequestSnapshotMetadata> FromJsonStrict(const nlohmann::json& json,
                                                                 std::string* error);
};

}  // namespace lubancode::agent
