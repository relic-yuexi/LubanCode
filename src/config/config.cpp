#include "config/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// i18n:cli/i18n 是零依赖的叶子字符串表(只用标准库 + json),config 层引它
// 不构成反向依赖——它不牵扯 cli 的任何交互逻辑。
#include "cli/i18n.hpp"
#include "platform/paths.hpp"

namespace lubancode::config {

namespace {

// 读一个环境变量;没设置或者是空串都算"没有"。跨平台单起委托 platform 层
// (Windows _dupenv_s / POSIX getenv 的分支收拢在那边)。
std::optional<std::string> GetEnv(const char* name) {
    return platform::GetEnvVar(name);
}

// 前向声明:定义在下面另一段匿名命名空间里(跟 UpdateProvidersInConfigFile
// 挨着),这里提前声明给 SaveConfigFile 用——同一个翻译单元里的匿名命名空间
// 全部等价于同一个具名空间,提前声明、后面定义没问题。
nlohmann::json ProvidersToJson(const std::vector<ProviderConfig>& providers);

}  // namespace

std::optional<std::string> HomeDir() {
    return platform::HomeDir();
}

std::optional<std::string> HomeLubancodeDir() {
    const auto home = HomeDir();
    if (!home.has_value()) {
        return std::nullopt;
    }
    return (std::filesystem::path(*home) / ".lubancode").string();
}

// 新位置配置文件的路径:<base_dir>/.lubancode/config.json。
std::filesystem::path NewConfigPathFor(const std::filesystem::path& base_dir) {
    return base_dir / ".lubancode" / "config.json";
}

// 旧位置配置文件的路径:<base_dir>/.lubancode.json。
std::filesystem::path OldConfigPathFor(const std::filesystem::path& base_dir) {
    return base_dir / ".lubancode.json";
}

std::expected<Wire, std::string> ParseProviderWire(const std::string& raw) {
    if (raw == "anthropic") {
        return Wire::Anthropic;
    }
    if (raw == "responses") {
        return Wire::Responses;
    }
    if (raw == "chat_completions" || raw == "chat") {
        return Wire::ChatCompletions;
    }
    return std::unexpected("只认得 anthropic、responses 或 chat_completions,写的是: " + raw);
}

std::string ProviderWireName(Wire wire) {
    switch (wire) {
        case Wire::Anthropic:
            return "anthropic";
        case Wire::Responses:
            return "responses";
        case Wire::ChatCompletions:
            return "chat_completions";
    }
    return "anthropic";
}

std::expected<bool, std::string> ParseBoolToggle(const std::string& raw) {
    std::string lower = raw;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower == "on" || lower == "true" || lower == "1") {
        return true;
    }
    if (lower == "off" || lower == "false" || lower == "0") {
        return false;
    }
    return std::unexpected("只认得 on/off、true/false、1/0,写的是: " + raw);
}

std::expected<void, std::string> ValidateProviderConfig(const ProviderConfig& provider) {
    if (provider.name.empty()) {
        return std::unexpected("provider 名字不能为空");
    }
    if (provider.base_url.rfind("http://", 0) != 0 && provider.base_url.rfind("https://", 0) != 0) {
        return std::unexpected("base_url 得以 http:// 或 https:// 开头");
    }
    if (provider.key_env.empty()) {
        return std::unexpected("key_env 不能为空");
    }
    if (provider.context_window_tokens == 0) {
        return std::unexpected("context_window 得是正整数");
    }
    return {};
}

std::expected<void, std::string> ValidateProviderName(const std::string& name,
                                                        const std::vector<ProviderConfig>& existing) {
    if (name.empty()) {
        return std::unexpected("provider 名字不能为空");
    }
    for (const char c : name) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '-';
        if (!ok) {
            return std::unexpected("provider 名字只能包含字母、数字、下划线、点、短横线: " + name);
        }
    }
    if (FindProvider(existing, name) != nullptr) {
        return std::unexpected("provider 已存在: " + name);
    }
    return {};
}

std::optional<std::string> ProviderApiKey(const ProviderConfig& provider) {
    if (!provider.api_key.empty()) {
        return provider.api_key;
    }
    return GetEnv(provider.key_env.c_str());
}

const ProviderConfig* FindProvider(const std::vector<ProviderConfig>& providers, const std::string& name) {
    for (const ProviderConfig& provider : providers) {
        if (provider.name == name) {
            return &provider;
        }
    }
    return nullptr;
}

bool ApplyConfiguredActiveProvider(ConfigResult& result) {
    if (result.config.active_provider.empty()) {
        return false;
    }
    const ProviderConfig* provider = FindProvider(result.config.providers, result.config.active_provider);
    if (provider == nullptr) {
        result.config.active_provider.clear();
        result.sources.active_provider = Source::Default;
        return false;
    }

    // Source 枚举按优先级从高到低排列。选择名与 provider 条目两者谁
    // 层级更高就按谁算；只覆盖同级或更低字段。
    const Source source = static_cast<int>(result.sources.active_provider) <
                                  static_cast<int>(result.sources.providers)
                              ? result.sources.active_provider
                              : result.sources.providers;
    const auto can_override = [source](Source current) {
        return static_cast<int>(current) >= static_cast<int>(source);
    };
    if (can_override(result.sources.wire)) {
        result.config.wire = provider->wire;
        result.sources.wire = source;
    }
    if (can_override(result.sources.base_url)) {
        result.config.base_url = provider->base_url;
        result.sources.base_url = source;
    }
    if (can_override(result.sources.auth_token)) {
        result.config.auth_token = ProviderApiKey(*provider).value_or(std::string());
        result.sources.auth_token = source;
    }
    if (can_override(result.sources.model)) {
        result.config.model = provider->model;
        result.sources.model = source;
    }
    if (can_override(result.sources.context_window_tokens)) {
        result.config.context_window_tokens = provider->context_window_tokens;
        result.sources.context_window_tokens = source;
    }
    if (!provider->model_reasoning_effort.empty() && can_override(result.sources.think)) {
        result.config.think = provider->model_reasoning_effort;
        result.sources.think = source;
    }
    if (can_override(result.sources.extra_body)) {
        result.config.extra_body = provider->extra_body;
        result.sources.extra_body = source;
    }
    if (can_override(result.sources.extra_headers)) {
        result.config.extra_headers = provider->extra_headers;
        result.sources.extra_headers = source;
    }
    result.config.native_web_search = provider->native_web_search;
    result.config.stream_usage = provider->stream_usage;
    result.config.reasoning_replay = provider->reasoning_replay;
    return true;
}

bool SetProviderNativeWebSearch(std::vector<ProviderConfig>& providers, const std::string& name, bool enabled) {
    for (ProviderConfig& provider : providers) {
        if (provider.name == name) {
            provider.native_web_search = enabled;
            return true;
        }
    }
    return false;
}

bool SetProviderExtraBody(std::vector<ProviderConfig>& providers, const std::string& name,
                           const nlohmann::json& body) {
    for (ProviderConfig& provider : providers) {
        if (provider.name == name) {
            provider.extra_body = body;
            return true;
        }
    }
    return false;
}

bool SetProviderExtraHeader(std::vector<ProviderConfig>& providers, const std::string& name,
                             const std::string& header_name, const std::string& value) {
    for (ProviderConfig& provider : providers) {
        if (provider.name == name) {
            if (value.empty()) {
                provider.extra_headers.erase(header_name);
            } else {
                provider.extra_headers[header_name] = value;
            }
            return true;
        }
    }
    return false;
}

std::string ToString(Source source) {
    switch (source) {
        case Source::LubancodeEnv:
            return cli::tr("config.source.lubancode_env");
        case Source::ProjectConfigFile:
            return cli::tr("config.source.project_config_file");
        case Source::GlobalConfigFile:
            return cli::tr("config.source.global_config_file");
        case Source::GenericEnv:
            return cli::tr("config.source.generic_env");
        case Source::Default:
            return cli::tr("config.source.default");
    }
    return cli::tr("config.source.unknown");
}

std::expected<std::size_t, std::string> ParseContextWindowTokens(const std::string& raw) {
    if (raw.empty()) {
        return std::unexpected(std::string("context_window 不能是空串"));
    }

    std::string lower = raw;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    std::size_t multiplier = 1;
    std::string digits = lower;
    if (lower.back() == 'k') {
        multiplier = 1000;
        digits = lower.substr(0, lower.size() - 1);
    } else if (lower.back() == 'm') {
        multiplier = 1000000;
        digits = lower.substr(0, lower.size() - 1);
    }

    if (digits.empty()) {
        return std::unexpected("context_window 取值不对: " + raw + "(k/m 后缀前面得跟数字,比如 256k)");
    }
    for (const char c : digits) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return std::unexpected("context_window 取值不对: " + raw + "(只认 256k / 512k / 1m 这种写法,或者裸数字)");
        }
    }

    long long value = 0;
    try {
        std::size_t consumed = 0;
        value = std::stoll(digits, &consumed);
        if (consumed != digits.size()) {
            return std::unexpected("context_window 取值不对: " + raw);
        }
    } catch (...) {
        return std::unexpected("context_window 取值不对: " + raw);
    }

    if (value <= 0) {
        return std::unexpected("context_window 必须是正数: " + raw);
    }

    return static_cast<std::size_t>(value) * multiplier;
}

namespace {

// pre_tool/post_tool 认 matcher 字段,session_start/session_end 不认——用
// with_matcher 区分,复用同一份数组解析逻辑。
std::expected<std::vector<HookEntry>, std::string> ParseHookEntryArray(const nlohmann::json& arr,
                                                                          const std::string& field_name,
                                                                          bool with_matcher,
                                                                          const std::string& file_path_for_error) {
    if (!arr.is_array()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 hooks." + field_name + " 字段必须是数组");
    }
    std::vector<HookEntry> out;
    out.reserve(arr.size());
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& item = arr[i];
        if (!item.is_object()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 hooks." + field_name + "[" +
                                    std::to_string(i) + "] 必须是一个 JSON object");
        }
        if (!item.contains("command") || !item["command"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 hooks." + field_name + "[" +
                                    std::to_string(i) + "] 缺少必填字段 command(字符串)");
        }
        HookEntry entry;
        entry.command = item["command"].get<std::string>();
        entry.source_path = file_path_for_error;
        if (with_matcher) {
            entry.matcher = "*";  // 缺省当 "*"
            if (item.contains("matcher")) {
                if (!item["matcher"].is_string()) {
                    return std::unexpected("配置文件 " + file_path_for_error + " 里的 hooks." + field_name + "[" +
                                            std::to_string(i) + "] 的 matcher 字段必须是字符串");
                }
                const std::string matcher = item["matcher"].get<std::string>();
                if (!matcher.empty()) {
                    entry.matcher = matcher;
                }
            }
        }
        out.push_back(std::move(entry));
    }
    return out;
}

}  // namespace

std::expected<std::map<std::string, McpServerConfig>, std::string> ParseMcpServersConfig(
    const nlohmann::json& mcp_servers_json, const std::string& file_path_for_error) {
    if (!mcp_servers_json.is_object()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 mcpServers 字段必须是一个 JSON object");
    }

    std::map<std::string, McpServerConfig> out;
    for (auto it = mcp_servers_json.begin(); it != mcp_servers_json.end(); ++it) {
        const std::string& server_name = it.key();
        const nlohmann::json& value = it.value();
        if (!value.is_object()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 mcpServers." + server_name +
                                    " 必须是一个 JSON object");
        }
        if (!value.contains("command") || !value["command"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 mcpServers." + server_name +
                                    " 缺少必填字段 command(字符串)");
        }

        McpServerConfig server;
        server.command = value["command"].get<std::string>();

        if (value.contains("args")) {
            if (!value["args"].is_array()) {
                return std::unexpected("配置文件 " + file_path_for_error + " 里的 mcpServers." + server_name +
                                        ".args 字段必须是数组");
            }
            for (const auto& item : value["args"]) {
                if (!item.is_string()) {
                    return std::unexpected("配置文件 " + file_path_for_error + " 里的 mcpServers." + server_name +
                                            ".args 数组元素必须是字符串");
                }
                server.args.push_back(item.get<std::string>());
            }
        }

        if (value.contains("env")) {
            if (!value["env"].is_object()) {
                return std::unexpected("配置文件 " + file_path_for_error + " 里的 mcpServers." + server_name +
                                        ".env 字段必须是一个 JSON object(字符串到字符串)");
            }
            for (auto env_it = value["env"].begin(); env_it != value["env"].end(); ++env_it) {
                if (!env_it.value().is_string()) {
                    return std::unexpected("配置文件 " + file_path_for_error + " 里的 mcpServers." + server_name +
                                            ".env." + env_it.key() + " 的值必须是字符串");
                }
                server.env.emplace_back(env_it.key(), env_it.value().get<std::string>());
            }
        }

        out.emplace(server_name, std::move(server));
    }
    return out;
}

std::expected<SearchConfig, std::string> ParseSearchConfig(const nlohmann::json& search_json,
                                                             const std::string& file_path_for_error) {
    if (!search_json.is_object()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 search 字段必须是一个 JSON object");
    }
    if (!search_json.contains("provider") || !search_json["provider"].is_string()) {
        return std::unexpected("配置文件 " + file_path_for_error +
                                " 里的 search 段缺少必填字段 provider(字符串,tavily/brave/serper 三选一)");
    }
    const std::string provider = search_json["provider"].get<std::string>();
    if (provider != "tavily" && provider != "brave" && provider != "serper") {
        return std::unexpected("配置文件 " + file_path_for_error +
                                " 里的 search.provider 只认 tavily/brave/serper,写的是: " + provider);
    }
    if (!search_json.contains("api_key") || !search_json["api_key"].is_string()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 search 段缺少必填字段 api_key(字符串)");
    }
    const std::string api_key = search_json["api_key"].get<std::string>();
    if (api_key.empty()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 search.api_key 不能是空串");
    }

    SearchConfig config;
    config.provider = provider;
    config.api_key = api_key;
    return config;
}

std::expected<std::map<std::string, LspServerConfig>, std::string> ParseLspServersConfig(
    const nlohmann::json& lsp_json, const std::string& file_path_for_error) {
    if (!lsp_json.is_object()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 lsp 字段必须是一个 JSON object");
    }

    std::map<std::string, LspServerConfig> out;
    for (auto it = lsp_json.begin(); it != lsp_json.end(); ++it) {
        const std::string& language = it.key();
        const nlohmann::json& value = it.value();
        if (!value.is_object()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 lsp." + language +
                                    " 必须是一个 JSON object");
        }
        if (!value.contains("command") || !value["command"].is_string() ||
            value["command"].get<std::string>().empty()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 lsp." + language +
                                    " 缺少必填字段 command(非空字符串)");
        }

        LspServerConfig server;
        server.command = value["command"].get<std::string>();

        if (value.contains("args")) {
            if (!value["args"].is_array()) {
                return std::unexpected("配置文件 " + file_path_for_error + " 里的 lsp." + language +
                                        ".args 字段必须是数组");
            }
            for (const auto& item : value["args"]) {
                if (!item.is_string()) {
                    return std::unexpected("配置文件 " + file_path_for_error + " 里的 lsp." + language +
                                            ".args 数组元素必须是字符串");
                }
                server.args.push_back(item.get<std::string>());
            }
        }

        if (!value.contains("extensions") || !value["extensions"].is_array() || value["extensions"].empty()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 lsp." + language +
                                    " 缺少必填字段 extensions(非空字符串数组,比如 [\".cpp\", \".hpp\"])");
        }
        for (const auto& item : value["extensions"]) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                return std::unexpected("配置文件 " + file_path_for_error + " 里的 lsp." + language +
                                        ".extensions 数组元素必须是非空字符串");
            }
            server.extensions.push_back(item.get<std::string>());
        }

        if (value.contains("idle_minutes")) {
            const auto& field = value["idle_minutes"];
            if ((!field.is_number_integer() && !field.is_number_unsigned()) || field.get<long long>() <= 0) {
                return std::unexpected("配置文件 " + file_path_for_error + " 里的 lsp." + language +
                                        ".idle_minutes 字段必须是正整数");
            }
            server.idle_minutes = static_cast<int>(field.get<long long>());
        }

        out.emplace(language, std::move(server));
    }
    return out;
}

std::expected<nlohmann::json, std::string> ParseExtraBodyConfig(const nlohmann::json& extra_body_json,
                                                                  const std::string& file_path_for_error) {
    if (!extra_body_json.is_object()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 extra_body 字段必须是一个 JSON object");
    }
    return extra_body_json;
}

std::expected<std::map<std::string, std::string>, std::string> ParseExtraHeadersConfig(
    const nlohmann::json& extra_headers_json, const std::string& file_path_for_error) {
    if (!extra_headers_json.is_object()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 extra_headers 字段必须是一个 JSON object");
    }
    std::map<std::string, std::string> out;
    for (auto it = extra_headers_json.begin(); it != extra_headers_json.end(); ++it) {
        if (!it.value().is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 extra_headers." + it.key() +
                                    " 的值必须是字符串");
        }
        out.emplace(it.key(), it.value().get<std::string>());
    }
    return out;
}

std::expected<StatusPanelConfig, std::string> ParseStatusPanelConfig(
    const nlohmann::json& panel_json, const std::string& file_path_for_error) {
    if (!panel_json.is_object()) {
        return std::unexpected("配置文件 " + file_path_for_error +
                               " 里的 status_panel 字段必须是一个 JSON object");
    }

    StatusPanelConfig out;
    if (panel_json.contains("items")) {
        const auto& items = panel_json["items"];
        if (!items.is_array()) {
            return std::unexpected("配置文件 " + file_path_for_error +
                                   " 里的 status_panel.items 字段必须是字符串数组");
        }
        out.items.clear();
        static constexpr std::string_view kAllowed[] = {
            "permission_mode", "model", "cwd", "git_branch",
            "context", "tokens", "provider", "effort"};
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!items[i].is_string()) {
                return std::unexpected("配置文件 " + file_path_for_error +
                                       " 里的 status_panel.items[" + std::to_string(i) +
                                       "] 必须是字符串");
            }
            const std::string item = items[i].get<std::string>();
            const bool known = std::find(std::begin(kAllowed), std::end(kAllowed), item) != std::end(kAllowed);
            if (!known) {
                return std::unexpected("配置文件 " + file_path_for_error +
                                       " 里的 status_panel.items 不认识字段: " + item);
            }
            if (std::find(out.items.begin(), out.items.end(), item) != out.items.end()) {
                return std::unexpected("配置文件 " + file_path_for_error +
                                       " 里的 status_panel.items 字段重复: " + item);
            }
            out.items.push_back(item);
        }
    }
    if (panel_json.contains("separator")) {
        if (!panel_json["separator"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error +
                                   " 里的 status_panel.separator 字段必须是字符串");
        }
        out.separator = panel_json["separator"].get<std::string>();
        const bool has_control = std::any_of(out.separator.begin(), out.separator.end(), [](unsigned char ch) {
            return ch < 0x20U || ch == 0x7fU;
        });
        if (has_control) {
            return std::unexpected("配置文件 " + file_path_for_error +
                                   " 里的 status_panel.separator 不能带控制字符");
        }
    }
    return out;
}

// hooks schema 2:解析一只 handler("type":"command" + command/args/exec
// form 变体)。字段用错在这里就报错,不静默猜。
std::expected<HookHandlerConfig, std::string> ParseHookHandlerConfig(const nlohmann::json& item,
                                                                     const std::string& where,
                                                                     const std::string& file_path_for_error) {
    if (!item.is_object()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where + " 必须是一个 JSON object");
    }
    HookHandlerConfig handler;
    if (item.contains("type")) {
        if (!item["type"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where + ".type 必须是字符串");
        }
        handler.type = item["type"].get<std::string>();
    }
    if (handler.type != "command") {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where + ".type 只支持 \"command\"");
    }
    if (!item.contains("command") || !item["command"].is_string() || item["command"].get<std::string>().empty()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where +
                               " 缺少必填字段 command(非空字符串)");
    }
    handler.command = item["command"].get<std::string>();

    const auto parse_args = [&](const char* field, std::vector<std::string>& out) -> bool {
        if (!item.contains(field)) {
            return true;
        }
        if (!item[field].is_array()) {
            return false;
        }
        for (const auto& arg : item[field]) {
            if (!arg.is_string()) {
                return false;
            }
            out.push_back(arg.get<std::string>());
        }
        return true;
    };
    if (!parse_args("args", handler.args)) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where + ".args 必须是字符串数组");
    }
    if (item.contains("command_windows")) {
        if (!item["command_windows"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where +
                                   ".command_windows 必须是字符串");
        }
        handler.command_windows = item["command_windows"].get<std::string>();
    }
    if (!parse_args("args_windows", handler.args_windows)) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where +
                               ".args_windows 必须是字符串数组");
    }
    if (item.contains("timeout")) {
        // 配置里写秒(与 Claude Code/Codex 的写法一致),1..600,这里换算成
        // 毫秒存。越界/类型不对报错——timeout 是安全参数,写错了糊弄过去
        // 比拦下来更危险。
        if (!item["timeout"].is_number_integer() || item["timeout"].get<long long>() < 1 ||
            item["timeout"].get<long long>() > 600) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where +
                                   ".timeout 必须是 1..600 的整数(秒)");
        }
        handler.timeout_ms = static_cast<int>(item["timeout"].get<long long>() * 1000);
    }
    if (item.contains("async")) {
        if (!item["async"].is_boolean()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where + ".async 必须是 boolean");
        }
        handler.async = item["async"].get<bool>();
    }
    if (item.contains("statusMessage")) {
        if (!item["statusMessage"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where +
                                   ".statusMessage 必须是字符串");
        }
        handler.status_message = item["statusMessage"].get<std::string>();
    }
    if (item.contains("failure_policy")) {
        if (!item["failure_policy"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where +
                                   ".failure_policy 必须是字符串(warn 或 deny)");
        }
        const std::string policy = item["failure_policy"].get<std::string>();
        if (policy != "warn" && policy != "deny") {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where +
                                   ".failure_policy 只支持 warn 或 deny");
        }
        handler.failure_policy = policy;
    }
    return handler;
}

// hooks schema 2:解析一个事件键下的匹配组数组([{matcher, regex, hooks[]}]).
std::expected<std::vector<HookMatcherGroupConfig>, std::string> ParseHookMatcherGroups(
    const nlohmann::json& arr, hooks::HookEvent event, const std::string& file_path_for_error) {
    if (!arr.is_array()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 hooks." + std::string(ToString(event)) +
                               " 字段必须是数组");
    }
    std::vector<HookMatcherGroupConfig> groups;
    groups.reserve(arr.size());
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& item = arr[i];
        const std::string where = "hooks." + std::string(ToString(event)) + "[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where + " 必须是一个 JSON object");
        }
        HookMatcherGroupConfig group;
        if (item.contains("matcher")) {
            if (!item["matcher"].is_string()) {
                return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where +
                                       ".matcher 必须是字符串");
            }
            group.matcher = item["matcher"].get<std::string>();
        }
        if (item.contains("regex")) {
            if (!item["regex"].is_boolean()) {
                return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where + ".regex 必须是 boolean");
            }
            group.regex = item["regex"].get<bool>();
        }
        // 没有匹配字段的事件(UserPromptSubmit/Stop/Subagent*)写具体 matcher
        // = 配置错误,当场报,不静默吞。
        if (!EventHasMatcherField(event) && !group.matcher.empty() && group.matcher != "*") {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where +
                                   ".matcher 无从匹配:" + std::string(ToString(event)) +
                                   " 事件没有可匹配字段,matcher 只能省略或 \"*\"");
        }
        if (!item.contains("hooks") || !item["hooks"].is_array() || item["hooks"].empty()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 " + where +
                                   ".hooks 必须是非空数组");
        }
        group.hooks.reserve(item["hooks"].size());
        for (std::size_t h = 0; h < item["hooks"].size(); ++h) {
            auto parsed =
                ParseHookHandlerConfig(item["hooks"][h], where + ".hooks[" + std::to_string(h) + "]", file_path_for_error);
            if (!parsed.has_value()) {
                return std::unexpected(parsed.error());
            }
            group.hooks.push_back(std::move(*parsed));
        }
        group.source_path = file_path_for_error;
        groups.push_back(std::move(group));
    }
    return groups;
}

std::expected<HooksConfig, std::string> ParseHooksConfig(const nlohmann::json& hooks_json,
                                                           const std::string& file_path_for_error) {
    if (!hooks_json.is_object()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 hooks 字段必须是一个 JSON object");
    }

    HooksConfig config;
    // schema_version 可选;写了 2(或缺省)都行——旧四类键与 v2 事件键可以
    // 同文件共存(旧四类走 legacy adapter)。写了别的版本号报错,不猜。
    if (hooks_json.contains("schema_version")) {
        if (!hooks_json["schema_version"].is_number_integer()) {
            return std::unexpected("配置文件 " + file_path_for_error +
                                   " 里的 hooks.schema_version 必须是整数(当前支持 2)");
        }
        const long long version = hooks_json["schema_version"].get<long long>();
        if (version != 2) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 hooks.schema_version=" +
                                   std::to_string(version) + " 不认识(当前支持 2)");
        }
    }
    if (hooks_json.contains("pre_tool")) {
        auto parsed = ParseHookEntryArray(hooks_json["pre_tool"], "pre_tool", true, file_path_for_error);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        config.pre_tool = std::move(*parsed);
    }
    if (hooks_json.contains("post_tool")) {
        auto parsed = ParseHookEntryArray(hooks_json["post_tool"], "post_tool", true, file_path_for_error);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        config.post_tool = std::move(*parsed);
    }
    if (hooks_json.contains("session_start")) {
        auto parsed = ParseHookEntryArray(hooks_json["session_start"], "session_start", false, file_path_for_error);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        config.session_start = std::move(*parsed);
    }
    if (hooks_json.contains("session_end")) {
        auto parsed = ParseHookEntryArray(hooks_json["session_end"], "session_end", false, file_path_for_error);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        config.session_end = std::move(*parsed);
    }
    // v2 事件键(PascalCase)。不认得的键报错——事件名拼错、或照抄了
    // Claude Code 三十来类里我们没实现的事件,都当场拦下,不假装支持。
    for (auto it = hooks_json.begin(); it != hooks_json.end(); ++it) {
        const std::string& key = it.key();
        if (key == "schema_version" || key == "pre_tool" || key == "post_tool" || key == "session_start" ||
            key == "session_end") {
            continue;
        }
        hooks::HookEvent event{};
        if (!hooks::ParseHookEvent(key, event)) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 hooks." + key +
                                   " 不是认识的事件名(schema 2 用 PascalCase,如 PreToolUse;旧格式 pre_tool "
                                   "等四类仍受支持)");
        }
        auto parsed = ParseHookMatcherGroups(it.value(), event, file_path_for_error);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        config.events[event] = std::move(*parsed);
    }
    return config;
}

std::expected<std::vector<ProviderConfig>, std::string> ParseProvidersConfig(
    const nlohmann::json& providers_json, const std::string& file_path_for_error) {
    if (!providers_json.is_array()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 providers 字段必须是数组");
    }

    std::vector<ProviderConfig> providers;
    for (std::size_t i = 0; i < providers_json.size(); ++i) {
        const nlohmann::json& item = providers_json[i];
        const std::string prefix = "配置文件 " + file_path_for_error + " 里的 providers[" +
                                   std::to_string(i) + "]";
        if (!item.is_object()) {
            return std::unexpected(prefix + " 必须是 JSON object");
        }

        const auto require_string = [&](const char* field) -> std::expected<std::string, std::string> {
            if (!item.contains(field) || !item[field].is_string() || item[field].get<std::string>().empty()) {
                return std::unexpected(prefix + " 里的 " + field + " 字段必须是非空字符串");
            }
            return item[field].get<std::string>();
        };

        ProviderConfig provider;
        const auto name = require_string("name");
        if (!name.has_value()) {
            return std::unexpected(name.error());
        }
        provider.name = *name;
        const auto base_url = require_string("base_url");
        if (!base_url.has_value()) {
            return std::unexpected(base_url.error());
        }
        provider.base_url = *base_url;
        const auto wire = require_string("wire");
        if (!wire.has_value()) {
            return std::unexpected(wire.error());
        }
        const auto parsed_wire = ParseProviderWire(*wire);
        if (!parsed_wire.has_value()) {
            return std::unexpected(prefix + " 里的 wire 字段" + ": " + parsed_wire.error());
        }
        provider.wire = *parsed_wire;

        if (item.contains("key_env")) {
            if (!item["key_env"].is_string() || item["key_env"].get<std::string>().empty()) {
                return std::unexpected(prefix + " 里的 key_env 字段必须是非空字符串");
            }
            provider.key_env = item["key_env"].get<std::string>();
        }
        if (item.contains("api_key")) {
            if (!item["api_key"].is_string()) {
                return std::unexpected(prefix + " 里的 api_key 字段必须是字符串");
            }
            provider.api_key = item["api_key"].get<std::string>();
        }
        if (item.contains("model")) {
            if (!item["model"].is_string()) {
                return std::unexpected(prefix + " 里的 model 字段必须是字符串");
            }
            provider.model = item["model"].get<std::string>();
        }
        if (item.contains("model_reasoning_effort")) {
            if (!item["model_reasoning_effort"].is_string()) {
                return std::unexpected(prefix + " 里的 model_reasoning_effort 字段必须是字符串");
            }
            provider.model_reasoning_effort = item["model_reasoning_effort"].get<std::string>();
        }
        if (item.contains("context_window")) {
            std::string raw_window;
            if (item["context_window"].is_string()) {
                raw_window = item["context_window"].get<std::string>();
            } else if (item["context_window"].is_number_integer() || item["context_window"].is_number_unsigned()) {
                raw_window = std::to_string(item["context_window"].get<long long>());
            } else {
                return std::unexpected(prefix + " 里的 context_window 字段必须是字符串或数字");
            }
            const auto parsed_window = ParseContextWindowTokens(raw_window);
            if (!parsed_window.has_value()) {
                return std::unexpected(prefix + " 里的 context_window 字段: " + parsed_window.error());
            }
            provider.context_window_tokens = *parsed_window;
        }
        if (item.contains("native_web_search")) {
            if (!item["native_web_search"].is_boolean()) {
                return std::unexpected(prefix + " 里的 native_web_search 字段必须是布尔值");
            }
            provider.native_web_search = item["native_web_search"].get<bool>();
        }
        if (item.contains("stream_usage")) {
            if (!item["stream_usage"].is_boolean()) {
                return std::unexpected(prefix + " 里的 stream_usage 字段必须是布尔值");
            }
            provider.stream_usage = item["stream_usage"].get<bool>();
        }
        if (item.contains("reasoning_replay")) {
            if (!item["reasoning_replay"].is_string()) {
                return std::unexpected(prefix + " 里的 reasoning_replay 字段必须是字符串");
            }
            const std::string replay = item["reasoning_replay"].get<std::string>();
            if (replay != "never" && replay != "tool_episode") {
                return std::unexpected(prefix + " 里的 reasoning_replay 只认 never/tool_episode: " + replay);
            }
            provider.reasoning_replay = replay;
        }
        if (item.contains("extra_body")) {
            // 不直接复用 ParseExtraBodyConfig——那个函数的报错信息自己拼了
            // 一遍"配置文件 xxx 里的",这里的 prefix 已经是
            // "配置文件 xxx 里的 providers[i]",直接套上去会把路径重复念
            // 两遍,不如就地判一下。
            if (!item["extra_body"].is_object()) {
                return std::unexpected(prefix + " 里的 extra_body 字段必须是一个 JSON object");
            }
            provider.extra_body = item["extra_body"];
        }
        if (item.contains("extra_headers")) {
            if (!item["extra_headers"].is_object()) {
                return std::unexpected(prefix + " 里的 extra_headers 字段必须是一个 JSON object");
            }
            for (auto it = item["extra_headers"].begin(); it != item["extra_headers"].end(); ++it) {
                if (!it.value().is_string()) {
                    return std::unexpected(prefix + " 里的 extra_headers." + it.key() + " 的值必须是字符串");
                }
                provider.extra_headers.emplace(it.key(), it.value().get<std::string>());
            }
        }

        const auto valid = ValidateProviderConfig(provider);
        if (!valid.has_value()) {
            return std::unexpected(prefix + ": " + valid.error());
        }
        if (FindProvider(providers, provider.name) != nullptr) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 providers 名字重复: " + provider.name);
        }
        providers.push_back(std::move(provider));
    }
    return providers;
}

std::expected<FileConfig, std::string> ParseFileConfigJson(const std::string& json_text,
                                                             const std::string& file_path_for_error) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::parse_error& e) {
        return std::unexpected("配置文件 " + file_path_for_error + " 不是合法 JSON: " + e.what());
    }

    if (!parsed.is_object()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 顶层必须是一个 JSON object(花括号包起来的那种)");
    }

    FileConfig config;
    config.source_path = file_path_for_error;

    if (parsed.contains("wire")) {
        if (!parsed["wire"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 wire 字段必须是字符串");
        }
        config.wire = parsed["wire"].get<std::string>();
    }
    if (parsed.contains("base_url")) {
        if (!parsed["base_url"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 base_url 字段必须是字符串");
        }
        config.base_url = parsed["base_url"].get<std::string>();
    }
    if (parsed.contains("api_key")) {
        if (!parsed["api_key"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 api_key 字段必须是字符串");
        }
        config.api_key = parsed["api_key"].get<std::string>();
    }
    if (parsed.contains("model")) {
        if (!parsed["model"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 model 字段必须是字符串");
        }
        config.model = parsed["model"].get<std::string>();
    }
    if (parsed.contains("active_provider")) {
        if (!parsed["active_provider"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error +
                                    " 里的 active_provider 字段必须是字符串");
        }
        config.active_provider = parsed["active_provider"].get<std::string>();
    }
    if (parsed.contains("theme")) {
        if (!parsed["theme"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 theme 字段必须是字符串");
        }
        config.theme = parsed["theme"].get<std::string>();
    }
    if (parsed.contains("language")) {
        if (!parsed["language"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 language 字段必须是字符串");
        }
        config.language = parsed["language"].get<std::string>();
    }
    if (parsed.contains("system_prompt_file")) {
        if (!parsed["system_prompt_file"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 system_prompt_file 字段必须是字符串");
        }
        config.system_prompt_file = parsed["system_prompt_file"].get<std::string>();
    }
    if (parsed.contains("context_window")) {
        const auto& field = parsed["context_window"];
        if (field.is_string()) {
            config.context_window = field.get<std::string>();
        } else if (field.is_number_integer() || field.is_number_unsigned()) {
            config.context_window = std::to_string(field.get<long long>());
        } else {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 context_window 字段必须是字符串或数字");
        }
    }
    if (parsed.contains("compact_model")) {
        if (!parsed["compact_model"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 compact_model 字段必须是字符串");
        }
        config.compact_model = parsed["compact_model"].get<std::string>();
    }
    if (parsed.contains("think")) {
        if (!parsed["think"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 think 字段必须是字符串");
        }
        config.think = parsed["think"].get<std::string>();
    }
    if (parsed.contains("soul")) {
        if (!parsed["soul"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 soul 字段必须是字符串");
        }
        config.soul = parsed["soul"].get<std::string>();
    }
    if (parsed.contains("max_context_chars")) {
        const auto& field = parsed["max_context_chars"];
        if (!field.is_number_integer() && !field.is_number_unsigned()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 max_context_chars 字段必须是正整数");
        }
        const long long value = field.get<long long>();
        if (value <= 0) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 max_context_chars 字段必须是正整数");
        }
        config.max_context_chars = static_cast<std::size_t>(value);
    }
    // 主预算键双读(命名规范第二批):新名 max_steps_per_turn 优先,旧名
    // max_turns 兼容。两键同现的取舍与提示在 MergeConfig 统一判定,这里
    // 只管把各自的值读出来。跟其余字段(比如 max_context_chars)不一样:
    // 不报错,类型不对或者是负数都静默跳过(留 nullopt,MergeConfig 那一级
    // 就当没写,往下一级/默认值找)。0 是合法值——显式声明"无上限"(跟
    // 不写这个字段效果一样);只有负数才当手滑写错。这是条"救命阀"字段,
    // 配置文件写错不该把整个启动拦下来。
    if (parsed.contains("max_steps_per_turn")) {
        const auto& field = parsed["max_steps_per_turn"];
        if ((field.is_number_integer() || field.is_number_unsigned()) && field.get<long long>() >= 0) {
            config.max_steps_per_turn = static_cast<int>(field.get<long long>());
        }
    }
    if (parsed.contains("max_turns")) {
        const auto& field = parsed["max_turns"];
        if ((field.is_number_integer() || field.is_number_unsigned()) && field.get<long long>() >= 0) {
            config.max_turns = static_cast<int>(field.get<long long>());
        }
    }
    if (parsed.contains("tool_search_threshold")) {
        const auto& field = parsed["tool_search_threshold"];
        if (!field.is_number_integer() && !field.is_number_unsigned()) {
            return std::unexpected("配置文件 " + file_path_for_error +
                                    " 里的 tool_search_threshold 字段必须是非负整数(0 = 永不延迟)");
        }
        const long long value = field.get<long long>();
        if (value < 0) {
            return std::unexpected("配置文件 " + file_path_for_error +
                                    " 里的 tool_search_threshold 字段必须是非负整数(0 = 永不延迟)");
        }
        config.tool_search_threshold = static_cast<int>(value);
    }
    if (parsed.contains("memory")) {
        const auto& field = parsed["memory"];
        if (!field.is_object()) {
            return std::unexpected("配置文件 " + file_path_for_error +
                                   " 里的 memory 字段必须是 JSON object");
        }
        MemoryFileConfig memory;
        const auto parse_bool = [&](const char* name, std::optional<bool>& target)
            -> std::expected<void, std::string> {
            if (!field.contains(name)) return {};
            if (!field[name].is_boolean()) {
                return std::unexpected("配置文件 " + file_path_for_error + " 里的 memory." + name +
                                       " 必须是布尔值");
            }
            target = field[name].get<bool>();
            return {};
        };
        const auto parse_positive = [&](const char* name, std::optional<std::size_t>& target)
            -> std::expected<void, std::string> {
            if (!field.contains(name)) return {};
            if ((!field[name].is_number_integer() && !field[name].is_number_unsigned()) ||
                field[name].get<long long>() <= 0) {
                return std::unexpected("配置文件 " + file_path_for_error + " 里的 memory." + name +
                                       " 必须是正整数");
            }
            target = static_cast<std::size_t>(field[name].get<long long>());
            return {};
        };
        if (auto result = parse_bool("enabled", memory.enabled); !result.has_value())
            return std::unexpected(result.error());
        if (auto result = parse_bool("use", memory.use); !result.has_value())
            return std::unexpected(result.error());
        if (auto result = parse_bool("generate", memory.generate); !result.has_value())
            return std::unexpected(result.error());
        if (field.contains("learn")) {
            if (!field["learn"].is_string()) {
                return std::unexpected("配置文件 " + file_path_for_error +
                                       " 里的 memory.learn 必须是字符串(off|review|auto)");
            }
            const std::string learn = field["learn"].get<std::string>();
            if (learn != "off" && learn != "review" && learn != "auto") {
                return std::unexpected("配置文件 " + file_path_for_error +
                                       " 里的 memory.learn 只认 off、review 或 auto");
            }
            memory.learn = learn;
        }
        if (auto result = parse_positive("max_index_bytes", memory.max_index_bytes); !result.has_value())
            return std::unexpected(result.error());
        if (auto result = parse_positive("max_retrieval_bytes", memory.max_retrieval_bytes); !result.has_value())
            return std::unexpected(result.error());
        if (auto result = parse_positive("max_results", memory.max_results); !result.has_value())
            return std::unexpected(result.error());
        config.memory = std::move(memory);
    }
    if (parsed.contains("connect_timeout_ms")) {
        const auto& field = parsed["connect_timeout_ms"];
        if ((!field.is_number_integer() && !field.is_number_unsigned()) || field.get<long long>() <= 0) {
            return std::unexpected("配置文件 " + file_path_for_error +
                                    " 里的 connect_timeout_ms 字段必须是正整数(单位毫秒)");
        }
        config.connect_timeout_ms = static_cast<int>(field.get<long long>());
    }
    if (parsed.contains("stream_idle_timeout_secs")) {
        const auto& field = parsed["stream_idle_timeout_secs"];
        if ((!field.is_number_integer() && !field.is_number_unsigned()) || field.get<long long>() <= 0) {
            return std::unexpected("配置文件 " + file_path_for_error +
                                    " 里的 stream_idle_timeout_secs 字段必须是正整数(单位秒)");
        }
        config.stream_idle_timeout_secs = static_cast<int>(field.get<long long>());
    }
    if (parsed.contains("request_timeout_secs")) {
        const auto& field = parsed["request_timeout_secs"];
        if ((!field.is_number_integer() && !field.is_number_unsigned()) || field.get<long long>() <= 0) {
            return std::unexpected("配置文件 " + file_path_for_error +
                                    " 里的 request_timeout_secs 字段必须是正整数(单位秒)");
        }
        config.request_timeout_secs = static_cast<int>(field.get<long long>());
    }
    if (parsed.contains("hooks")) {
        auto hooks_result = ParseHooksConfig(parsed["hooks"], file_path_for_error);
        if (!hooks_result.has_value()) {
            return std::unexpected(hooks_result.error());
        }
        config.hooks = std::move(*hooks_result);
    }
    if (parsed.contains("mcpServers")) {
        auto mcp_result = ParseMcpServersConfig(parsed["mcpServers"], file_path_for_error);
        if (!mcp_result.has_value()) {
            return std::unexpected(mcp_result.error());
        }
        config.mcp_servers = std::move(*mcp_result);
    }
    if (parsed.contains("search")) {
        auto search_result = ParseSearchConfig(parsed["search"], file_path_for_error);
        if (!search_result.has_value()) {
            return std::unexpected(search_result.error());
        }
        config.search = std::move(*search_result);
    }
    if (parsed.contains("lsp")) {
        auto lsp_result = ParseLspServersConfig(parsed["lsp"], file_path_for_error);
        if (!lsp_result.has_value()) {
            return std::unexpected(lsp_result.error());
        }
        config.lsp_servers = std::move(*lsp_result);
    }
    if (parsed.contains("status_panel")) {
        auto panel_result = ParseStatusPanelConfig(parsed["status_panel"], file_path_for_error);
        if (!panel_result.has_value()) {
            return std::unexpected(panel_result.error());
        }
        config.status_panel = std::move(*panel_result);
    }
    // subagent 段双读(命名规范第二批):新名 {"subagent": {"max_steps_per_turn": N}}
    // 优先,旧名 {"subagent": {"max_turns": N}} 兼容;段不是 object、字段
    // 类型不对、负数、超上限(1000000),一律静默跳过(留 nullopt,运行时
    // 继承主预算)——待遇同主预算本身:救命阀字段,配置写错不拦人开工。
    if (parsed.contains("subagent") && parsed["subagent"].is_object()) {
        const auto& subagent = parsed["subagent"];
        if (subagent.contains("max_steps_per_turn") && subagent["max_steps_per_turn"].is_number_integer()) {
            const long long steps = subagent["max_steps_per_turn"].get<long long>();
            if (steps >= 0 && steps <= static_cast<long long>(1000000)) {
                config.subagent_max_steps_per_turn = static_cast<int>(steps);
            }
        }
        if (subagent.contains("max_turns") && subagent["max_turns"].is_number_integer()) {
            const long long turns = subagent["max_turns"].get<long long>();
            if (turns >= 0 && turns <= static_cast<long long>(1000000)) {
                config.subagent_max_turns = static_cast<int>(turns);
            }
        }
    }
    if (parsed.contains("extra_body")) {
        auto extra_body_result = ParseExtraBodyConfig(parsed["extra_body"], file_path_for_error);
        if (!extra_body_result.has_value()) {
            return std::unexpected(extra_body_result.error());
        }
        config.extra_body = std::move(*extra_body_result);
    }
    if (parsed.contains("extra_headers")) {
        auto extra_headers_result = ParseExtraHeadersConfig(parsed["extra_headers"], file_path_for_error);
        if (!extra_headers_result.has_value()) {
            return std::unexpected(extra_headers_result.error());
        }
        config.extra_headers = std::move(*extra_headers_result);
    }
    if (parsed.contains("providers")) {
        auto providers_result = ParseProvidersConfig(parsed["providers"], file_path_for_error);
        if (!providers_result.has_value()) {
            return std::unexpected(providers_result.error());
        }
        config.providers = std::move(*providers_result);
    }

    return config;
}

ConfigMigrationOutcome MigrateConfigFileIfNeeded(const std::string& old_path_str, const std::string& new_path_str) {
    namespace fs = std::filesystem;
    const fs::path old_path(old_path_str);
    const fs::path new_path(new_path_str);

    std::error_code ec;
    if (fs::exists(new_path, ec) && !ec) {
        return ConfigMigrationOutcome{new_path_str, std::nullopt};
    }

    ec.clear();
    if (!fs::exists(old_path, ec) || ec) {
        return ConfigMigrationOutcome{std::string(), std::nullopt};
    }

    ec.clear();
    fs::create_directories(new_path.parent_path(), ec);
    if (ec) {
        return ConfigMigrationOutcome{old_path_str, "配置迁移失败(建目录 " + new_path.parent_path().string() +
                                                          " 出错: " + ec.message() + "),继续使用旧配置 " + old_path_str};
    }

    ec.clear();
    fs::rename(old_path, new_path, ec);
    if (ec) {
        // rename 失败多半是跨盘符导致的(比如 old/new 不在同一个磁盘分区),
        // 退化成复制 + 删除旧文件。
        ec.clear();
        fs::copy_file(old_path, new_path, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return ConfigMigrationOutcome{old_path_str,
                                           "配置迁移失败(" + ec.message() + "),继续使用旧配置 " + old_path_str};
        }
        std::error_code remove_ec;
        fs::remove(old_path, remove_ec);  // 尽力删除,删不掉不影响"已经迁移成功"这件事,不报错
    }

    return ConfigMigrationOutcome{new_path_str, "配置已迁移到 " + new_path_str};
}

namespace {

// 真正读盘 + 解析一个已知存在的配置文件路径。
std::expected<FileConfig, std::string> ReadAndParseConfigFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("配置文件 " + path.string() + " 存在,但打不开(检查一下权限)");
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return ParseFileConfigJson(buffer.str(), path.string());
}

}  // namespace

namespace {

// 在一个 base 目录里找配置:先新位置,没有再旧位置(命中就迁移到新位置)。
// 都没有返回 std::optional<FileConfig>(std::nullopt)(不算错);解析失败
// 返回错误(带路径)。迁移通知(若有)写在 FileConfig::migration_notice 上。
std::expected<std::optional<FileConfig>, std::string> LoadConfigFromBaseDir(const std::filesystem::path& base) {
    namespace fs = std::filesystem;
    const fs::path new_path = NewConfigPathFor(base);
    std::error_code ec;
    if (fs::exists(new_path, ec) && !ec) {
        const auto parsed = ReadAndParseConfigFile(new_path);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        return std::optional<FileConfig>(*parsed);
    }

    ec.clear();
    const fs::path old_path = OldConfigPathFor(base);
    if (!fs::exists(old_path, ec) || ec) {
        return std::optional<FileConfig>(std::nullopt);
    }
    const ConfigMigrationOutcome outcome = MigrateConfigFileIfNeeded(old_path.string(), new_path.string());
    const fs::path effective = outcome.effective_path.empty() ? old_path : fs::path(outcome.effective_path);
    auto parsed = ReadAndParseConfigFile(effective);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    parsed->migration_notice = outcome.notice;
    return std::optional<FileConfig>(*parsed);
}

}  // namespace

std::expected<LoadedFileConfigs, std::string> LoadFileConfigs() {
    namespace fs = std::filesystem;
    LoadedFileConfigs out;

    const fs::path cwd = fs::current_path();
    const auto home = HomeDir();

    // 项目级:<cwd>/.lubancode/...
    auto project = LoadConfigFromBaseDir(cwd);
    if (!project.has_value()) {
        return std::unexpected(project.error());
    }
    out.project = *project;

    // 全局:<主目录>/.lubancode/...,但 cwd 就是主目录时不重复读(否则同一份
    // 文件读两遍、来源标记打架)——那种情形只当项目级一份。
    bool cwd_is_home = false;
    if (home.has_value()) {
        std::error_code ec;
        cwd_is_home = fs::equivalent(cwd, fs::path(*home), ec) && !ec;
    }
    if (home.has_value() && !cwd_is_home) {
        auto global = LoadConfigFromBaseDir(fs::path(*home));
        if (!global.has_value()) {
            return std::unexpected(global.error());
        }
        out.global = *global;
    }

    // 迁移通知合并:项目级、全局各自可能有一行,拼一起(都没有就 nullopt)。
    std::vector<std::string> notices;
    if (out.project.has_value() && out.project->migration_notice.has_value()) {
        notices.push_back(*out.project->migration_notice);
    }
    if (out.global.has_value() && out.global->migration_notice.has_value()) {
        notices.push_back(*out.global->migration_notice);
    }
    if (!notices.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < notices.size(); ++i) {
            if (i != 0) {
                joined += "\n";
            }
            joined += notices[i];
        }
        out.migration_notice = joined;
    }
    return out;
}

namespace {

// 配置文件那一级的取值(项目级优先,回退全局)。value 指向命中的值(没命中
// 是 nullptr),source 记是项目级还是全局,path 指向命中那份文件的 source_path
// (拼 wire/context_window 报错时用)。
struct FileStrPick {
    const std::string* value = nullptr;
    Source source = Source::Default;
    const std::string* path = nullptr;
};

FileStrPick PickFileStr(const std::optional<FileConfig>& project_file,
                        const std::optional<FileConfig>& global_file,
                        std::optional<std::string> FileConfig::* field) {
    if (project_file.has_value() && ((*project_file).*field).has_value()) {
        return {&*((*project_file).*field), Source::ProjectConfigFile, &project_file->source_path};
    }
    if (global_file.has_value() && ((*global_file).*field).has_value()) {
        return {&*((*global_file).*field), Source::GlobalConfigFile, &global_file->source_path};
    }
    return {};
}

}  // namespace

std::expected<ConfigResult, std::string> MergeConfig(const LubancodeEnvValues& lubancode_env,
                                                       const std::optional<FileConfig>& project_file,
                                                       const std::optional<FileConfig>& global_file,
                                                       const GenericEnvValues& generic_env) {
    ConfigResult result;

    // 配置文件那一级统一走这个:项目级优先,回退全局(见 PickFileStr)。
    const auto pick = [&](std::optional<std::string> FileConfig::* field) {
        return PickFileStr(project_file, global_file, field);
    };

    // ---- 第一步:解出 wire。专属 env(1 级)> 项目级(2 级)> 全局(3 级)>
    // 默认值——通用环境变量里没有"wire"这一说。 ----
    std::string wire_str;
    Source wire_source = Source::Default;
    std::string wire_error_origin;  // 报错时说清楚这个坏值是从哪儿来的

    if (lubancode_env.wire.has_value()) {
        wire_str = *lubancode_env.wire;
        wire_source = Source::LubancodeEnv;
        wire_error_origin = "环境变量 LUBANCODE_WIRE";
    } else if (const auto p = pick(&FileConfig::wire); p.value != nullptr) {
        wire_str = *p.value;
        wire_source = p.source;
        wire_error_origin = "配置文件 " + *p.path + " 里的 wire 字段";
    } else {
        wire_str = "anthropic";
        wire_source = Source::Default;
    }

    const auto parsed_wire = ParseProviderWire(wire_str);
    if (!parsed_wire.has_value()) {
        return std::unexpected(wire_error_origin + " " + parsed_wire.error());
    }
    const Wire wire = *parsed_wire;
    result.config.wire = wire;
    result.sources.wire = wire_source;

    const bool is_anthropic = (wire == Wire::Anthropic);
    // base_url、model 没有内置默认值(lubancode 不绑死哪一家模型服务)——
    // 各级都没配到时就是空串,来源记 Default,留给上层(初次配置向导 /
    // RequireConfigured)去拦。
    const std::string default_base_url;
    const std::string default_model;
    const std::optional<std::string>& generic_base_url =
        is_anthropic ? generic_env.anthropic_base_url : generic_env.openai_base_url;
    const std::optional<std::string>& generic_api_key =
        is_anthropic ? generic_env.anthropic_auth_token : generic_env.openai_api_key;
    const std::optional<std::string>& generic_model =
        is_anthropic ? generic_env.anthropic_model : generic_env.openai_model;

    // ---- base_url:env > 项目级 > 全局 > 通用 env(按 wire 挑)> 默认值 ----
    if (lubancode_env.base_url.has_value()) {
        result.config.base_url = *lubancode_env.base_url;
        result.sources.base_url = Source::LubancodeEnv;
    } else if (const auto p = pick(&FileConfig::base_url); p.value != nullptr) {
        result.config.base_url = *p.value;
        result.sources.base_url = p.source;
    } else if (generic_base_url.has_value()) {
        result.config.base_url = *generic_base_url;
        result.sources.base_url = Source::GenericEnv;
    } else {
        result.config.base_url = default_base_url;
        result.sources.base_url = Source::Default;
    }

    // ---- model:同上 ----
    if (lubancode_env.model.has_value()) {
        result.config.model = *lubancode_env.model;
        result.sources.model = Source::LubancodeEnv;
    } else if (const auto p = pick(&FileConfig::model); p.value != nullptr) {
        result.config.model = *p.value;
        result.sources.model = p.source;
    } else if (generic_model.has_value()) {
        result.config.model = *generic_model;
        result.sources.model = Source::GenericEnv;
    } else {
        result.config.model = default_model;
        result.sources.model = Source::Default;
    }

    // ---- api_key:同上,但没有内置默认值——都没有时留空,来源记成 Default,
    // 不在这里报错(报错交给 RequireApiKey,见该函数注释)。 ----
    if (lubancode_env.api_key.has_value()) {
        result.config.auth_token = *lubancode_env.api_key;
        result.sources.auth_token = Source::LubancodeEnv;
    } else if (const auto p = pick(&FileConfig::api_key); p.value != nullptr) {
        result.config.auth_token = *p.value;
        result.sources.auth_token = p.source;
    } else if (generic_api_key.has_value()) {
        result.config.auth_token = *generic_api_key;
        result.sources.auth_token = Source::GenericEnv;
    } else {
        result.config.auth_token.clear();
        result.sources.auth_token = Source::Default;
    }

    // ---- max_context_chars:env > 项目级 > 全局 > 默认值,没有通用 env 这一级 ----
    if (lubancode_env.max_context_chars.has_value()) {
        result.config.max_context_chars = *lubancode_env.max_context_chars;
        result.sources.max_context_chars = Source::LubancodeEnv;
    } else if (project_file.has_value() && project_file->max_context_chars.has_value()) {
        result.config.max_context_chars = *project_file->max_context_chars;
        result.sources.max_context_chars = Source::ProjectConfigFile;
    } else if (global_file.has_value() && global_file->max_context_chars.has_value()) {
        result.config.max_context_chars = *global_file->max_context_chars;
        result.sources.max_context_chars = Source::GlobalConfigFile;
    } else {
        result.config.max_context_chars = kDefaultMaxContextChars;
        result.sources.max_context_chars = Source::Default;
    }

    // ---- max_steps_per_turn(旧名 max_turns):env > 项目级 > 全局 > 默认值,
    // 没有通用 env 这一级(待遇同 max_context_chars)。负数/非法值已经在解析
    // 阶段被过滤(不会落进 FileConfig/LubancodeEnvValues),0(显式无上限)是
    // 合法值,这里只管按优先级挑;都没配到时默认值 kDefaultMaxStepsPerTurn
    // 本身也是 0(无上限)。 ----
    //
    // 兼容期双读(命名规范第二批):新名优先;新旧同现同值按新名收账并提
    // 示弃用;同现异值明报冲突(仍取新名,但把两个值都摆出来,不暗取);
    // 只出现旧名时映射到同一字段并打一次性弃用提示。来源账(Source)照记
    // 读到的是哪一级。 ----
    const auto resolve_steps_per_turn = [&result](const std::optional<int>& new_value,
                                                   const std::optional<int>& old_value,
                                                   const std::string& origin_label) -> std::optional<int> {
        if (new_value.has_value() && old_value.has_value()) {
            if (*new_value != *old_value) {
                result.deprecation_notices.push_back(
                    "[配置] " + origin_label + " 里 max_steps_per_turn=" + std::to_string(*new_value) +
                    " 与旧名 max_turns=" + std::to_string(*old_value) +
                    " 冲突:已采用 max_steps_per_turn=" + std::to_string(*new_value) +
                    "。请删掉 max_turns 或把两者改成同一个值。");
            } else {
                result.deprecation_notices.push_back("[配置] " + origin_label + " 里 max_turns=" +
                                                     std::to_string(*old_value) +
                                                     " 已弃用,与 max_steps_per_turn 同值,按新名收账。");
            }
            return new_value;
        }
        if (old_value.has_value()) {
            result.deprecation_notices.push_back(
                "[配置] " + origin_label + " 里 max_turns=" + std::to_string(*old_value) +
                " 已弃用,新名是 max_steps_per_turn(0 = 不限步,语义不变);旧名将在未来版本移除。");
            return old_value;
        }
        return new_value;
    };

    std::optional<int> env_steps;
    if (lubancode_env.max_steps_per_turn.has_value() || lubancode_env.max_turns.has_value()) {
        env_steps = resolve_steps_per_turn(lubancode_env.max_steps_per_turn, lubancode_env.max_turns,
                                           "环境变量 LUBANCODE_MAX_STEPS_PER_TURN / LUBANCODE_MAX_TURNS");
    }
    std::optional<int> project_steps;
    if (project_file.has_value() &&
        (project_file->max_steps_per_turn.has_value() || project_file->max_turns.has_value())) {
        project_steps = resolve_steps_per_turn(project_file->max_steps_per_turn, project_file->max_turns,
                                               "项目级配置 " + project_file->source_path);
    }
    std::optional<int> global_steps;
    if (global_file.has_value() &&
        (global_file->max_steps_per_turn.has_value() || global_file->max_turns.has_value())) {
        global_steps = resolve_steps_per_turn(global_file->max_steps_per_turn, global_file->max_turns,
                                              "全局配置 " + global_file->source_path);
    }
    if (env_steps.has_value()) {
        result.config.max_steps_per_turn = *env_steps;
        result.sources.max_steps_per_turn = Source::LubancodeEnv;
    } else if (project_steps.has_value()) {
        result.config.max_steps_per_turn = *project_steps;
        result.sources.max_steps_per_turn = Source::ProjectConfigFile;
    } else if (global_steps.has_value()) {
        result.config.max_steps_per_turn = *global_steps;
        result.sources.max_steps_per_turn = Source::GlobalConfigFile;
    } else {
        result.config.max_steps_per_turn = kDefaultMaxStepsPerTurn;
        result.sources.max_steps_per_turn = Source::Default;
    }

    // ---- theme:env > 项目级 > 全局 > 默认值,没有通用 env 这一级 ----
    if (lubancode_env.theme.has_value()) {
        result.config.theme = *lubancode_env.theme;
        result.sources.theme = Source::LubancodeEnv;
    } else if (const auto p = pick(&FileConfig::theme); p.value != nullptr) {
        result.config.theme = *p.value;
        result.sources.theme = p.source;
    } else {
        result.config.theme = kDefaultTheme;
        result.sources.theme = Source::Default;
    }

    // ---- language(i18n):env(LUBANCODE_LANG)> 项目级 > 全局 > 默认值(空串
    // = 跟系统),没有通用 env 这一级。 ----
    if (lubancode_env.language.has_value()) {
        result.config.language = *lubancode_env.language;
        result.sources.language = Source::LubancodeEnv;
    } else if (const auto p = pick(&FileConfig::language); p.value != nullptr) {
        result.config.language = *p.value;
        result.sources.language = p.source;
    } else {
        result.config.language.clear();
        result.sources.language = Source::Default;
    }

    // ---- system_prompt_file:同上,env > 项目级 > 全局 > 默认值(空串) ----
    if (lubancode_env.system_prompt_file.has_value()) {
        result.config.system_prompt_file = *lubancode_env.system_prompt_file;
        result.sources.system_prompt_file = Source::LubancodeEnv;
    } else if (const auto p = pick(&FileConfig::system_prompt_file); p.value != nullptr) {
        result.config.system_prompt_file = *p.value;
        result.sources.system_prompt_file = p.source;
    } else {
        result.config.system_prompt_file.clear();
        result.sources.system_prompt_file = Source::Default;
    }

    // ---- context_window:env > 项目级 > 全局 > 默认值,没有通用 env 这一级。
    // 取值要过 ParseContextWindowTokens 校验,坏值直接报错。 ----
    std::string context_window_error_origin;
    std::optional<std::string> context_window_raw;
    if (lubancode_env.context_window.has_value()) {
        context_window_raw = *lubancode_env.context_window;
        context_window_error_origin = "环境变量 LUBANCODE_CONTEXT_WINDOW";
        result.sources.context_window_tokens = Source::LubancodeEnv;
    } else if (const auto p = pick(&FileConfig::context_window); p.value != nullptr) {
        context_window_raw = *p.value;
        context_window_error_origin = "配置文件 " + *p.path + " 里的 context_window 字段";
        result.sources.context_window_tokens = p.source;
    }
    if (context_window_raw.has_value()) {
        const auto parsed_window = ParseContextWindowTokens(*context_window_raw);
        if (!parsed_window.has_value()) {
            return std::unexpected(context_window_error_origin + ": " + parsed_window.error());
        }
        result.config.context_window_tokens = *parsed_window;
    } else {
        result.config.context_window_tokens = kDefaultContextWindowTokens;
        result.sources.context_window_tokens = Source::Default;
    }

    // ---- compact_model:env > 项目级 > 全局 > 默认值(空串 = 跟当前会话模型
    // 一致),没有通用 env 这一级、没有校验。 ----
    if (lubancode_env.compact_model.has_value()) {
        result.config.compact_model = *lubancode_env.compact_model;
        result.sources.compact_model = Source::LubancodeEnv;
    } else if (const auto p = pick(&FileConfig::compact_model); p.value != nullptr) {
        result.config.compact_model = *p.value;
        result.sources.compact_model = p.source;
    } else {
        result.config.compact_model.clear();
        result.sources.compact_model = Source::Default;
    }

    // ---- think:env > 项目级 > 全局 > 默认值(空串 = 不发这个参数),没有
    // 通用 env 这一级。M10 放开成任意字符串,原样存不做大小写归一化(理由见
    // api/anthropic/client.cpp 的 BuildThinkingJson)。 ----
    if (lubancode_env.think.has_value()) {
        result.config.think = *lubancode_env.think;
        result.sources.think = Source::LubancodeEnv;
    } else if (const auto p = pick(&FileConfig::think); p.value != nullptr) {
        result.config.think = *p.value;
        result.sources.think = p.source;
    } else {
        result.config.think.clear();
        result.sources.think = Source::Default;
    }

    // ---- soul:env(LUBANCODE_SOUL)> 项目级 > 全局 > 默认值(空串 = 用主目录
    // SOUL.md),没有通用 env 这一级。 ----
    if (lubancode_env.soul.has_value()) {
        result.config.soul = *lubancode_env.soul;
        result.sources.soul = Source::LubancodeEnv;
    } else if (const auto p = pick(&FileConfig::soul); p.value != nullptr) {
        result.config.soul = *p.value;
        result.sources.soul = p.source;
    } else {
        result.config.soul.clear();
        result.sources.soul = Source::Default;
    }

    // ---- tool_search_threshold:项目级 > 全局 > 默认值(20),没有环境变量
    // 这一级。取值校验在 ParseFileConfigJson 里做过了。 ----
    if (project_file.has_value() && project_file->tool_search_threshold.has_value()) {
        result.config.tool_search_threshold = *project_file->tool_search_threshold;
        result.sources.tool_search_threshold = Source::ProjectConfigFile;
    } else if (global_file.has_value() && global_file->tool_search_threshold.has_value()) {
        result.config.tool_search_threshold = *global_file->tool_search_threshold;
        result.sources.tool_search_threshold = Source::GlobalConfigFile;
    } else {
        result.config.tool_search_threshold = kDefaultToolSearchThreshold;
        result.sources.tool_search_threshold = Source::Default;
    }

    // ---- memory:默认关闭。只有用户主目录的全局配置能打开；受版本控制的
    // 项目 config.json 只能在全局已打开之后收窄 use/generate/learn/预算，
    // 或显式关闭。陌生仓库不能替用户开启聊天提取,更不能替用户升到 auto。 ----
    // learn 档位按"只收窄"合并:off(0) < review(1) < auto(2),项目级取
    // min(全局, 项目);老 generate=false 压成 off。auto 只能出自全局配置。
    const auto learn_rank = [](const std::string& learn) {
        if (learn == "auto") return 2;
        if (learn == "review") return 1;
        return 0;
    };
    const auto rank_to_learn = [](int rank) {
        if (rank >= 2) return std::string("auto");
        if (rank == 1) return std::string("review");
        return std::string("off");
    };
    const auto apply_memory_fields = [learn_rank, rank_to_learn](MemoryConfig& target,
                                                                 const MemoryFileConfig& source,
                                                                 bool allow_enable, bool global_level) {
        if (source.enabled.has_value()) {
            if (!*source.enabled) target.enabled = false;
            else if (allow_enable) target.enabled = true;
        }
        if (source.use.has_value()) target.use = *source.use;
        if (source.generate.has_value()) target.generate = *source.generate;
        if (source.learn.has_value()) {
            if (global_level) {
                target.learn = *source.learn;
            } else {
                // 项目级只收窄:取两档中更低的那档。
                const int rank = (std::min)(learn_rank(target.learn), learn_rank(*source.learn));
                target.learn = rank_to_learn(rank);
            }
        }
        if (source.generate.has_value() && !*source.generate) {
            target.learn = "off";  // 老写法 generate=false 等价 learn=off
        }
        if (source.max_index_bytes.has_value()) target.max_index_bytes = *source.max_index_bytes;
        if (source.max_retrieval_bytes.has_value()) target.max_retrieval_bytes = *source.max_retrieval_bytes;
        if (source.max_results.has_value()) target.max_results = *source.max_results;
    };
    result.config.memory = MemoryConfig{};
    if (global_file.has_value() && global_file->memory.has_value()) {
        apply_memory_fields(result.config.memory, *global_file->memory, /*allow_enable=*/true, /*global_level=*/true);
        result.sources.memory = Source::GlobalConfigFile;
    }
    if (project_file.has_value() && project_file->memory.has_value()) {
        if (result.config.memory.enabled) {
            apply_memory_fields(result.config.memory, *project_file->memory, /*allow_enable=*/false,
                                /*global_level=*/false);
        }
        // 全局没开时，项目 enabled=true 不生效；enabled=false 仍如实保持关闭。
        result.sources.memory = Source::ProjectConfigFile;
    }

    // ---- M11(网络超时):connect_timeout_ms / stream_idle_timeout_secs /
    // request_timeout_secs 三个字段都是"项目级 > 全局 > 默认值",没有环境
    // 变量这一级(跟 tool_search_threshold 同样待遇)。取值校验在
    // ParseFileConfigJson 里做过了。 ----
    if (project_file.has_value() && project_file->connect_timeout_ms.has_value()) {
        result.config.connect_timeout_ms = *project_file->connect_timeout_ms;
        result.sources.connect_timeout_ms = Source::ProjectConfigFile;
    } else if (global_file.has_value() && global_file->connect_timeout_ms.has_value()) {
        result.config.connect_timeout_ms = *global_file->connect_timeout_ms;
        result.sources.connect_timeout_ms = Source::GlobalConfigFile;
    } else {
        result.config.connect_timeout_ms = kDefaultConnectTimeoutMs;
        result.sources.connect_timeout_ms = Source::Default;
    }

    if (project_file.has_value() && project_file->stream_idle_timeout_secs.has_value()) {
        result.config.stream_idle_timeout_secs = *project_file->stream_idle_timeout_secs;
        result.sources.stream_idle_timeout_secs = Source::ProjectConfigFile;
    } else if (global_file.has_value() && global_file->stream_idle_timeout_secs.has_value()) {
        result.config.stream_idle_timeout_secs = *global_file->stream_idle_timeout_secs;
        result.sources.stream_idle_timeout_secs = Source::GlobalConfigFile;
    } else {
        result.config.stream_idle_timeout_secs = kDefaultStreamIdleTimeoutSecs;
        result.sources.stream_idle_timeout_secs = Source::Default;
    }

    if (project_file.has_value() && project_file->request_timeout_secs.has_value()) {
        result.config.request_timeout_secs = *project_file->request_timeout_secs;
        result.sources.request_timeout_secs = Source::ProjectConfigFile;
    } else if (global_file.has_value() && global_file->request_timeout_secs.has_value()) {
        result.config.request_timeout_secs = *global_file->request_timeout_secs;
        result.sources.request_timeout_secs = Source::GlobalConfigFile;
    } else {
        result.config.request_timeout_secs = kDefaultRequestTimeoutSecs;
        result.sources.request_timeout_secs = Source::Default;
    }

    // ---- 对象型整段(hooks 除外):只从配置文件来,没有环境变量、没有
    // 内置默认值这两级。按"整段"回退——项目级写了就用项目级那一整段,
    // 否则用全局那一整段(不做键级混合,语义清楚)。hooks 是唯一例外,
    // 改成相加(见下一段)。 ----

    // ---- hooks:相加合并(Hooks 生命周期单,breaking change)。 ----
    // 旧行为是"项目级整段压过全局"——仓库配置能把用户/管理员的审计钩子
    // 整段顶走,是条安全洞。现在两层相加:旧四类数组拼接(用户级在前、
    // 项目级在后),v2 事件组同事件追加。项目配置删不掉 user/managed 的
    // 任何一条;执行排序按来源(user 先于 project),决策归并在 hooks 层。
    // 两边都有旧四类时打一次迁移说明(行为变了,得让用户看见)。
    {
        const bool project_has_hooks = project_file.has_value() && project_file->hooks.has_value();
        const bool global_has_hooks = global_file.has_value() && global_file->hooks.has_value();
        const bool project_has_legacy = project_has_hooks && (*project_file->hooks).HasLegacy();
        const bool global_has_legacy = global_has_hooks && (*global_file->hooks).HasLegacy();

        HooksConfig merged;
        if (global_has_hooks) {
            merged = *global_file->hooks;  // 用户级打底
        }
        if (project_has_hooks) {
            const HooksConfig& project_hooks = *project_file->hooks;
            // 旧四类拼接(用户级在前)。source_path 各自带,信任分级靠它。
            merged.pre_tool.insert(merged.pre_tool.end(), project_hooks.pre_tool.begin(),
                                   project_hooks.pre_tool.end());
            merged.post_tool.insert(merged.post_tool.end(), project_hooks.post_tool.begin(),
                                    project_hooks.post_tool.end());
            merged.session_start.insert(merged.session_start.end(), project_hooks.session_start.begin(),
                                        project_hooks.session_start.end());
            merged.session_end.insert(merged.session_end.end(), project_hooks.session_end.begin(),
                                      project_hooks.session_end.end());
            // v2 事件组:同事件的组追加(用户级组在前)。
            for (const auto& [event, groups] : project_hooks.events) {
                auto& target = merged.events[event];
                target.insert(target.end(), groups.begin(), groups.end());
            }
        }
        result.config.hooks = std::move(merged);

        if (global_has_legacy && project_has_legacy) {
            result.deprecation_notices.push_back(
                "hooks 合并规则已变:全局与项目里的旧格式 hooks(pre_tool/post_tool/"
                "session_start/session_end)现在会一起生效(旧版本项目级整段覆盖全局)。"
                "两层都拦不动对方;项目级 hooks 须经 /hooks 信任后才执行。");
        }
        if (global_has_legacy ^ project_has_legacy) {
            const std::string which = global_has_legacy ? "全局" : "项目";
            result.deprecation_notices.push_back(
                which + "配置里有旧格式 hooks(pre_tool 等四类)。旧协议已废弃:环境变量输入、"
                        "任意非零退出码拦截、固定 30 秒超时这些旧语义只保兼容,将来迁到 schema 2"
                        "(stdin JSON + 事件名键,见文档 docs/hooks.md)。");
        }
    }

    if (project_file.has_value() && project_file->mcp_servers.has_value()) {
        result.config.mcp_servers = *project_file->mcp_servers;
    } else if (global_file.has_value() && global_file->mcp_servers.has_value()) {
        result.config.mcp_servers = *global_file->mcp_servers;
    }

    if (project_file.has_value() && project_file->search.has_value()) {
        result.config.search = *project_file->search;
    } else if (global_file.has_value() && global_file->search.has_value()) {
        result.config.search = *global_file->search;
    }

    if (project_file.has_value() && project_file->lsp_servers.has_value()) {
        result.config.lsp_servers = *project_file->lsp_servers;
    } else if (global_file.has_value() && global_file->lsp_servers.has_value()) {
        result.config.lsp_servers = *global_file->lsp_servers;
    }

    // subagent 段双读:新名优先,旧名兼容,冲突明报(同上 resolve 逻辑,
    // 标签换成 subagent 段,提示里写清键的全名)。
    std::optional<int> project_subagent_steps;
    if (project_file.has_value() &&
        (project_file->subagent_max_steps_per_turn.has_value() || project_file->subagent_max_turns.has_value())) {
        project_subagent_steps =
            resolve_steps_per_turn(project_file->subagent_max_steps_per_turn, project_file->subagent_max_turns,
                                   "项目级配置 subagent 段 " + project_file->source_path);
    }
    std::optional<int> global_subagent_steps;
    if (global_file.has_value() &&
        (global_file->subagent_max_steps_per_turn.has_value() || global_file->subagent_max_turns.has_value())) {
        global_subagent_steps =
            resolve_steps_per_turn(global_file->subagent_max_steps_per_turn, global_file->subagent_max_turns,
                                   "全局配置 subagent 段 " + global_file->source_path);
    }
    if (project_subagent_steps.has_value()) {
        result.config.subagent.max_steps_per_turn = project_subagent_steps;
        result.sources.subagent = Source::ProjectConfigFile;
    } else if (global_subagent_steps.has_value()) {
        result.config.subagent.max_steps_per_turn = global_subagent_steps;
        result.sources.subagent = Source::GlobalConfigFile;
    } else {
        result.config.subagent.max_steps_per_turn = std::nullopt;  // 未单独配置:运行时继承主代理预算
        result.sources.subagent = Source::Default;
    }
    if (project_file.has_value() && project_file->status_panel.has_value()) {
        result.config.status_panel = *project_file->status_panel;
        result.sources.status_panel = Source::ProjectConfigFile;
    } else if (global_file.has_value() && global_file->status_panel.has_value()) {
        result.config.status_panel = *global_file->status_panel;
        result.sources.status_panel = Source::GlobalConfigFile;
    } else {
        result.config.status_panel = StatusPanelConfig{};
        result.sources.status_panel = Source::Default;
    }

    // extra_body/extra_headers:顶层"单 provider 配置"写法专用,跟 hooks/
    // mcpServers/search/lsp 同一套"整段回退"——项目级写了就用项目级那一整
    // 段,否则用全局那一整段,都没写就是默认空(result.config 本来就是新建
    // 出来的,default 状态已经是空 object/空 map,不需要额外 else 清空)。
    if (project_file.has_value() && project_file->extra_body.has_value()) {
        result.config.extra_body = *project_file->extra_body;
        result.sources.extra_body = Source::ProjectConfigFile;
    } else if (global_file.has_value() && global_file->extra_body.has_value()) {
        result.config.extra_body = *global_file->extra_body;
        result.sources.extra_body = Source::GlobalConfigFile;
    }
    if (project_file.has_value() && project_file->extra_headers.has_value()) {
        result.config.extra_headers = *project_file->extra_headers;
        result.sources.extra_headers = Source::ProjectConfigFile;
    } else if (global_file.has_value() && global_file->extra_headers.has_value()) {
        result.config.extra_headers = *global_file->extra_headers;
        result.sources.extra_headers = Source::GlobalConfigFile;
    }

    if (project_file.has_value() && project_file->providers.has_value()) {
        result.config.providers = *project_file->providers;
        result.sources.providers = Source::ProjectConfigFile;
    } else if (global_file.has_value() && global_file->providers.has_value()) {
        result.config.providers = *global_file->providers;
        result.sources.providers = Source::GlobalConfigFile;
    } else {
        result.config.providers.clear();
        result.sources.providers = Source::Default;
    }

    // active_provider 是一枚名字指针，项目级可钉住，没写就回退全局。
    // 真正展开 provider 放在 LoadFromEnv：那一步才能按 key_env 读取任意
    // 环境变量，同时让 MergeConfig 继续保持纯函数。
    if (const auto p = pick(&FileConfig::active_provider); p.value != nullptr) {
        result.config.active_provider = *p.value;
        result.sources.active_provider = p.source;
    }

    return result;
}

std::expected<ConfigResult, std::string> MergeConfig(const LubancodeEnvValues& lubancode_env,
                                                       const std::optional<FileConfig>& file_config,
                                                       const GenericEnvValues& generic_env) {
    // 只有一份配置文件时当项目级看待(全局留空),来源统一记 ProjectConfigFile。
    return MergeConfig(lubancode_env, file_config, std::nullopt, generic_env);
}

std::expected<void, std::string> RequireApiKey(const ConfigResult& result) {
    if (!result.config.auth_token.empty()) {
        return {};
    }
    const std::string generic_api_key_name =
        result.config.wire == Wire::Anthropic ? "ANTHROPIC_AUTH_TOKEN" : "OPENAI_API_KEY";
    return std::unexpected(cli::trf("error.api_key_missing", generic_api_key_name));
}

std::string MaskApiKey(const std::string& api_key) {
    if (api_key.empty()) {
        return "(未设置)";
    }
    if (api_key.size() <= 8) {
        return api_key + "...";
    }
    return api_key.substr(0, 8) + "...";
}

std::expected<void, std::string> RequireConfigured(const ConfigResult& result) {
    std::vector<std::string> missing;
    if (result.config.base_url.empty()) {
        missing.push_back("base_url");
    }
    if (result.config.auth_token.empty()) {
        missing.push_back("api_key");
    }
    if (result.config.model.empty()) {
        missing.push_back("model");
    }
    if (missing.empty()) {
        return {};
    }

    // 提示语只点名实际缺的那几个字段——不然"只缺 model"时,提示里混进
    // "base_url"/"api_key" 字样反而误导人去查不缺的字段。
    std::string joined;
    std::string joined_env;
    for (std::size_t i = 0; i < missing.size(); ++i) {
        if (i != 0) {
            joined += "、";
            joined_env += " / ";
        }
        joined += missing[i];
        joined_env += "LUBANCODE_";
        for (const char c : missing[i]) {
            joined_env += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }

    return std::unexpected(cli::trf("error.not_configured", joined, joined_env));
}

std::expected<std::string, std::string> SaveConfigFile(const Config& config) {
    const auto home = HomeDir();
    if (!home.has_value()) {
        return std::unexpected("找不到用户主目录(Windows 下是 %USERPROFILE%),没法保存配置文件");
    }

    namespace fs = std::filesystem;
    const fs::path path = NewConfigPathFor(fs::path(*home));

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return std::unexpected("建目录 " + path.parent_path().string() + " 失败: " + ec.message());
    }

    nlohmann::json j;
    j["wire"] = ProviderWireName(config.wire);
    j["base_url"] = config.base_url;
    j["api_key"] = config.auth_token;
    j["model"] = config.model;
    j["max_context_chars"] = config.max_context_chars;
    if (!config.active_provider.empty()) {
        j["active_provider"] = config.active_provider;
    }
    if (!config.providers.empty()) {
        j["providers"] = ProvidersToJson(config.providers);
    }
    if (!config.language.empty()) {
        j["language"] = config.language;  // i18n:向导选过语言才写,空 = 跟系统,不落字段
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return std::unexpected("配置文件 " + path.string() + " 打不开写入(检查一下权限)");
    }
    file << j.dump(2);
    file.close();
    return path.string();
}

namespace {

// 只更新一份已存在配置文件里的某个字符串字段,其余字段(哪怕是 FileConfig
// 不认得的)原样保留——直接读原始 JSON、改一个键、写回去。
// UpdateModelInConfigFile / UpdateSoulInConfigFile 共用这一份。
std::expected<void, std::string> UpdateStringFieldInConfigFile(const std::string& file_path, const char* field,
                                                                 const std::string& value) {
    std::ifstream in(file_path, std::ios::binary);
    if (!in.is_open()) {
        return std::unexpected("配置文件 " + file_path + " 打不开(检查一下权限)");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    in.close();

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(buffer.str());
    } catch (const nlohmann::json::parse_error& e) {
        return std::unexpected("配置文件 " + file_path + " 不是合法 JSON: " + std::string(e.what()));
    }
    if (!parsed.is_object()) {
        return std::unexpected("配置文件 " + file_path + " 顶层必须是一个 JSON object(花括号包起来的那种)");
    }
    parsed[field] = value;

    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return std::unexpected("配置文件 " + file_path + " 打不开写入(检查一下权限)");
    }
    out << parsed.dump(2);
    return {};
}

}  // namespace

std::expected<void, std::string> UpdateModelInConfigFile(const std::string& file_path, const std::string& model) {
    return UpdateStringFieldInConfigFile(file_path, "model", model);
}

std::expected<void, std::string> UpdateSoulInConfigFile(const std::string& file_path, const std::string& soul) {
    return UpdateStringFieldInConfigFile(file_path, "soul", soul);
}

std::expected<void, std::string> UpdateLanguageInConfigFile(const std::string& file_path,
                                                              const std::string& language) {
    return UpdateStringFieldInConfigFile(file_path, "language", language);
}

namespace {

nlohmann::json ProvidersToJson(const std::vector<ProviderConfig>& providers) {
    nlohmann::json out = nlohmann::json::array();
    for (const ProviderConfig& provider : providers) {
        nlohmann::json item = {{"name", provider.name},
                               {"base_url", provider.base_url},
                               {"wire", ProviderWireName(provider.wire)},
                               {"key_env", provider.key_env},
                               {"model", provider.model},
                               {"context_window", provider.context_window_tokens}};
        // api_key/model_reasoning_effort 都可选：没设置就不落这个键，别让
        // 一份没贴过明文 key 的旧配置写回后平白多出一个空字符串字段。
        if (!provider.api_key.empty()) {
            item["api_key"] = provider.api_key;
        }
        if (!provider.model_reasoning_effort.empty()) {
            item["model_reasoning_effort"] = provider.model_reasoning_effort;
        }
        // native_web_search 同理：默认 false，开了才落盘，没开的旧配置写
        // 回后不多出这个键。
        if (provider.native_web_search) {
            item["native_web_search"] = provider.native_web_search;
        }
        // stream_usage 同理(见 ProviderConfig::stream_usage)。
        if (provider.stream_usage) {
            item["stream_usage"] = provider.stream_usage;
        }
        // reasoning_replay 同理:默认空(=never)不落盘。
        if (!provider.reasoning_replay.empty()) {
            item["reasoning_replay"] = provider.reasoning_replay;
        }
        // extra_body/extra_headers 同理:默认空,非空才落盘,没设置的旧
        // 配置写回后不多出这两个键。
        if (!provider.extra_body.empty()) {
            item["extra_body"] = provider.extra_body;
        }
        if (!provider.extra_headers.empty()) {
            item["extra_headers"] = provider.extra_headers;
        }
        out.push_back(std::move(item));
    }
    return out;
}

std::expected<nlohmann::json, std::string> ReadConfigObjectForUpdate(const std::string& file_path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(fs::path(file_path), ec) && !ec) {
        return nlohmann::json::object();
    }
    if (ec) {
        return std::unexpected("检查配置文件 " + file_path + " 失败: " + ec.message());
    }

    std::ifstream in(file_path, std::ios::binary);
    if (!in.is_open()) {
        return std::unexpected("配置文件 " + file_path + " 打不开(检查一下权限)");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    try {
        nlohmann::json root = nlohmann::json::parse(buffer.str());
        if (!root.is_object()) {
            return std::unexpected("配置文件 " + file_path + " 顶层必须是一个 JSON object(花括号包起来的那种)");
        }
        return root;
    } catch (const nlohmann::json::parse_error& e) {
        return std::unexpected("配置文件 " + file_path + " 不是合法 JSON: " + std::string(e.what()));
    }
}

std::expected<void, std::string> WriteConfigObject(const std::string& file_path, const nlohmann::json& root) {
    namespace fs = std::filesystem;
    const fs::path path(file_path);
    std::error_code ec;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            return std::unexpected("建目录 " + path.parent_path().string() + " 失败: " + ec.message());
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return std::unexpected("配置文件 " + file_path + " 打不开写入(检查一下权限)");
    }
    out << root.dump(2);
    if (!out.good()) {
        return std::unexpected("配置文件 " + file_path + " 写入失败(检查一下磁盘/权限)");
    }
    return {};
}

std::expected<std::vector<ProviderConfig>, std::string> ProvidersInConfigObject(const nlohmann::json& root,
                                                                                  const std::string& file_path) {
    if (!root.contains("providers")) {
        return std::vector<ProviderConfig>{};
    }
    return ParseProvidersConfig(root["providers"], file_path);
}

}  // namespace

std::expected<void, std::string> UpdateProvidersInConfigFile(const std::string& file_path,
                                                               const std::vector<ProviderConfig>& providers) {
    for (std::size_t i = 0; i < providers.size(); ++i) {
        const auto valid = ValidateProviderConfig(providers[i]);
        if (!valid.has_value()) {
            return std::unexpected("providers[" + std::to_string(i) + "]: " + valid.error());
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (providers[j].name == providers[i].name) {
                return std::unexpected("providers 名字重复: " + providers[i].name);
            }
        }
    }
    auto root = ReadConfigObjectForUpdate(file_path);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    (*root)["providers"] = ProvidersToJson(providers);
    return WriteConfigObject(file_path, *root);
}

std::expected<void, std::string> UpdateActiveProviderInConfigFile(const std::string& file_path,
                                                                    const std::string& name) {
    if (name.empty()) {
        return std::unexpected("active_provider 不能为空");
    }
    return UpdateStringFieldInConfigFile(file_path, "active_provider", name);
}

std::expected<std::string, std::string> SetActiveProviderInGlobalConfig(const std::string& name) {
    if (name.empty()) {
        return std::unexpected("active_provider 不能为空");
    }
    const auto home = HomeDir();
    if (!home.has_value()) {
        return std::unexpected("找不到用户主目录,没法记住当前 provider");
    }
    const std::string path = NewConfigPathFor(std::filesystem::path(*home)).string();
    auto root = ReadConfigObjectForUpdate(path);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    (*root)["active_provider"] = name;
    const auto written = WriteConfigObject(path, *root);
    if (!written.has_value()) {
        return std::unexpected(written.error());
    }
    return path;
}

std::expected<std::string, std::string> AddProviderToGlobalConfig(const ProviderConfig& provider) {
    const auto valid = ValidateProviderConfig(provider);
    if (!valid.has_value()) {
        return std::unexpected(valid.error());
    }
    const auto home = HomeDir();
    if (!home.has_value()) {
        return std::unexpected("找不到用户主目录,没法保存 provider 配置");
    }
    const std::string path = NewConfigPathFor(std::filesystem::path(*home)).string();
    auto root = ReadConfigObjectForUpdate(path);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto providers = ProvidersInConfigObject(*root, path);
    if (!providers.has_value()) {
        return std::unexpected(providers.error());
    }
    if (FindProvider(*providers, provider.name) != nullptr) {
        return std::unexpected("provider 已存在: " + provider.name);
    }
    providers->push_back(provider);
    const auto written = UpdateProvidersInConfigFile(path, *providers);
    if (!written.has_value()) {
        return std::unexpected(written.error());
    }
    return path;
}

std::expected<std::string, std::string> RemoveProviderFromGlobalConfig(const std::string& name) {
    const auto home = HomeDir();
    if (!home.has_value()) {
        return std::unexpected("找不到用户主目录,没法更新 provider 配置");
    }
    const std::string path = NewConfigPathFor(std::filesystem::path(*home)).string();
    auto root = ReadConfigObjectForUpdate(path);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto providers = ProvidersInConfigObject(*root, path);
    if (!providers.has_value()) {
        return std::unexpected(providers.error());
    }
    auto it = std::find_if(providers->begin(), providers->end(), [&](const ProviderConfig& provider) {
        return provider.name == name;
    });
    if (it == providers->end()) {
        return std::unexpected("provider 不存在: " + name);
    }
    providers->erase(it);
    const auto written = UpdateProvidersInConfigFile(path, *providers);
    if (!written.has_value()) {
        return std::unexpected(written.error());
    }
    return path;
}

std::expected<std::string, std::string> SetProviderNativeWebSearchInGlobalConfig(const std::string& name,
                                                                                   bool enabled) {
    const auto home = HomeDir();
    if (!home.has_value()) {
        return std::unexpected("找不到用户主目录,没法更新 provider 配置");
    }
    const std::string path = NewConfigPathFor(std::filesystem::path(*home)).string();
    auto root = ReadConfigObjectForUpdate(path);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto providers = ProvidersInConfigObject(*root, path);
    if (!providers.has_value()) {
        return std::unexpected(providers.error());
    }
    if (!SetProviderNativeWebSearch(*providers, name, enabled)) {
        return std::unexpected("provider 不存在: " + name);
    }
    const auto written = UpdateProvidersInConfigFile(path, *providers);
    if (!written.has_value()) {
        return std::unexpected(written.error());
    }
    return path;
}

std::expected<std::string, std::string> SetProviderExtraBodyInGlobalConfig(const std::string& name,
                                                                             const nlohmann::json& body) {
    const auto home = HomeDir();
    if (!home.has_value()) {
        return std::unexpected("找不到用户主目录,没法更新 provider 配置");
    }
    const std::string path = NewConfigPathFor(std::filesystem::path(*home)).string();
    auto root = ReadConfigObjectForUpdate(path);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto providers = ProvidersInConfigObject(*root, path);
    if (!providers.has_value()) {
        return std::unexpected(providers.error());
    }
    if (!SetProviderExtraBody(*providers, name, body)) {
        return std::unexpected("provider 不存在: " + name);
    }
    const auto written = UpdateProvidersInConfigFile(path, *providers);
    if (!written.has_value()) {
        return std::unexpected(written.error());
    }
    return path;
}

std::expected<std::string, std::string> SetProviderExtraHeaderInGlobalConfig(const std::string& name,
                                                                               const std::string& header_name,
                                                                               const std::string& value) {
    const auto home = HomeDir();
    if (!home.has_value()) {
        return std::unexpected("找不到用户主目录,没法更新 provider 配置");
    }
    const std::string path = NewConfigPathFor(std::filesystem::path(*home)).string();
    auto root = ReadConfigObjectForUpdate(path);
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    auto providers = ProvidersInConfigObject(*root, path);
    if (!providers.has_value()) {
        return std::unexpected(providers.error());
    }
    if (!SetProviderExtraHeader(*providers, name, header_name, value)) {
        return std::unexpected("provider 不存在: " + name);
    }
    const auto written = UpdateProvidersInConfigFile(path, *providers);
    if (!written.has_value()) {
        return std::unexpected(written.error());
    }
    return path;
}

std::expected<ConfigResult, std::string> LoadFromEnv() {
    LubancodeEnvValues lubancode_env;
    lubancode_env.wire = GetEnv("LUBANCODE_WIRE");
    lubancode_env.base_url = GetEnv("LUBANCODE_BASE_URL");
    lubancode_env.api_key = GetEnv("LUBANCODE_API_KEY");
    lubancode_env.model = GetEnv("LUBANCODE_MODEL");
    lubancode_env.theme = GetEnv("LUBANCODE_THEME");
    lubancode_env.language = GetEnv("LUBANCODE_LANG");
    lubancode_env.system_prompt_file = GetEnv("LUBANCODE_SYSTEM_PROMPT_FILE");
    lubancode_env.context_window = GetEnv("LUBANCODE_CONTEXT_WINDOW");
    lubancode_env.compact_model = GetEnv("LUBANCODE_COMPACT_MODEL");
    lubancode_env.think = GetEnv("LUBANCODE_THINK");
    lubancode_env.soul = GetEnv("LUBANCODE_SOUL");
    if (const auto raw = GetEnv("LUBANCODE_MAX_CONTEXT"); raw.has_value()) {
        try {
            const long long parsed = std::stoll(*raw);
            if (parsed > 0) {
                lubancode_env.max_context_chars = static_cast<std::size_t>(parsed);
            }
            // 解析出来但 <= 0:当没设置处理,往下一级找,不报错。
        } catch (...) {
            // 不是合法数字:同样当没设置处理,不报错(跟原来的
            // agent::MaxContextCharsFromEnv 行为保持一致)。
        }
    }
    // 步数上限双读(命名规范第二批):新名 LUBANCODE_MAX_STEPS_PER_TURN
    // 优先,旧名 LUBANCODE_MAX_TURNS 兼容;同现取舍与提示在 MergeConfig 判定。
    for (const char* name : {"LUBANCODE_MAX_STEPS_PER_TURN", "LUBANCODE_MAX_TURNS"}) {
        const auto raw = GetEnv(name);
        if (!raw.has_value()) {
            continue;
        }
        try {
            const long long parsed = std::stoll(*raw);
            if (parsed < 0) {
                continue;  // 负数:当没设置处理,往下一级找,不报错。
            }
            if (std::string(name) == "LUBANCODE_MAX_STEPS_PER_TURN") {
                lubancode_env.max_steps_per_turn = static_cast<int>(parsed);
            } else {
                lubancode_env.max_turns = static_cast<int>(parsed);  // 0 是合法值(显式无上限)
            }
        } catch (...) {
            // 不是合法数字:同样当没设置处理,不报错。
        }
    }

    const auto loaded = LoadFileConfigs();
    if (!loaded.has_value()) {
        return std::unexpected(loaded.error());
    }

    GenericEnvValues generic_env;
    generic_env.anthropic_base_url = GetEnv("ANTHROPIC_BASE_URL");
    generic_env.anthropic_auth_token = GetEnv("ANTHROPIC_AUTH_TOKEN");
    generic_env.anthropic_model = GetEnv("ANTHROPIC_MODEL");
    generic_env.openai_base_url = GetEnv("OPENAI_BASE_URL");
    generic_env.openai_api_key = GetEnv("OPENAI_API_KEY");
    generic_env.openai_model = GetEnv("OPENAI_MODEL");

    auto merged = MergeConfig(lubancode_env, loaded->project, loaded->global, generic_env);
    if (merged.has_value()) {
        ApplyConfiguredActiveProvider(*merged);
        if (loaded->project.has_value()) {
            merged->project_config_file_path = loaded->project->source_path;
        }
        if (loaded->global.has_value()) {
            merged->global_config_file_path = loaded->global->source_path;
        }
        // 写回优先目标:项目级在就写项目级,否则写全局(都没有就空)。
        merged->config_file_path = merged->project_config_file_path.has_value()
                                       ? merged->project_config_file_path
                                       : merged->global_config_file_path;
        merged->migration_notice = loaded->migration_notice;
    }
    return merged;
}

std::expected<std::string, std::string> ReadSystemPromptFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("--system-prompt 指定的文件打不开: " + path + "(检查路径和读权限)");
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    if (content.empty()) {
        return std::unexpected("--system-prompt 指定的文件是空的: " + path);
    }
    return content;
}

// ---------------------------------------------------------------------------
// settings.local.json:项目级本地权限(不进版本库)。
// ---------------------------------------------------------------------------

std::string SettingsLocalPath(const std::string& cwd_dir) {
    return (std::filesystem::path(cwd_dir) / ".lubancode" / "settings.local.json").string();
}

std::expected<SettingsLocal, std::string> ParseSettingsLocal(const std::string& json_text,
                                                              const std::string& path_for_error) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::parse_error& e) {
        return std::unexpected("settings.local.json " + path_for_error + " 不是合法 JSON: " + e.what());
    }
    if (!parsed.is_object()) {
        return std::unexpected("settings.local.json " + path_for_error + " 顶层必须是一个 JSON object");
    }

    SettingsLocal out;
    if (!parsed.contains("permissions")) {
        return out;  // 没有 permissions 段就是空的,不算错
    }
    const auto& perms = parsed["permissions"];
    if (!perms.is_object()) {
        return std::unexpected("settings.local.json " + path_for_error + " 里的 permissions 字段必须是 JSON object");
    }

    // 字符串数组:非字符串元素跳过(宽容),不因为夹了个坏元素就整份作废。
    const auto read_str_array = [&](const char* key, std::vector<std::string>& into) {
        if (!perms.contains(key) || !perms[key].is_array()) {
            return;
        }
        for (const auto& item : perms[key]) {
            if (item.is_string()) {
                into.push_back(item.get<std::string>());
            }
        }
    };
    read_str_array("allow_tools", out.allow_tools);
    read_str_array("allow_commands", out.allow_commands);
    read_str_array("deny_commands", out.deny_commands);

    if (perms.contains("default_confirm_mode") && perms["default_confirm_mode"].is_string()) {
        std::string mode = perms["default_confirm_mode"].get<std::string>();
        if (!mode.empty()) {
            out.default_confirm_mode = std::move(mode);  // auto/yolo/confirm,别的值交给调用方判
        }
    }
    return out;
}

std::expected<std::optional<SettingsLocal>, std::string> LoadSettingsLocal(const std::string& cwd_dir) {
    const std::string path = SettingsLocalPath(cwd_dir);
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(path), ec) || ec) {
        return std::optional<SettingsLocal>(std::nullopt);  // 没这文件不算错
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("settings.local.json " + path + " 存在,但打不开(检查一下权限)");
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    auto parsed = ParseSettingsLocal(buffer.str(), path);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    return std::optional<SettingsLocal>(*parsed);
}

std::expected<std::string, std::string> AddAllowedToolToSettingsLocal(const std::string& cwd_dir,
                                                                       const std::string& tool_name) {
    namespace fs = std::filesystem;
    const std::string path = SettingsLocalPath(cwd_dir);

    // 已有内容原样读进来(保留不认得的字段);读不到/坏 JSON 就从空 object 起。
    nlohmann::json root = nlohmann::json::object();
    {
        std::error_code ec;
        if (fs::exists(fs::path(path), ec) && !ec) {
            std::ifstream in(path, std::ios::binary);
            if (in.is_open()) {
                std::ostringstream buffer;
                buffer << in.rdbuf();
                try {
                    auto existing = nlohmann::json::parse(buffer.str());
                    if (existing.is_object()) {
                        root = std::move(existing);
                    }
                } catch (const nlohmann::json::parse_error&) {
                    // 坏 JSON:不覆盖用户手写的东西,报错让人自己看一眼。
                    return std::unexpected("settings.local.json " + path +
                                            " 不是合法 JSON,没敢覆盖;请手动检查后再试");
                }
            }
        }
    }

    if (!root.contains("permissions") || !root["permissions"].is_object()) {
        root["permissions"] = nlohmann::json::object();
    }
    auto& perms = root["permissions"];
    if (!perms.contains("allow_tools") || !perms["allow_tools"].is_array()) {
        perms["allow_tools"] = nlohmann::json::array();
    }
    auto& allow = perms["allow_tools"];
    for (const auto& item : allow) {
        if (item.is_string() && item.get<std::string>() == tool_name) {
            return path;  // 已经在了,幂等,不重复写
        }
    }
    allow.push_back(tool_name);

    // 项目级 .lubancode/ 只在这一刻按需落地。
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) {
        return std::unexpected("建目录 " + fs::path(path).parent_path().string() + " 失败: " + ec.message());
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return std::unexpected("settings.local.json " + path + " 打不开写入(检查一下权限)");
    }
    out << root.dump(2);
    if (!out.good()) {
        return std::unexpected("settings.local.json " + path + " 写入失败(检查一下磁盘/权限)");
    }
    return path;
}

namespace {

// 命令去掉前导空白后,以某条(非空)前缀打头就算命中。
bool CommandHasPrefix(const std::string& command, const std::vector<std::string>& prefixes) {
    if (prefixes.empty()) {
        return false;
    }
    const std::size_t start = command.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return false;
    }
    const std::string trimmed = command.substr(start);
    for (const std::string& prefix : prefixes) {
        if (!prefix.empty() && trimmed.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

CommandPermission ClassifyCommandByPermissions(const std::string& command,
                                               const std::vector<std::string>& allow_commands,
                                               const std::vector<std::string>& deny_commands) {
    if (CommandHasPrefix(command, deny_commands)) {
        return CommandPermission::Deny;  // deny 压过 allow
    }
    if (CommandHasPrefix(command, allow_commands)) {
        return CommandPermission::Allow;
    }
    return CommandPermission::None;
}

std::string EnsureGitignoreCoversSettingsLocal(const std::string& cwd_dir) {
    namespace fs = std::filesystem;
    const fs::path gitignore = fs::path(cwd_dir) / ".gitignore";
    const std::string kIgnoreLine = ".lubancode/settings.local.json";

    std::error_code ec;
    if (!fs::exists(gitignore, ec) || ec) {
        // 没有 .gitignore,别硬塞——打一行提示教用户手动加。
        return "提示:本目录没有 .gitignore;要不进版本库,请手动加一行 " + kIgnoreLine;
    }

    std::string content;
    {
        std::ifstream in(gitignore, std::ios::binary);
        if (!in.is_open()) {
            return "提示:.gitignore 打不开;请手动加一行 " + kIgnoreLine;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        content = buffer.str();
    }

    // 已经挡住?整个 .lubancode/ 目录被忽略、或者精确忽略了这个文件,都算。
    if (content.find(".lubancode/") != std::string::npos ||
        content.find("settings.local.json") != std::string::npos) {
        return "";  // 已经挡住,什么都不必做
    }

    std::ofstream out(gitignore, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        return "提示:.gitignore 追加不了;请手动加一行 " + kIgnoreLine;
    }
    if (!content.empty() && content.back() != '\n') {
        out << "\n";
    }
    out << kIgnoreLine << "\n";
    return "已在 .gitignore 追加一行 " + kIgnoreLine;
}

}  // namespace lubancode::config
