#include "config/model_catalog.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "config/provider_catalog.hpp"

namespace lubancode::config {

namespace {

// ASCII 小写化,只给档位比较用(档位都是 none/low/high 这类 ASCII 词)。
std::string ToLowerAscii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// 取一个可选的字符串字段:没写返回 true 且 out 不动;写了但不是字符串,
// 返回 false(调用方按坏条目跳过)。
bool ReadOptionalString(const nlohmann::json& obj, const char* key, std::string& out) {
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_string()) {
        return false;
    }
    out = obj[key].get<std::string>();
    return true;
}

// 解析一个模型条目。失败时返回 nullopt,error_out 里是"哪儿坏了"的半截
// 话(调用方拼上"models[i]"前缀)。
std::optional<ModelCatalogEntry> ParseEntry(const nlohmann::json& item, std::string& error_out) {
    if (!item.is_object()) {
        error_out = "不是一个 JSON object";
        return std::nullopt;
    }
    if (!item.contains("slug") || !item["slug"].is_string() || item["slug"].get<std::string>().empty()) {
        error_out = "缺少必填字段 slug(非空字符串)";
        return std::nullopt;
    }

    ModelCatalogEntry entry;
    entry.slug = item["slug"].get<std::string>();

    if (!ReadOptionalString(item, "display_name", entry.display_name)) {
        error_out = "display_name 字段必须是字符串";
        return std::nullopt;
    }
    if (!ReadOptionalString(item, "description", entry.description)) {
        error_out = "description 字段必须是字符串";
        return std::nullopt;
    }
    if (!ReadOptionalString(item, "default_think", entry.default_think)) {
        error_out = "default_think 字段必须是字符串";
        return std::nullopt;
    }
    if (!ReadOptionalString(item, "base_instructions", entry.base_instructions)) {
        error_out = "base_instructions 字段必须是字符串";
        return std::nullopt;
    }
    if (!ReadOptionalString(item, "truncation_policy", entry.truncation_policy)) {
        error_out = "truncation_policy 字段必须是字符串";
        return std::nullopt;
    }

    if (item.contains("supported_think_levels")) {
        const auto& levels = item["supported_think_levels"];
        if (!levels.is_array()) {
            error_out = "supported_think_levels 字段必须是数组";
            return std::nullopt;
        }
        for (std::size_t i = 0; i < levels.size(); ++i) {
            const auto& level = levels[i];
            if (!level.is_object() || !level.contains("effort") || !level["effort"].is_string() ||
                level["effort"].get<std::string>().empty()) {
                error_out = "supported_think_levels[" + std::to_string(i) +
                            "] 必须是带非空 effort(字符串)的 object";
                return std::nullopt;
            }
            ThinkLevel parsed_level;
            parsed_level.effort = level["effort"].get<std::string>();
            if (!ReadOptionalString(level, "description", parsed_level.description)) {
                error_out = "supported_think_levels[" + std::to_string(i) + "] 的 description 字段必须是字符串";
                return std::nullopt;
            }
            if (level.contains("extra_body")) {
                if (!level["extra_body"].is_object()) {
                    error_out = "supported_think_levels[" + std::to_string(i) + "] 的 extra_body 字段必须是 object";
                    return std::nullopt;
                }
                parsed_level.extra_body = level["extra_body"];
            }
            entry.supported_think_levels.push_back(std::move(parsed_level));
        }
    }

    if (item.contains("context_window")) {
        const auto& field = item["context_window"];
        std::string raw;
        if (field.is_string()) {
            raw = field.get<std::string>();
        } else if (field.is_number_integer() || field.is_number_unsigned()) {
            raw = std::to_string(field.get<long long>());
        } else {
            error_out = "context_window 字段必须是字符串或数字";
            return std::nullopt;
        }
        const auto parsed = ParseContextWindowTokens(raw);
        if (!parsed.has_value()) {
            error_out = parsed.error();
            return std::nullopt;
        }
        entry.context_window_tokens = *parsed;
    }

    if (item.contains("supports_parallel_tool_calls")) {
        if (!item["supports_parallel_tool_calls"].is_boolean()) {
            error_out = "supports_parallel_tool_calls 字段必须是布尔值";
            return std::nullopt;
        }
        entry.supports_parallel_tool_calls = item["supports_parallel_tool_calls"].get<bool>();
    }

    if (item.contains("input_modalities")) {
        const auto& modalities = item["input_modalities"];
        if (!modalities.is_array()) {
            error_out = "input_modalities 字段必须是字符串数组";
            return std::nullopt;
        }
        for (const auto& modality : modalities) {
            if (!modality.is_string()) {
                error_out = "input_modalities 数组元素必须是字符串";
                return std::nullopt;
            }
            entry.input_modalities.push_back(modality.get<std::string>());
        }
    }

    return entry;
}

ModelCatalog BuiltinModelsFromProviderCatalog() {
    const ProviderCatalog providers = LoadProviderCatalog();
    ModelCatalog catalog;
    catalog.source_path = providers.source_path;
    catalog.warnings = providers.warnings;
    for (const auto& provider : providers.providers) {
        for (const auto& model : provider.models) {
            bool duplicate = false;
            for (const auto& existing : catalog.models) {
                if (existing.slug == model.id) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            ModelCatalogEntry entry;
            entry.slug = model.id;
            entry.display_name = model.name;
            entry.description = model.description;
            entry.default_think = model.default_think;
            entry.context_window_tokens = model.context_window_tokens;
            for (const auto& variant : model.variants) {
                entry.supported_think_levels.push_back(
                    ThinkLevel{variant.id, variant.description, variant.extra_body});
            }
            catalog.models.push_back(std::move(entry));
        }
    }
    return catalog;
}

}  // namespace

const ModelCatalogEntry* ModelCatalog::FindBySlug(const std::string& slug) const {
    for (const auto& entry : models) {
        if (entry.slug == slug) {
            return &entry;
        }
    }
    return nullptr;
}

std::optional<std::string> ModelCatalogPath() {
    const auto dir = HomeLubancodeDir();
    if (!dir.has_value()) {
        return std::nullopt;
    }
    return (std::filesystem::path(*dir) / "models.json").string();
}

ModelCatalog ParseModelCatalogJson(const std::string& json_text, const std::string& file_path_for_error) {
    ModelCatalog catalog;
    catalog.source_path = file_path_for_error;

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::parse_error& e) {
        catalog.warnings.push_back("模型目录 " + file_path_for_error + " 不是合法 JSON,当作空目录: " + e.what());
        return catalog;
    }

    if (!parsed.is_object() || !parsed.contains("models")) {
        catalog.warnings.push_back("模型目录 " + file_path_for_error +
                                    " 顶层必须是 {\"models\":[...]},当作空目录");
        return catalog;
    }
    if (!parsed["models"].is_array()) {
        catalog.warnings.push_back("模型目录 " + file_path_for_error + " 的 models 字段必须是数组,当作空目录");
        return catalog;
    }

    const auto& models = parsed["models"];
    for (std::size_t i = 0; i < models.size(); ++i) {
        std::string error;
        auto entry = ParseEntry(models[i], error);
        if (!entry.has_value()) {
            catalog.warnings.push_back("模型目录 " + file_path_for_error + " 的 models[" + std::to_string(i) +
                                        "] " + error + ",跳过该条目");
            continue;
        }
        catalog.models.push_back(std::move(*entry));
    }
    return catalog;
}

ModelCatalog LoadModelCatalog() {
    ModelCatalog builtin = BuiltinModelsFromProviderCatalog();
    const auto path = ModelCatalogPath();
    if (!path.has_value()) {
        return builtin;
    }

    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(*path), ec) || ec) {
        return builtin;
    }

    std::ifstream file(std::filesystem::path(*path), std::ios::binary);
    if (!file.is_open()) {
        builtin.warnings.push_back("模型目录 " + *path + " 存在但打不开(检查权限),改用内置目录");
        return builtin;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    ModelCatalog user = ParseModelCatalogJson(buffer.str(), *path);
    // 用户 models.json 优先；只把没被用户同 slug 覆盖的内置条目补在后头。
    for (const auto& entry : builtin.models) {
        if (user.FindBySlug(entry.slug) == nullptr) user.models.push_back(entry);
    }
    user.warnings.insert(user.warnings.end(), builtin.warnings.begin(), builtin.warnings.end());
    return user;
}

std::vector<std::string> ThinkLevelHintLines(const ModelCatalogEntry* entry) {
    std::vector<std::string> lines;
    if (entry == nullptr) {
        return lines;
    }
    for (const auto& level : entry->supported_think_levels) {
        std::string line = "  - " + level.effort;
        if (!level.description.empty()) {
            line += "  " + level.description;
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

bool ThinkLevelDeclared(const ModelCatalogEntry& entry, const std::string& level) {
    const std::string wanted = ToLowerAscii(level);
    for (const auto& declared : entry.supported_think_levels) {
        if (ToLowerAscii(declared.effort) == wanted) {
            return true;
        }
    }
    return false;
}

nlohmann::json ThinkLevelExtraBody(const ModelCatalogEntry* entry, const std::string& level) {
    if (entry == nullptr) return nlohmann::json::object();
    const std::string wanted = ToLowerAscii(level);
    for (const auto& declared : entry->supported_think_levels) {
        if (ToLowerAscii(declared.effort) == wanted) return declared.extra_body;
    }
    return nlohmann::json::object();
}

CatalogApplication ComputeCatalogApplication(const ModelCatalog& catalog, const std::string& slug,
                                              bool think_explicitly_configured,
                                              bool window_explicitly_configured) {
    CatalogApplication out;
    const ModelCatalogEntry* entry = catalog.FindBySlug(slug);
    if (entry == nullptr) {
        return out;  // 不在目录:什么都不应用,base_instructions 空串 = 该清掉
    }
    out.in_catalog = true;
    if (!entry->default_think.empty() && !think_explicitly_configured) {
        out.think = entry->default_think;
    }
    if (entry->context_window_tokens.has_value() && !window_explicitly_configured) {
        out.context_window_tokens = entry->context_window_tokens;
    }
    out.base_instructions = entry->base_instructions;
    return out;
}

}  // namespace lubancode::config
