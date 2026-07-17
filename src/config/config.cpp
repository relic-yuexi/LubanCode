#include "config/config.hpp"

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

constexpr const char* kDefaultAnthropicBaseUrl = "https://api.minimaxi.com/anthropic";
constexpr const char* kDefaultAnthropicModel = "MiniMax-M3";

constexpr const char* kDefaultOpenAiBaseUrl = "https://api.minimaxi.com/v1";
constexpr const char* kDefaultOpenAiModel = "MiniMax-M3";

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

// 用户主目录:Windows 取 %USERPROFILE%,别的平台取 $HOME。
std::optional<std::string> HomeDir() {
#ifdef _WIN32
    return GetEnv("USERPROFILE");
#else
    return GetEnv("HOME");
#endif
}

}  // namespace

std::string ToString(Source source) {
    switch (source) {
        case Source::LubancodeEnv:
            return "LUBANCODE_ 专属环境变量";
        case Source::ConfigFile:
            return "配置文件(.lubancode.json)";
        case Source::GenericEnv:
            return "通用环境变量(ANTHROPIC_*/OPENAI_*)";
        case Source::Default:
            return "内置默认值";
    }
    return "未知来源";
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

    return config;
}

std::expected<std::optional<FileConfig>, std::string> LoadFileConfig() {
    namespace fs = std::filesystem;

    std::vector<fs::path> candidates;
    candidates.push_back(fs::current_path() / ".lubancode.json");
    if (const auto home = HomeDir(); home.has_value()) {
        candidates.push_back(fs::path(*home) / ".lubancode.json");
    }

    for (const auto& path : candidates) {
        std::error_code ec;
        if (!fs::exists(path, ec) || ec) {
            continue;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected("配置文件 " + path.string() + " 存在,但打不开(检查一下权限)");
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();

        const auto parsed = ParseFileConfigJson(buffer.str(), path.string());
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
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
    const std::string default_base_url = is_anthropic ? kDefaultAnthropicBaseUrl : kDefaultOpenAiBaseUrl;
    const std::string default_model = is_anthropic ? kDefaultAnthropicModel : kDefaultOpenAiModel;
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
        "  2) 配置文件(cwd 或用户主目录的 .lubancode.json)里的 api_key 字段\n"
        "  3) 通用环境变量 " +
        generic_api_key_name +
        "\n"
        "  4) 内置默认值(api_key 没有内置默认值,必须自己配一个)\n"
        "挑一种配上,再重新运行 lubancode。用 --config 能看到当前每个字段实际读到了什么。");
}

std::expected<ConfigResult, std::string> LoadFromEnv() {
    LubancodeEnvValues lubancode_env;
    lubancode_env.wire = GetEnv("LUBANCODE_WIRE");
    lubancode_env.base_url = GetEnv("LUBANCODE_BASE_URL");
    lubancode_env.api_key = GetEnv("LUBANCODE_API_KEY");
    lubancode_env.model = GetEnv("LUBANCODE_MODEL");
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

    return MergeConfig(lubancode_env, *file_config, generic_env);
}

}  // namespace lubancode::config
