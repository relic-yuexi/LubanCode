#include "config/model_catalog.hpp"

#include <cctype>
#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "config/config.hpp"

#include "platform/paths.hpp"  // PathToUtf8:目录名窄口不走 ACP
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

    // provider_id 可选:内置条目声明归属;用户 models.json 手写的条目
    // 留空 = 全局覆盖。活列表选择落痕(RememberModelChoiceInCatalog)写的
    // 用户条目带这字段——"这家确实用过这模型"的凭据,跨家判定认它。
    if (!ReadOptionalString(item, "provider_id", entry.provider_id)) {
        error_out = "provider_id 字段必须是字符串";
        return std::nullopt;
    }
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
            entry.supported_think_levels.push_back(std::move(parsed_level));
            entry.reasoning.supported_efforts.push_back(entry.supported_think_levels.back().effort);
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

    // 输出上限声明(规格根因一):与 context_window 同一套换算("8k"/
    // 裸数字),坏条目跳过。它是三级声明的最底一级(agent::ResolveOutput-
    // Budget):配置文件与 provider 声明都缺席才轮到它;再缺席 = unset。
    if (item.contains("max_output_tokens")) {
        const auto& field = item["max_output_tokens"];
        std::string raw;
        if (field.is_string()) {
            raw = field.get<std::string>();
        } else if (field.is_number_integer() || field.is_number_unsigned()) {
            raw = std::to_string(field.get<long long>());
        } else {
            error_out = "max_output_tokens 字段必须是字符串或数字";
            return std::nullopt;
        }
        const auto parsed = ParseContextWindowTokens(raw);
        if (!parsed.has_value()) {
            error_out = parsed.error();
            return std::nullopt;
        }
        entry.max_output_tokens = *parsed;
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

    // capabilities(端点能力,ccmoon 巡检单 P1):object,键任意、值必须
    // 布尔。与 provider catalog 的 capabilities 同一形状,/model 的端点
    // 相性提示(ClassifyModelEndpoint)吃它。
    if (item.contains("capabilities")) {
        if (!item["capabilities"].is_object()) {
            error_out = "capabilities 字段必须是 object";
            return std::nullopt;
        }
        for (auto cap = item["capabilities"].begin(); cap != item["capabilities"].end(); ++cap) {
            if (!cap.value().is_boolean()) {
                error_out = "capabilities." + cap.key() + " 必须是布尔值";
                return std::nullopt;
            }
            entry.capabilities[cap.key()] = cap.value().get<bool>();
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
            ModelCatalogEntry entry;
            entry.provider_id = provider.id;
            entry.slug = model.id;
            entry.display_name = model.name;
            entry.description = model.description;
            entry.default_think = model.default_think;
            entry.context_window_tokens = model.context_window_tokens;
            entry.max_output_tokens = model.max_output_tokens;
            entry.reasoning = model.reasoning;
            entry.capabilities = model.capabilities;
            // 巡检单 P2(推理档位边界):目录声明"只出图、不吃推理"的模型
            //(image-generation=true 且 reasoning 不在能力表),推理档案
            // 显式落 declined——四家 wire 一律停发推理参数;用户档位
            //(current_think)不动,切回推理模型照旧生效。目录没写
            // capabilities 的照旧走 legacy(不猜)。
            if (model.reasoning.empty() && !model.capabilities.empty()) {
                const auto cap = [&model](const char* key) {
                    const auto it = model.capabilities.find(key);
                    return it != model.capabilities.end() && it->second;
                };
                if (cap("image-generation") && !cap("reasoning")) {
                    entry.reasoning.declined = true;
                }
            }
            for (const auto& effort : model.reasoning.supported_efforts) {
                entry.supported_think_levels.push_back(ThinkLevel{effort, {}});
            }
            catalog.models.push_back(std::move(entry));
        }
    }
    return catalog;
}

}  // namespace

ModelEndpointKind ClassifyModelEndpoint(const ModelCatalogEntry* entry, const std::string& model_id) {
    const auto cap = [entry](const char* key) {
        if (entry == nullptr) {
            return false;
        }
        const auto it = entry->capabilities.find(key);
        return it != entry->capabilities.end() && it->second;
    };
    // 名字兜底:中转家的活列表常有目录没收的名字(ccmoon 的
    // gpt-4o-realtime-preview 就不在内置目录里),"realtime" 字样是行业
    // 通名,认它不至于误伤普通模型。
    const std::string lowered = ToLowerAscii(model_id);
    if (cap("realtime") || lowered.find("realtime") != std::string::npos) {
        return ModelEndpointKind::Realtime;
    }
    if (cap("image-generation") && !cap("reasoning")) {
        return ModelEndpointKind::ImageGen;
    }
    return ModelEndpointKind::Standard;
}

const ModelCatalogEntry* ModelCatalog::FindBySlug(const std::string& slug) const {
    for (const auto& entry : models) {
        if (entry.slug == slug) {
            return &entry;
        }
    }
    return nullptr;
}

const ModelCatalogEntry* ModelCatalog::FindByProviderAndSlug(const std::string& provider,
                                                              const std::string& slug) const {
    // 用户目录条目没有 provider_id，照旧压过内置档案。
    for (const auto& entry : models) {
        if (entry.provider_id.empty() && entry.slug == slug) return &entry;
    }
    for (const auto& entry : models) {
        if (entry.provider_id == provider && entry.slug == slug) return &entry;
    }
    return FindBySlug(slug);
}

std::optional<std::string> ModelCatalogPath() {
    const auto dir = HomeLubancodeDir();
    if (!dir.has_value()) {
        return std::nullopt;
    }
    // dir 已是 UTF-8 字符串,纯 ASCII 文件名直接拼接,不绕 path 窄口(ACP)。
    return *dir + "/models.json";
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
    // 用户 models.json 优先；同 slug 的用户条目压过各家内置条目。
    for (const auto& entry : builtin.models) {
        if (user.FindBySlug(entry.slug) == nullptr) user.models.push_back(entry);
    }
    user.warnings.insert(user.warnings.end(), builtin.warnings.begin(), builtin.warnings.end());
    return user;
}

std::expected<void, std::string> RememberModelChoiceInCatalog(const std::string& models_json_path,
                                                              const std::string& provider_id,
                                                              const std::string& slug,
                                                              const std::string& display_name) {
    // 活列表选择落痕(跨家判定第三轮返件):/model 切成的模型在当前家落
    // 一条用户条目 {"slug","provider_id","display_name"} 进 models.json——
    // "这家确实用过这模型"的真凭据,比任何目录猜测都硬。此后跨家判定
    // 第一步(当前家条目)认它,零提示零动作。
    //
    // 直接在 json 树上读改写,不走 ModelCatalogEntry 往返:条目上已有的
    // 别的字段(effort 声明、窗口、指令)一个都不许冲掉。幂等:同 slug
    // 且同 provider_id 的条目已在,原样返回,一个字节不动。文件不存在
    // 从头建;坏 JSON/形状不对报错不写,绝不覆盖用户手写的目录。
    if (models_json_path.empty()) {
        return std::unexpected("没有 models.json 路径(找不到用户主目录)");
    }
    nlohmann::json root = nlohmann::json::object();
    root["models"] = nlohmann::json::array();
    std::error_code ec;
    const std::filesystem::path path(models_json_path);
    if (std::filesystem::exists(path, ec) && !ec) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            return std::unexpected("models.json 打不开(检查权限)");
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        try {
            root = nlohmann::json::parse(buffer.str());
        } catch (const nlohmann::json::exception& e) {
            return std::unexpected(std::string("models.json 不是合法 JSON,不落痕: ") + e.what());
        }
        if (!root.is_object() || !root.contains("models") || !root["models"].is_array()) {
            return std::unexpected("models.json 形状不对(要 {\"models\":[...]}),不落痕");
        }
    }
    for (const auto& item : root["models"]) {
        if (!item.is_object()) {
            continue;
        }
        const bool same_slug = item.contains("slug") && item["slug"].is_string() &&
                               item["slug"].get<std::string>() == slug;
        const bool same_provider = item.contains("provider_id") && item["provider_id"].is_string() &&
                                   item["provider_id"].get<std::string>() == provider_id;
        if (same_slug && same_provider) {
            return {};  // 已有这条落痕:幂等,别冲已有字段
        }
    }
    nlohmann::json entry = nlohmann::json::object();
    entry["slug"] = slug;
    entry["provider_id"] = provider_id;
    if (!display_name.empty()) {
        entry["display_name"] = display_name;
    }
    root["models"].push_back(std::move(entry));
    std::error_code dir_ec;
    std::filesystem::create_directories(path.parent_path(), dir_ec);  // 已存在不报错
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return std::unexpected("models.json 写不进去(目录不存在或没权限)");
    }
    out << root.dump(2) << "\n";
    out.flush();
    if (!out) {
        return std::unexpected("models.json 写入中断,落痕可能没保存");
    }
    return {};
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
