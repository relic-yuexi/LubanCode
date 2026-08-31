// channel.yaml schema 1 严格 parser(多渠道消息接入单阶段 1)。
//
// 唯一真源是 docs/architecture/channels/channel-manifest.md(阶段 0
// 冻结件)。字段表、取值集、占位符信任规矩全照那份文档;实现不放宽。
// 与 package/component.cpp 的 ParseMcpComponentYaml 同一件事的另一份:
// 未知字段报错、类型错报错、占位符只认 ${channel_dir}、越界报错。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::channel {

// channel.yaml 只认 schema 1(channel-manifest.md §2)。
inline constexpr int kChannelManifestSchemaVersion = 1;

struct ChannelManifestExecutableRequirement {
    std::string name;
    std::optional<std::string> version;  // 原文,如 ">=20"(§3.2:不做 semver 求值,仅记账)
};

// runtime(§3.2)。首版 kind 只收 "process"。
struct ChannelManifestRuntime {
    std::string kind = "process";
    std::string command;
    std::vector<std::string> args;  // 只许 ${channel_dir} 占位
    std::string protocol = "lubancode-channel/1";
    int startup_timeout_ms = 15000;
    int shutdown_timeout_ms = 5000;
    std::vector<ChannelManifestExecutableRequirement> requires_executables;
};

// capabilities(§3.3)。声明,不是授权;取值集冻结,未知值报错。
struct ChannelManifestCapabilities {
    std::vector<std::string> transports;     // websocket webhook long_polling sdk_events
    std::vector<std::string> conversations;  // direct group guild channel thread
    std::vector<std::string> inbound;    // text image audio video file link mention reply
                                          // location unsupported
    std::vector<std::string> outbound;   // text image audio video file reply
    std::vector<std::string> delivery;   // send edit native_stream typing react
    std::vector<std::string> login;      // credentials qr oauth device_code
};

// limits(§3.4)。
struct ChannelManifestLimits {
    std::optional<std::int64_t> text_chars;
    std::optional<std::int64_t> media_bytes;
    std::optional<std::int64_t> outbound_requests_per_minute;
};

// state(§3.5)。
struct ChannelManifestState {
    int format = 1;
    std::optional<std::string> migrator;  // 支持 ${channel_dir}
};

// 完整 channel.yaml(§2 完整示例 + §3 字段规矩)。
struct ChannelManifest {
    int schema = kChannelManifestSchemaVersion;
    std::string id;
    std::string name;
    std::string description;
    ChannelManifestRuntime runtime;
    ChannelManifestCapabilities capabilities;
    ChannelManifestLimits limits;
    ChannelManifestState state;
    // 非官方个人账号自动化的唯一附加风险标注(§3.6:"其余未知字段一律
    // 报错",risk 是唯一例外)。首版只认 "unofficial_personal_account"。
    std::optional<std::string> risk;
};

// 一条 channel.yaml 解析错:字段路径 + 行(1 起,0 = 拿不到)+ 人话。与
// package::ManifestError / McpComponentError 同款口径,方便 doctor 统一
// 格式化。
struct ChannelManifestError {
    std::string field;
    int line = 0;
    std::string detail;
};

// 严格解析一份 channel.yaml 文本。package_root 用于 ${channel_dir} 占位符
// 展开后的越界检查(§3.2:Package 内 command canonical 后必须留在 Package
// 根内)。channel_dir 是 channel.yaml 所在目录(通常是
// package_root/channels/<local-id>),供占位符实际展开用;调用方(阶段 2
// 起的组件 loader)传入。
std::expected<ChannelManifest, std::vector<ChannelManifestError>> ParseChannelManifestYaml(
    std::string_view yaml_text, const std::filesystem::path& package_root,
    const std::filesystem::path& channel_dir);

}  // namespace lubancode::channel
