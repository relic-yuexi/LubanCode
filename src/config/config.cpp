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

    // ---- 对象型整段(hooks/mcpServers/search/lsp):只从配置文件来,没有
    // 环境变量、没有内置默认值这两级。按"整段"回退——项目级写了就用项目级
    // 那一整段,否则用全局那一整段(不做键级混合,语义清楚)。 ----
    if (project_file.has_value() && project_file->hooks.has_value()) {
        result.config.hooks = *project_file->hooks;
    } else if (global_file.has_value() && global_file->hooks.has_value()) {
        result.config.hooks = *global_file->hooks;
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
    j["wire"] = (config.wire == Wire::Responses) ? "responses" : "anthropic";
    j["base_url"] = config.base_url;
    j["api_key"] = config.auth_token;
    j["model"] = config.model;
    j["max_context_chars"] = config.max_context_chars;
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
