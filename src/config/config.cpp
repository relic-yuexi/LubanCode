#include "config/config.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::config {

namespace {

// 读一个环境变量;没设置或者是空串都算"没有"。
std::optional<std::string> GetEnv(const char* name) {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    const errno_t err = _dupenv_s(&buffer, &size, name);
    if (err != 0 || buffer == nullptr) {
        return std::nullopt;
    }
    std::string value(buffer);
    std::free(buffer);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

}  // namespace

std::optional<std::string> HomeDir() {
#ifdef _WIN32
    return GetEnv("USERPROFILE");
#else
    return GetEnv("HOME");
#endif
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

std::string ToString(Source source) {
    switch (source) {
        case Source::LubancodeEnv:
            return "LUBANCODE_ 专属环境变量";
        case Source::ConfigFile:
            return "配置文件(.lubancode/config.json)";
        case Source::GenericEnv:
            return "通用环境变量(ANTHROPIC_*/OPENAI_*)";
        case Source::Default:
            return "内置默认值";
    }
    return "未知来源";
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

std::expected<HooksConfig, std::string> ParseHooksConfig(const nlohmann::json& hooks_json,
                                                           const std::string& file_path_for_error) {
    if (!hooks_json.is_object()) {
        return std::unexpected("配置文件 " + file_path_for_error + " 里的 hooks 字段必须是一个 JSON object");
    }

    HooksConfig config;
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
    return config;
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
    if (parsed.contains("theme")) {
        if (!parsed["theme"].is_string()) {
            return std::unexpected("配置文件 " + file_path_for_error + " 里的 theme 字段必须是字符串");
        }
        config.theme = parsed["theme"].get<std::string>();
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

std::expected<std::optional<FileConfig>, std::string> LoadFileConfig() {
    namespace fs = std::filesystem;

    std::vector<fs::path> base_dirs;
    base_dirs.push_back(fs::current_path());
    if (const auto home = HomeDir(); home.has_value()) {
        base_dirs.push_back(fs::path(*home));
    }

    // 第一遍:新位置优先,cwd 新位置 -> 主目录新位置。
    for (const auto& base : base_dirs) {
        const fs::path new_path = NewConfigPathFor(base);
        std::error_code ec;
        if (fs::exists(new_path, ec) && !ec) {
            const auto parsed = ReadAndParseConfigFile(new_path);
            if (!parsed.has_value()) {
                return std::unexpected(parsed.error());
            }
            return std::optional<FileConfig>(*parsed);
        }
    }

    // 第二遍:旧位置,cwd 旧位置 -> 主目录旧位置,命中就顺手迁移。
    for (const auto& base : base_dirs) {
        const fs::path old_path = OldConfigPathFor(base);
        std::error_code ec;
        if (!fs::exists(old_path, ec) || ec) {
            continue;
        }
        const fs::path new_path = NewConfigPathFor(base);
        const ConfigMigrationOutcome outcome = MigrateConfigFileIfNeeded(old_path.string(), new_path.string());
        const fs::path effective = outcome.effective_path.empty() ? old_path : fs::path(outcome.effective_path);

        auto parsed = ReadAndParseConfigFile(effective);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        parsed->migration_notice = outcome.notice;
        return std::optional<FileConfig>(*parsed);
    }

    return std::optional<FileConfig>(std::nullopt);
}

std::expected<ConfigResult, std::string> MergeConfig(const LubancodeEnvValues& lubancode_env,
                                                       const std::optional<FileConfig>& file_config,
                                                       const GenericEnvValues& generic_env) {
    ConfigResult result;

    // ---- 第一步:解出 wire。只看专属 env(1 级)、配置文件(2 级)、
    // 默认值(4 级)——通用环境变量里没有"wire"这一说,tier 3 跳过。 ----
    std::string wire_str;
    Source wire_source = Source::Default;
    std::string wire_error_origin;  // 报错时说清楚这个坏值是从哪儿来的

    if (lubancode_env.wire.has_value()) {
        wire_str = *lubancode_env.wire;
        wire_source = Source::LubancodeEnv;
        wire_error_origin = "环境变量 LUBANCODE_WIRE";
    } else if (file_config.has_value() && file_config->wire.has_value()) {
        wire_str = *file_config->wire;
        wire_source = Source::ConfigFile;
        wire_error_origin = "配置文件 " + file_config->source_path + " 里的 wire 字段";
    } else {
        wire_str = "anthropic";
        wire_source = Source::Default;
    }

    Wire wire;
    if (wire_str == "anthropic") {
        wire = Wire::Anthropic;
    } else if (wire_str == "responses") {
        wire = Wire::Responses;
    } else {
        return std::unexpected(wire_error_origin + " 只认得 anthropic 或 responses,写的是: " + wire_str);
    }
    result.config.wire = wire;
    result.sources.wire = wire_source;

    const bool is_anthropic = (wire == Wire::Anthropic);
    // base_url、model 没有内置默认值(lubancode 不绑死哪一家模型服务)——
    // 四级都没配到时就是空串,来源记 Default,留给上层(初次配置向导 /
    // RequireConfigured)去拦。
    const std::string default_base_url;
    const std::string default_model;
    const std::optional<std::string>& generic_base_url =
        is_anthropic ? generic_env.anthropic_base_url : generic_env.openai_base_url;
    const std::optional<std::string>& generic_api_key =
        is_anthropic ? generic_env.anthropic_auth_token : generic_env.openai_api_key;
    const std::optional<std::string>& generic_model =
        is_anthropic ? generic_env.anthropic_model : generic_env.openai_model;

    // ---- base_url:1 级 > 2 级 > 3 级(按 wire 挑对应变量)> 4 级默认值 ----
    if (lubancode_env.base_url.has_value()) {
        result.config.base_url = *lubancode_env.base_url;
        result.sources.base_url = Source::LubancodeEnv;
    } else if (file_config.has_value() && file_config->base_url.has_value()) {
        result.config.base_url = *file_config->base_url;
        result.sources.base_url = Source::ConfigFile;
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
    } else if (file_config.has_value() && file_config->model.has_value()) {
        result.config.model = *file_config->model;
        result.sources.model = Source::ConfigFile;
    } else if (generic_model.has_value()) {
        result.config.model = *generic_model;
        result.sources.model = Source::GenericEnv;
    } else {
        result.config.model = default_model;
        result.sources.model = Source::Default;
    }

    // ---- api_key:同上,但没有内置默认值——四级都没有时留空,来源记成
    // Default,不在这里报错(报错交给 RequireApiKey,见该函数注释)。 ----
    if (lubancode_env.api_key.has_value()) {
        result.config.auth_token = *lubancode_env.api_key;
        result.sources.auth_token = Source::LubancodeEnv;
    } else if (file_config.has_value() && file_config->api_key.has_value()) {
        result.config.auth_token = *file_config->api_key;
        result.sources.auth_token = Source::ConfigFile;
    } else if (generic_api_key.has_value()) {
        result.config.auth_token = *generic_api_key;
        result.sources.auth_token = Source::GenericEnv;
    } else {
        result.config.auth_token.clear();
        result.sources.auth_token = Source::Default;
    }

    // ---- max_context_chars:1 级 > 2 级 > 4 级默认值,没有通用 env 这一级 ----
    if (lubancode_env.max_context_chars.has_value()) {
        result.config.max_context_chars = *lubancode_env.max_context_chars;
        result.sources.max_context_chars = Source::LubancodeEnv;
    } else if (file_config.has_value() && file_config->max_context_chars.has_value()) {
        result.config.max_context_chars = *file_config->max_context_chars;
        result.sources.max_context_chars = Source::ConfigFile;
    } else {
        result.config.max_context_chars = kDefaultMaxContextChars;
        result.sources.max_context_chars = Source::Default;
    }

    // ---- theme:1 级 > 2 级 > 4 级默认值,没有通用 env 这一级(跟 wire 一样,
    // "主题名字"这种事通用环境变量 ANTHROPIC_*/OPENAI_* 压根没这个概念) ----
    if (lubancode_env.theme.has_value()) {
        result.config.theme = *lubancode_env.theme;
        result.sources.theme = Source::LubancodeEnv;
    } else if (file_config.has_value() && file_config->theme.has_value()) {
        result.config.theme = *file_config->theme;
        result.sources.theme = Source::ConfigFile;
    } else {
        result.config.theme = kDefaultTheme;
        result.sources.theme = Source::Default;
    }

    // ---- system_prompt_file:同上,1 级 > 2 级 > 4 级默认值(空串) ----
    if (lubancode_env.system_prompt_file.has_value()) {
        result.config.system_prompt_file = *lubancode_env.system_prompt_file;
        result.sources.system_prompt_file = Source::LubancodeEnv;
    } else if (file_config.has_value() && file_config->system_prompt_file.has_value()) {
        result.config.system_prompt_file = *file_config->system_prompt_file;
        result.sources.system_prompt_file = Source::ConfigFile;
    } else {
        result.config.system_prompt_file.clear();
        result.sources.system_prompt_file = Source::Default;
    }

    // ---- context_window:1 级 > 2 级 > 4 级默认值,没有通用 env 这一级。
    // 取值要过 ParseContextWindowTokens 校验,坏值直接报错(没法糊弄一个
    // "留空当默认"的语义——用户显式写了却写错,该让他知道)。 ----
    std::string context_window_error_origin;
    std::optional<std::string> context_window_raw;
    if (lubancode_env.context_window.has_value()) {
        context_window_raw = *lubancode_env.context_window;
        context_window_error_origin = "环境变量 LUBANCODE_CONTEXT_WINDOW";
        result.sources.context_window_tokens = Source::LubancodeEnv;
    } else if (file_config.has_value() && file_config->context_window.has_value()) {
        context_window_raw = *file_config->context_window;
        context_window_error_origin = "配置文件 " + file_config->source_path + " 里的 context_window 字段";
        result.sources.context_window_tokens = Source::ConfigFile;
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

    // ---- compact_model:1 级 > 2 级 > 4 级默认值(空串 = 跟当前会话模型
    // 一致),没有通用 env 这一级、没有校验(留给真正压缩时用,压不动
    // 由那时候的请求自然报错)。 ----
    if (lubancode_env.compact_model.has_value()) {
        result.config.compact_model = *lubancode_env.compact_model;
        result.sources.compact_model = Source::LubancodeEnv;
    } else if (file_config.has_value() && file_config->compact_model.has_value()) {
        result.config.compact_model = *file_config->compact_model;
        result.sources.compact_model = Source::ConfigFile;
    } else {
        result.config.compact_model.clear();
        result.sources.compact_model = Source::Default;
    }

    // ---- think:1 级 > 2 级 > 4 级默认值(空串 = 不发这个参数),没有通用
    // env 这一级。M10 把档位放开成任意字符串——档位这东西两边协议长得不一样:
    // responses 这边原样把字符串递给 API,档位是服务商定的,lubancode 没资格
    // 拦在半路先报错;anthropic 那边自己有一张映射表(见
    // api/anthropic/client.cpp 的 BuildThinkingJson),映射不上会在真正发请求
    // 那一刻打警告、当没设,不在这儿提前拦。这里只原样存,不做大小写归一化
    // ——responses 要"原样递",硬转小写会破坏这条承诺;anthropic 那张映射表
    // 自己内部做了大小写不敏感匹配,不依赖这里转不转小写。 ----
    if (lubancode_env.think.has_value()) {
        result.config.think = *lubancode_env.think;
        result.sources.think = Source::LubancodeEnv;
    } else if (file_config.has_value() && file_config->think.has_value()) {
        result.config.think = *file_config->think;
        result.sources.think = Source::ConfigFile;
    } else {
        result.config.think.clear();
        result.sources.think = Source::Default;
    }

    // ---- hooks:M9 新增,只从配置文件来,没有环境变量、没有内置默认值这
    // 两级(HooksConfig 的 ConfigSources 也不需要——只有一个来源,没什么好
    // 追踪的)。配置文件没写 hooks 字段,就是默认构造的空 HooksConfig
    // (四个数组都是空的)。 ----
    if (file_config.has_value() && file_config->hooks.has_value()) {
        result.config.hooks = *file_config->hooks;
    }

    // ---- mcpServers:M8 新增,只从配置文件来,没有环境变量、没有内置
    // 默认值这两级(跟 hooks 一样)。配置文件没写这字段,就是空 map。 ----
    if (file_config.has_value() && file_config->mcp_servers.has_value()) {
        result.config.mcp_servers = *file_config->mcp_servers;
    }

    // ---- search:websearch 用,只从配置文件来(跟 hooks/mcpServers 一样)。
    // 没写这一段就是空的 SearchConfig,web_search 工具不注册。 ----
    if (file_config.has_value() && file_config->search.has_value()) {
        result.config.search = *file_config->search;
    }

    return result;
}

std::expected<void, std::string> RequireApiKey(const ConfigResult& result) {
    if (!result.config.auth_token.empty()) {
        return {};
    }
    const std::string generic_api_key_name =
        result.config.wire == Wire::Anthropic ? "ANTHROPIC_AUTH_TOKEN" : "OPENAI_API_KEY";
    return std::unexpected(
        "缺少 API Key,没有它没法跟模型对话。按优先级从高到低找了这些地方,都没找到:\n"
        "  1) 环境变量 LUBANCODE_API_KEY\n"
        "  2) 配置文件(cwd 或用户主目录的 .lubancode/config.json,旧位置 .lubancode.json 也认,\n"
        "     读到会自动迁移)里的 api_key 字段\n"
        "  3) 通用环境变量 " +
        generic_api_key_name +
        "\n"
        "  4) 内置默认值(api_key 没有内置默认值,必须自己配一个)\n"
        "挑一种配上,再重新运行 lubancode。用 --config 能看到当前每个字段实际读到了什么。");
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

    return std::unexpected(
        "缺少配置: " + joined +
        ",没法跟模型对话(lubancode 不内置哪一家的地址/模型,得自己配)。三条途径挑一种:\n"
        "  1) 不带位置参数运行 lubancode,进入交互模式,会自动走初次配置向导\n"
        "  2) 在用户主目录放一份 .lubancode/config.json(旧版 .lubancode.json 也认,读到会自动迁移),把 " +
        joined + " 写进去(字段全部可选)\n"
        "  3) 设置对应的环境变量: " + joined_env + "\n"
        "配好之后用 --config 能看到当前每个字段实际读到了什么、来自哪一级。");
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
    j["wire"] = (config.wire == Wire::Responses) ? "responses" : "anthropic";
    j["base_url"] = config.base_url;
    j["api_key"] = config.auth_token;
    j["model"] = config.model;
    j["max_context_chars"] = config.max_context_chars;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return std::unexpected("配置文件 " + path.string() + " 打不开写入(检查一下权限)");
    }
    file << j.dump(2);
    file.close();
    return path.string();
}

std::expected<void, std::string> UpdateModelInConfigFile(const std::string& file_path, const std::string& model) {
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
    parsed["model"] = model;

    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return std::unexpected("配置文件 " + file_path + " 打不开写入(检查一下权限)");
    }
    out << parsed.dump(2);
    return {};
}

std::expected<ConfigResult, std::string> LoadFromEnv() {
    LubancodeEnvValues lubancode_env;
    lubancode_env.wire = GetEnv("LUBANCODE_WIRE");
    lubancode_env.base_url = GetEnv("LUBANCODE_BASE_URL");
    lubancode_env.api_key = GetEnv("LUBANCODE_API_KEY");
    lubancode_env.model = GetEnv("LUBANCODE_MODEL");
    lubancode_env.theme = GetEnv("LUBANCODE_THEME");
    lubancode_env.system_prompt_file = GetEnv("LUBANCODE_SYSTEM_PROMPT_FILE");
    lubancode_env.context_window = GetEnv("LUBANCODE_CONTEXT_WINDOW");
    lubancode_env.compact_model = GetEnv("LUBANCODE_COMPACT_MODEL");
    lubancode_env.think = GetEnv("LUBANCODE_THINK");
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

    const auto file_config = LoadFileConfig();
    if (!file_config.has_value()) {
        return std::unexpected(file_config.error());
    }

    GenericEnvValues generic_env;
    generic_env.anthropic_base_url = GetEnv("ANTHROPIC_BASE_URL");
    generic_env.anthropic_auth_token = GetEnv("ANTHROPIC_AUTH_TOKEN");
    generic_env.anthropic_model = GetEnv("ANTHROPIC_MODEL");
    generic_env.openai_base_url = GetEnv("OPENAI_BASE_URL");
    generic_env.openai_api_key = GetEnv("OPENAI_API_KEY");
    generic_env.openai_model = GetEnv("OPENAI_MODEL");

    auto merged = MergeConfig(lubancode_env, *file_config, generic_env);
    if (merged.has_value() && file_config->has_value()) {
        merged->config_file_path = (*file_config)->source_path;
        merged->migration_notice = (*file_config)->migration_notice;
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

}  // namespace lubancode::config
