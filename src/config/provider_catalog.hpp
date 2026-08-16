#pragma once

#include <chrono>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/config.hpp"

namespace lubancode::config {

inline constexpr int kProviderCatalogSchemaVersion = 1;
inline constexpr std::size_t kProviderCatalogMaxBytes = 2 * 1024 * 1024;
inline constexpr const char* kProviderCatalogUrl =
    "https://raw.githubusercontent.com/relic-yuexi/LubanCode/main/catalog/providers.json";

struct ProviderCatalogVariant {
    std::string id;
    std::string description;
    nlohmann::json extra_body = nlohmann::json::object();
};

struct ProviderCatalogModel {
    std::string id;
    std::string name;
    std::string description;
    std::optional<std::size_t> context_window_tokens;
    std::optional<std::size_t> max_output_tokens;
    std::string default_think;
    std::map<std::string, bool> capabilities;
    std::vector<ProviderCatalogVariant> variants;
};

struct ProviderPreset {
    std::string id;
    std::string name;
    std::string description;
    Wire wire = Wire::Anthropic;
    std::string base_url;
    std::string key_env;
    std::string default_model;
    std::string model_reasoning_effort;
    bool native_web_search = false;
    // stream_usage:该端支持 Chat 流式的 stream_options.include_usage(在
    // [DONE] 前多回一只完整 usage chunk)。有些兼容端不认 stream_options,
    // 所以按 provider 声明,默认不发;DeepSeek 等家置真。
    bool stream_usage = false;
    // reasoning_replay:Chat wire 的思考回传策略,"" / "never" / "tool_episode"
    // (语义见 api/chat/request.hpp)。DeepSeek 这类要求工具交互段回传
    // reasoning_content 的端配 "tool_episode";默认空 = never。
    std::string reasoning_replay;
    // reasoning_delta_field / reasoning_replay_field:Chat wire 思考字段的
    // 两枚名字声明(语义见 api/chat/request.hpp 的 ChatRequestOptions)。
    // 空 = 自动兼容(delta 侧两个别名都认) / 回传写 reasoning_content。
    // vLLM 0.27+/Qwen 这类端 delta 叫 reasoning、回传也只认 reasoning,
    // 两条都声明成 "reasoning"。
    std::string reasoning_delta_field;
    std::string reasoning_replay_field;
    std::string docs_url;
    nlohmann::json extra_body = nlohmann::json::object();
    std::map<std::string, std::string> extra_headers;
    std::vector<ProviderCatalogModel> models;

    const ProviderCatalogModel* FindModel(const std::string& model_id) const;
};

struct ProviderCatalog {
    int schema_version = kProviderCatalogSchemaVersion;
    std::string revision;
    std::vector<ProviderPreset> providers;
    std::string source_path;
    std::vector<std::string> warnings;

    const ProviderPreset* FindProvider(const std::string& id) const;
};

std::expected<ProviderCatalog, std::string> ParseProviderCatalogJson(
    const std::string& text, const std::string& source_path);

// 内置快照打底；缓存存在且合法时压过内置。坏缓存只添警告，不拦启动。
ProviderCatalog LoadProviderCatalog();

std::optional<std::string> ProviderCatalogCachePath();
bool ProviderCatalogCacheIsStale(std::chrono::hours max_age = std::chrono::hours(24));

struct ProviderCatalogRefresh {
    bool updated = false;
    bool not_modified = false;
    std::string cache_path;
    std::string revision;
};

std::expected<ProviderCatalogRefresh, std::string> RefreshProviderCatalog(
    int connect_timeout_ms = 3000, int request_timeout_secs = 10);

// 从预设生一条本地 provider 配置。目录只给默认值，落盘后用户仍可自由改。
ProviderConfig ProviderConfigFromPreset(const ProviderPreset& preset);

// extra_headers 可写 ${LUBANCODE_API_KEY}，真正发请求前才替换，不把密钥写进目录。
std::map<std::string, std::string> ResolveProviderHeaderTemplates(
    const std::map<std::string, std::string>& headers, const std::string& api_key);

}  // namespace lubancode::config
