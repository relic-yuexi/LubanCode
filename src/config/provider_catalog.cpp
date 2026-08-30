#include "config/provider_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

#include <cpr/cpr.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "embedded_provider_catalog.hpp"
#include "platform/network_proxy.hpp"
#include "platform/paths.hpp"  // PathToUtf8:缓存路径不走 ACP 窄口

namespace lubancode::config {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

std::expected<void, std::string> RejectUnknown(
    const json& object, std::initializer_list<std::string_view> allowed, const std::string& where) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        const bool known = std::find(allowed.begin(), allowed.end(), std::string_view(it.key())) != allowed.end();
        if (!known) return std::unexpected(where + " 有不认识的字段: " + it.key());
    }
    return {};
}

bool ValidRevision(const std::string& revision) {
    if (revision.size() != 10 || revision[4] != '-' || revision[7] != '-') return false;
    for (std::size_t i = 0; i < revision.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (std::isdigit(static_cast<unsigned char>(revision[i])) == 0) return false;
    }
    return true;
}

std::expected<std::string, std::string> RequiredString(const json& object, const char* field,
                                                        const std::string& where) {
    auto it = object.find(field);
    if (it == object.end() || !it->is_string() || it->get<std::string>().empty()) {
        return std::unexpected(where + "." + field + " 必须是非空字符串");
    }
    return it->get<std::string>();
}

std::expected<std::optional<std::size_t>, std::string> OptionalTokenCount(
    const json& object, const char* field, const std::string& where) {
    auto it = object.find(field);
    if (it == object.end()) return std::optional<std::size_t>{};
    std::string raw;
    if (it->is_string()) raw = it->get<std::string>();
    else if (it->is_number_unsigned() || it->is_number_integer()) raw = std::to_string(it->get<long long>());
    else return std::unexpected(where + "." + field + " 必须是正整数或 k/m 字符串");
    auto parsed = ParseContextWindowTokens(raw);
    if (!parsed.has_value()) return std::unexpected(where + "." + field + ": " + parsed.error());
    return std::optional<std::size_t>{*parsed};
}

// 落线方言解析(模型协议兼容实录矩阵单 P1)。字段值域与
// api::ReasoningWireDialect 的注释一致;拼错的枚举当场报错并带坐标。
std::expected<lubancode::api::ReasoningWireDialect, std::string> ParseReasoningDialect(
    const json& value, const std::string& where) {
    if (!value.is_object()) return std::unexpected(where + " 必须是 object");
    if (auto known = RejectUnknown(value, {"toggle", "toggle_on", "toggle_off", "effort_path",
                                           "effort_param", "budget_path", "delta", "replay",
                                           "replay_field", "history_control", "history_all_value",
                                           "signature_required", "verified"},
                                   where);
        !known.has_value()) return std::unexpected(known.error());

    lubancode::api::ReasoningWireDialect out;
    const auto read_enum = [&](const char* field, std::string* target,
                               std::initializer_list<std::string_view> allowed)
        -> std::expected<void, std::string> {
        if (!value.contains(field)) return std::expected<void, std::string>{};
        if (!value[field].is_string()) {
            return std::unexpected(where + "." + field + " 必须是字符串");
        }
        const std::string parsed = value[field].get<std::string>();
        if (std::find(allowed.begin(), allowed.end(), parsed) == allowed.end()) {
            std::string choices;
            for (auto choice : allowed) choices += std::string(choice) + "/";
            return std::unexpected(where + "." + field + " 只认 " + choices + ": " + parsed);
        }
        *target = parsed;
        return std::expected<void, std::string>{};
    };
    if (auto parsed = read_enum("toggle", &out.toggle,
                                {"none", "enable_thinking_bool", "thinking_type", "include_thoughts"});
        !parsed.has_value()) return std::unexpected(parsed.error());
    if (out.toggle != "none") {
        if (out.toggle == "enable_thinking_bool") {
            if (auto parsed = read_enum("toggle_on", &out.toggle_on, {"true"}); !parsed.has_value())
                return std::unexpected(parsed.error());
            if (auto parsed = read_enum("toggle_off", &out.toggle_off, {"false"}); !parsed.has_value())
                return std::unexpected(parsed.error());
        } else {
            if (auto parsed = read_enum("toggle_on", &out.toggle_on, {"enabled", "adaptive"});
                !parsed.has_value()) return std::unexpected(parsed.error());
            if (auto parsed = read_enum("toggle_off", &out.toggle_off, {"disabled"}); !parsed.has_value())
                return std::unexpected(parsed.error());
        }
    }
    if (auto parsed = read_enum("effort_path", &out.effort_path,
                                {"none", "reasoning.effort", "reasoning_effort",
                                 "output_config.effort", "thinkingLevel"});
        !parsed.has_value()) return std::unexpected(parsed.error());
    if (out.effort_path == "none") out.effort_path.clear();
    if (value.contains("effort_param")) {
        if (!value["effort_param"].is_string() || value["effort_param"].get<std::string>().empty()) {
            return std::unexpected(where + ".effort_param 必须是非空字符串");
        }
        out.effort_param = value["effort_param"].get<std::string>();
    }
    if (auto parsed = read_enum("budget_path", &out.budget_path,
                                {"none", "thinking_budget", "thinking.budget_tokens", "thinkingBudget"});
        !parsed.has_value()) return std::unexpected(parsed.error());
    if (out.budget_path == "none") out.budget_path.clear();
    if (auto parsed = read_enum("delta", &out.delta,
                                {"none", "reasoning_content", "reasoning", "reasoning_details",
                                 "reasoning_summary", "anthropic_thinking_block",
                                 "gemini_thought_part"});
        !parsed.has_value()) return std::unexpected(parsed.error());
    if (out.delta == "none") out.delta.clear();
    if (auto parsed = read_enum("replay", &out.replay, {"never", "tool_episode", "always"});
        !parsed.has_value()) return std::unexpected(parsed.error());
    if (value.contains("replay_field")) {
        if (!value["replay_field"].is_string() || value["replay_field"].get<std::string>().empty()) {
            return std::unexpected(where + ".replay_field 必须是非空字符串");
        }
        out.replay_field = value["replay_field"].get<std::string>();
    }
    // 服务端历史控制(P1):none 显式清掉继承(provider 级声明了 thinking_keep,
    // 模型级可用 none 拒收——同 provider 里 K2.5/K3 与 K2.6 分家的那道闸)。
    if (auto parsed = read_enum("history_control", &out.history_control, {"none", "thinking_keep"});
        !parsed.has_value()) return std::unexpected(parsed.error());
    if (out.history_control == "none") out.history_control.clear();
    if (value.contains("history_all_value")) {
        if (!value["history_all_value"].is_string() || value["history_all_value"].get<std::string>().empty()) {
            return std::unexpected(where + ".history_all_value 必须是非空字符串");
        }
        out.history_all_value = value["history_all_value"].get<std::string>();
    }
    if (value.contains("signature_required")) {
        if (!value["signature_required"].is_boolean()) {
            return std::unexpected(where + ".signature_required 必须是布尔值");
        }
        out.signature_required = value["signature_required"].get<bool>();
    }
    if (value.contains("verified")) {
        if (!value["verified"].is_boolean()) {
            return std::unexpected(where + ".verified 必须是布尔值");
        }
        out.verified = value["verified"].get<bool>();
    }
    return out;
}

// 方言与 wire 家族的组合校验(L1):wire 不认的字段组合在 catalog parse
// 阶段就报错,不留到运行时猜。
std::expected<void, std::string> CheckDialectMatchesWire(const lubancode::api::ReasoningWireDialect& dialect,
                                                         Wire wire, const std::string& where) {
    if (dialect.empty()) return {};
    const auto reject = [&](const std::string& what) {
        return std::unexpected(where + " 的方言与 wire 不匹配: " + what);
    };
    const std::string wire_name = wire == Wire::ChatCompletions
                                      ? "openai-chat-completions"
                                      : wire == Wire::Responses
                                            ? "openai-responses"
                                            : wire == Wire::Anthropic
                                                  ? "anthropic-messages"
                                                  : "google-generate-content";
    // history_control=thinking_keep 只有 chat 家认(Kimi 的 thinking.keep);
    // 其余各家在各自 case 里拒掉,不留到运行时猜。
    if (dialect.history_control == "thinking_keep" && wire != Wire::ChatCompletions) {
        return reject(wire_name + " 家没有 thinking.keep 这类服务端历史控制字段");
    }
    switch (wire) {
    case Wire::ChatCompletions:
        if (dialect.toggle == "include_thoughts") return reject("chat 家没有 include_thoughts 开关");
        if (dialect.effort_path != "reasoning_effort" && !dialect.effort_path.empty()) {
            return reject("chat 家的档位只认顶层参数(effort_path=reasoning_effort),不认 " +
                          dialect.effort_path);
        }
        if (dialect.budget_path != "thinking_budget" && !dialect.budget_path.empty()) {
            return reject("chat 家的预算只认顶层 thinking_budget,不认 " + dialect.budget_path);
        }
        if (dialect.delta == "anthropic_thinking_block" || dialect.delta == "gemini_thought_part" ||
            dialect.delta == "reasoning_summary") {
            return reject("chat 家的思考增量不是 " + dialect.delta);
        }
        break;
    case Wire::Responses:
        if (dialect.toggle == "thinking_type" || dialect.toggle == "include_thoughts") {
            return reject("responses 家没有 " + dialect.toggle + " 开关(手册只有 enable_thinking 旧开关)");
        }
        if (dialect.effort_path != "reasoning.effort" && !dialect.effort_path.empty()) {
            return reject("responses 家的档位只认 reasoning.effort,不认 " + dialect.effort_path);
        }
        if (!dialect.budget_path.empty()) {
            return reject("responses 家没有思考预算参数,不认 " + dialect.budget_path);
        }
        if (!dialect.delta.empty() && dialect.delta != "reasoning_summary") {
            return reject("responses 家的思考增量是 reasoning_summary_text.delta,不是 " + dialect.delta);
        }
        break;
    case Wire::Anthropic:
        if (dialect.toggle != "thinking_type" && dialect.toggle != "none") {
            return reject("anthropic 家的开关只认 thinking.type,不认 " + dialect.toggle);
        }
        if (!dialect.effort_path.empty() && dialect.effort_path != "output_config.effort") {
            return reject("anthropic 家的档位只认 output_config.effort,不认 " + dialect.effort_path);
        }
        if (!dialect.budget_path.empty() && dialect.budget_path != "thinking.budget_tokens") {
            return reject("anthropic 家的预算只认 thinking.budget_tokens,不认 " + dialect.budget_path);
        }
        if (!dialect.delta.empty() && dialect.delta != "anthropic_thinking_block") {
            return reject("anthropic 家的思考增量是 thinking 块,不是 " + dialect.delta);
        }
        break;
    case Wire::GoogleGenerateContent:
        if (dialect.toggle != "include_thoughts" && dialect.toggle != "none") {
            return reject("gemini 家的开关只认 include_thoughts,不认 " + dialect.toggle);
        }
        if (!dialect.effort_path.empty() && dialect.effort_path != "thinkingLevel") {
            return reject("gemini 家的档位只认 thinkingLevel,不认 " + dialect.effort_path);
        }
        if (!dialect.budget_path.empty() && dialect.budget_path != "thinkingBudget") {
            return reject("gemini 家的预算只认 thinkingBudget,不认 " + dialect.budget_path);
        }
        if (!dialect.delta.empty() && dialect.delta != "gemini_thought_part") {
            return reject("gemini 家的思考增量是 thought part,不是 " + dialect.delta);
        }
        break;
    }
    // 静默吞掉 switch 之外的组合(wire_name 留给错误信息;编译器不必再警告)
    (void)wire_name;
    return {};
}

std::expected<lubancode::api::ReasoningConfig, std::string> ParseReasoning(
    const json& value, const std::string& where, const json& provider_dialect) {
    if (!value.is_object()) return std::unexpected(where + " 必须是 object");
    if (auto known = RejectUnknown(value, {"controls", "supportedEfforts", "thinkingTokenLimits",
                                           "wireDialect", "dialect", "declined"}, where);
        !known.has_value()) return std::unexpected(known.error());

    // 模型级 dialect 逐字段覆写 provider 级(L1:不可拿 provider 默认抹平
    // 模型特例,也不必整份抄)。
    json merged_dialect = provider_dialect.is_object() ? provider_dialect : json::object();
    if (value.contains("dialect")) {
        if (!value["dialect"].is_object()) {
            return std::unexpected(where + ".dialect 必须是 object");
        }
        for (auto it = value["dialect"].begin(); it != value["dialect"].end(); ++it) {
            merged_dialect[it.key()] = it.value();
        }
    }
    std::optional<lubancode::api::ReasoningWireDialect> dialect;
    if (!merged_dialect.empty()) {
        auto parsed = ParseReasoningDialect(merged_dialect, where + ".dialect");
        if (!parsed.has_value()) return std::unexpected(parsed.error());
        dialect = std::move(*parsed);
    }

    lubancode::api::ReasoningConfig out;
    // 巡检单 P2:目录明说"这模型不吃推理"(纯生成类)。true 时四家 wire
    // 停发推理参数;缺省 false(不声明 = 不猜)。
    if (value.contains("declined")) {
        if (!value["declined"].is_boolean()) {
            return std::unexpected(where + ".declined 必须是布尔值");
        }
        out.declined = value["declined"].get<bool>();
    }
    if (value.contains("supportedEfforts")) {
        if (!value["supportedEfforts"].is_array()) {
            return std::unexpected(where + ".supportedEfforts 必须是字符串数组");
        }
        for (const auto& effort : value["supportedEfforts"]) {
            if (!effort.is_string() || effort.get<std::string>().empty()) {
                return std::unexpected(where + ".supportedEfforts 必须是非空字符串数组");
            }
            out.supported_efforts.push_back(effort.get<std::string>());
        }
    }
    if (value.contains("wireDialect")) {
        if (!value["wireDialect"].is_string()) {
            return std::unexpected(where + ".wireDialect 必须是字符串");
        }
        out.wire_dialect = value["wireDialect"].get<std::string>();
        if (out.wire_dialect != "effort" && out.wire_dialect != "budget") {
            return std::unexpected(where + ".wireDialect 只认 effort/budget");
        }
    }

    const auto read_limits = [&](const json& limits, const std::string& limits_where)
        -> std::expected<void, std::string> {
        if (!limits.is_object()) return std::unexpected(limits_where + " 必须是 object");
        for (const char* field : {"min", "max"}) {
            if (!limits.contains(field)) continue;
            if (!limits[field].is_number_integer()) {
                return std::unexpected(limits_where + "." + field + " 必须是整数");
            }
            const int number = limits[field].get<int>();
            if (number < 0) return std::unexpected(limits_where + "." + field + " 不能小于 0");
            if (std::string(field) == "min") out.budget_min = number;
            else out.budget_max = number;
        }
        return {};
    };
    if (value.contains("thinkingTokenLimits")) {
        if (auto known = RejectUnknown(value["thinkingTokenLimits"], {"min", "max"},
                                       where + ".thinkingTokenLimits"); !known.has_value()) {
            return std::unexpected(known.error());
        }
        auto parsed = read_limits(value["thinkingTokenLimits"], where + ".thinkingTokenLimits");
        if (!parsed.has_value()) return std::unexpected(parsed.error());
    }
    if (value.contains("controls")) {
        if (!value["controls"].is_array()) return std::unexpected(where + ".controls 必须是数组");
        for (std::size_t i = 0; i < value["controls"].size(); ++i) {
            const auto& control = value["controls"][i];
            const std::string control_where = where + ".controls[" + std::to_string(i) + "]";
            if (!control.is_object()) return std::unexpected(control_where + " 必须是 object");
            if (auto known = RejectUnknown(control, {"kind", "values", "min", "max"}, control_where);
                !known.has_value()) return std::unexpected(known.error());
            auto kind = RequiredString(control, "kind", control_where);
            if (!kind.has_value()) return std::unexpected(kind.error());
            if (*kind == "toggle") {
                out.supports_toggle = true;
            } else if (*kind == "budget") {
                auto parsed = read_limits(control, control_where);
                if (!parsed.has_value()) return std::unexpected(parsed.error());
            } else if (*kind == "effort") {
                out.supports_effort = true;
                if (!control.contains("values") || !control["values"].is_array()) {
                    return std::unexpected(control_where + ".values 必须是字符串数组");
                }
                if (out.supported_efforts.empty()) {
                    for (const auto& effort : control["values"]) {
                        if (!effort.is_string() || effort.get<std::string>().empty()) {
                            return std::unexpected(control_where + ".values 必须是非空字符串数组");
                        }
                        out.supported_efforts.push_back(effort.get<std::string>());
                    }
                }
            } else {
                return std::unexpected(control_where + ".kind 不认识: " + *kind);
            }
        }
    }
    if (out.budget_min.has_value() && out.budget_max.has_value() && *out.budget_min > *out.budget_max) {
        return std::unexpected(where + " 的 budget min 不能大于 max");
    }
    if (dialect.has_value()) out.dialect = std::move(*dialect);
    return out;
}

std::expected<ProviderCatalogModel, std::string> ParseModel(const std::string& id, const json& value,
                                                            const std::string& where,
                                                            const json& provider_dialect,
                                                            Wire wire) {
    if (!value.is_object()) return std::unexpected(where + " 必须是 JSON object");
    if (auto known = RejectUnknown(value, {"name", "description", "context_window", "max_output",
                                           "default_think", "capabilities", "reasoning"}, where);
        !known.has_value()) return std::unexpected(known.error());
    ProviderCatalogModel model;
    model.id = id;
    auto name = RequiredString(value, "name", where);
    if (!name.has_value()) return std::unexpected(name.error());
    model.name = *name;
    if (value.contains("description")) {
        if (!value["description"].is_string()) return std::unexpected(where + ".description 必须是字符串");
        model.description = value["description"].get<std::string>();
    }
    auto context = OptionalTokenCount(value, "context_window", where);
    if (!context.has_value()) return std::unexpected(context.error());
    model.context_window_tokens = *context;
    auto output = OptionalTokenCount(value, "max_output", where);
    if (!output.has_value()) return std::unexpected(output.error());
    model.max_output_tokens = *output;
    if (value.contains("default_think")) {
        if (!value["default_think"].is_string()) return std::unexpected(where + ".default_think 必须是字符串");
        model.default_think = value["default_think"].get<std::string>();
    }
    if (value.contains("capabilities")) {
        if (!value["capabilities"].is_object()) return std::unexpected(where + ".capabilities 必须是 object");
        for (auto it = value["capabilities"].begin(); it != value["capabilities"].end(); ++it) {
            if (!it.value().is_boolean()) {
                return std::unexpected(where + ".capabilities." + it.key() + " 必须是布尔值");
            }
            model.capabilities[it.key()] = it.value().get<bool>();
        }
    }
    if (value.contains("reasoning")) {
        auto reasoning = ParseReasoning(value["reasoning"], where + ".reasoning", provider_dialect);
        if (!reasoning.has_value()) return std::unexpected(reasoning.error());
        if (auto matched = CheckDialectMatchesWire(reasoning->dialect, wire, where);
            !matched.has_value()) return std::unexpected(matched.error());
        model.reasoning = std::move(*reasoning);
    }
    return model;
}

std::expected<ProviderPreset, std::string> ParseProvider(const std::string& id, const json& value) {
    const std::string where = "providers." + id;
    if (!value.is_object()) return std::unexpected(where + " 必须是 JSON object");
    if (auto known = RejectUnknown(value, {"name", "description", "wire", "base_url", "key_env",
                                           "default_model", "model_reasoning_effort", "native_web_search",
                                           "stream_usage", "reasoning_replay", "reasoning_delta_field",
                                           "reasoning_replay_field", "reasoning_dialect", "docs_url",
                                           "extra_body", "extra_headers", "models"},
                                   where);
        !known.has_value()) return std::unexpected(known.error());
    ProviderPreset preset;
    preset.id = id;
    for (const char* field : {"name", "base_url", "key_env", "default_model"}) {
        auto parsed = RequiredString(value, field, where);
        if (!parsed.has_value()) return std::unexpected(parsed.error());
        if (std::string(field) == "name") preset.name = *parsed;
        else if (std::string(field) == "base_url") preset.base_url = *parsed;
        else if (std::string(field) == "key_env") preset.key_env = *parsed;
        else preset.default_model = *parsed;
    }
    auto wire = RequiredString(value, "wire", where);
    if (!wire.has_value()) return std::unexpected(wire.error());
    auto parsed_wire = ParseProviderWire(*wire);
    if (!parsed_wire.has_value()) return std::unexpected(where + ".wire: " + parsed_wire.error());
    preset.wire = *parsed_wire;

    // provider 级落线方言(P1):models[].reasoning.dialect 可逐字段覆写。
    json provider_dialect = json::object();
    if (value.contains("reasoning_dialect")) {
        auto dialect = ParseReasoningDialect(value["reasoning_dialect"], where + ".reasoning_dialect");
        if (!dialect.has_value()) return std::unexpected(dialect.error());
        if (auto matched = CheckDialectMatchesWire(*dialect, preset.wire, where + ".reasoning_dialect");
            !matched.has_value()) return std::unexpected(matched.error());
        preset.reasoning_dialect = *dialect;
        provider_dialect = value["reasoning_dialect"];
    }
    if (preset.base_url.rfind("https://", 0) != 0) return std::unexpected(where + ".base_url 必须以 https:// 开头");
    if (value.contains("description")) {
        if (!value["description"].is_string()) return std::unexpected(where + ".description 必须是字符串");
        preset.description = value["description"].get<std::string>();
    }
    if (value.contains("model_reasoning_effort")) {
        if (!value["model_reasoning_effort"].is_string()) {
            return std::unexpected(where + ".model_reasoning_effort 必须是字符串");
        }
        preset.model_reasoning_effort = value["model_reasoning_effort"].get<std::string>();
    }
    if (value.contains("native_web_search")) {
        if (!value["native_web_search"].is_boolean()) {
            return std::unexpected(where + ".native_web_search 必须是布尔值");
        }
        preset.native_web_search = value["native_web_search"].get<bool>();
    }
    if (value.contains("stream_usage")) {
        if (!value["stream_usage"].is_boolean()) {
            return std::unexpected(where + ".stream_usage 必须是布尔值");
        }
        preset.stream_usage = value["stream_usage"].get<bool>();
    }
    if (value.contains("reasoning_replay")) {
        if (!value["reasoning_replay"].is_string()) {
            return std::unexpected(where + ".reasoning_replay 必须是字符串");
        }
        const std::string replay = value["reasoning_replay"].get<std::string>();
        if (replay != "never" && replay != "tool_episode") {
            return std::unexpected(where + ".reasoning_replay 只认 never/tool_episode: " + replay);
        }
        preset.reasoning_replay = replay;
    }
    // 思考字段名声明(只认非空字符串;名字对不对由服务端说了算,这里
    // 不猜合法值——声明错字的代价是思考解析不到,诊断行看得见)。
    for (const char* field : {"reasoning_delta_field", "reasoning_replay_field"}) {
        if (value.contains(field)) {
            if (!value[field].is_string() || value[field].get<std::string>().empty()) {
                return std::unexpected(where + "." + field + " 必须是非空字符串");
            }
            const std::string declared = value[field].get<std::string>();
            if (std::string(field) == "reasoning_delta_field") preset.reasoning_delta_field = declared;
            else preset.reasoning_replay_field = declared;
        }
    }
    if (value.contains("docs_url")) {
        if (!value["docs_url"].is_string()) return std::unexpected(where + ".docs_url 必须是字符串");
        preset.docs_url = value["docs_url"].get<std::string>();
    }
    if (value.contains("extra_body")) {
        if (!value["extra_body"].is_object()) return std::unexpected(where + ".extra_body 必须是 object");
        preset.extra_body = value["extra_body"];
    }
    if (value.contains("extra_headers")) {
        if (!value["extra_headers"].is_object()) return std::unexpected(where + ".extra_headers 必须是 object");
        for (auto it = value["extra_headers"].begin(); it != value["extra_headers"].end(); ++it) {
            if (!it.value().is_string()) return std::unexpected(where + ".extra_headers." + it.key() + " 必须是字符串");
            preset.extra_headers[it.key()] = it.value().get<std::string>();
        }
    }
    auto models = value.find("models");
    if (models == value.end() || !models->is_object() || models->empty()) {
        return std::unexpected(where + ".models 必须是非空 JSON object");
    }
    for (auto it = models->begin(); it != models->end(); ++it) {
        auto model = ParseModel(it.key(), it.value(), where + ".models." + it.key(), provider_dialect,
                                preset.wire);
        if (!model.has_value()) return std::unexpected(model.error());
        preset.models.push_back(std::move(*model));
    }
    if (preset.FindModel(preset.default_model) == nullptr) {
        return std::unexpected(where + ".default_model 不在 models 中: " + preset.default_model);
    }
    return preset;
}

std::optional<fs::path> CacheDir() {
    const auto home = HomeLubancodeDir();
    if (!home.has_value()) return std::nullopt;
    return fs::path(*home) / "cache";
}

std::optional<std::string> ReadSmallFile(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > kProviderCatalogMaxBytes) return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::expected<void, std::string> AtomicWrite(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return std::unexpected("建立目录失败: " + ec.message());
    fs::path temp = path;
    temp += ".tmp";  // 纯 ASCII 后缀,窄口拼接不涉代码页
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return std::unexpected("临时文件打不开: " + platform::PathToUtf8(temp));
        out << text;
        if (!out.good()) return std::unexpected("临时文件写入失败: " + platform::PathToUtf8(temp));
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = GetLastError();
        fs::remove(temp, ec);
        return std::unexpected("替换缓存失败，Win32 错误码 " + std::to_string(move_error));
    }
#else
    fs::rename(temp, path, ec);  // POSIX rename 同卷原子替换已有文件
    if (ec) {
        fs::remove(temp, ec);
        return std::unexpected("替换缓存失败: " + ec.message());
    }
#endif
    return {};
}

std::string HeaderValue(const cpr::Header& headers, const std::string& wanted) {
    for (const auto& [name, value] : headers) {
        if (name.size() != wanted.size()) continue;
        bool equal = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(name[i])) !=
                std::tolower(static_cast<unsigned char>(wanted[i]))) {
                equal = false;
                break;
            }
        }
        if (equal) return value;
    }
    return {};
}

fs::path MetaPath(const fs::path& cache) { return cache.parent_path() / "provider-catalog.meta.json"; }

json ReadMeta(const fs::path& cache) {
    const auto text = ReadSmallFile(MetaPath(cache));
    if (!text.has_value()) return json::object();
    try {
        json parsed = json::parse(*text);
        return parsed.is_object() ? parsed : json::object();
    } catch (const json::exception&) {
        return json::object();
    }
}

}  // namespace

const ProviderCatalogModel* ProviderPreset::FindModel(const std::string& model_id) const {
    for (const auto& model : models) if (model.id == model_id) return &model;
    return nullptr;
}

const ProviderPreset* ProviderCatalog::FindProvider(const std::string& id) const {
    for (const auto& provider : providers) if (provider.id == id) return &provider;
    return nullptr;
}

std::expected<ProviderCatalog, std::string> ParseProviderCatalogJson(const std::string& text,
                                                                      const std::string& source_path) {
    if (text.size() > kProviderCatalogMaxBytes) return std::unexpected("provider 目录超过 2MB 上限");
    json root;
    try {
        root = json::parse(text);
    } catch (const json::exception& e) {
        return std::unexpected("provider 目录不是合法 JSON: " + std::string(e.what()));
    }
    if (!root.is_object()) return std::unexpected("provider 目录顶层必须是 JSON object");
    if (auto known = RejectUnknown(root, {"schema_version", "revision", "providers"}, "catalog");
        !known.has_value()) return std::unexpected(known.error());
    if (!root.contains("schema_version") || !root["schema_version"].is_number_integer() ||
        root["schema_version"].get<int>() != kProviderCatalogSchemaVersion) {
        return std::unexpected("provider 目录 schema_version 不受支持");
    }
    auto revision = RequiredString(root, "revision", "catalog");
    if (!revision.has_value()) return std::unexpected(revision.error());
    if (!ValidRevision(*revision)) return std::unexpected("catalog.revision 必须是 YYYY-MM-DD");
    auto providers = root.find("providers");
    if (providers == root.end() || !providers->is_object() || providers->empty()) {
        return std::unexpected("provider 目录 providers 必须是非空 object");
    }
    ProviderCatalog catalog;
    catalog.revision = *revision;
    catalog.source_path = source_path;
    for (auto it = providers->begin(); it != providers->end(); ++it) {
        auto provider = ParseProvider(it.key(), it.value());
        if (!provider.has_value()) return std::unexpected(provider.error());
        catalog.providers.push_back(std::move(*provider));
    }
    return catalog;
}

std::optional<std::string> ProviderCatalogCachePath() {
    const auto dir = CacheDir();
    if (!dir.has_value()) return std::nullopt;
    return platform::PathToUtf8(*dir / "provider-catalog.json");
}

ProviderCatalog LoadProviderCatalog() {
    auto builtin = ParseProviderCatalogJson(embedded::kProviderCatalogJson, "<内置 provider 目录>");
    ProviderCatalog catalog;
    if (builtin.has_value()) catalog = std::move(*builtin);
    else catalog.warnings.push_back("内置 provider 目录损坏: " + builtin.error());

    const auto cache_path = ProviderCatalogCachePath();
    if (!cache_path.has_value()) return catalog;
    std::error_code ec;
    if (!fs::exists(*cache_path, ec) || ec) return catalog;
    const auto text = ReadSmallFile(*cache_path);
    if (!text.has_value()) {
        catalog.warnings.push_back("provider 目录缓存打不开或超过 2MB，改用内置快照");
        return catalog;
    }
    auto cached = ParseProviderCatalogJson(*text, *cache_path);
    if (!cached.has_value()) {
        catalog.warnings.push_back("provider 目录缓存无效，改用内置快照: " + cached.error());
        return catalog;
    }
    if (cached->revision < catalog.revision) {
        catalog.warnings.push_back("provider 目录缓存比 exe 内置快照旧，已忽略旧缓存");
        return catalog;
    }
    cached->warnings = std::move(catalog.warnings);
    return std::move(*cached);
}

bool ProviderCatalogCacheIsStale(std::chrono::hours max_age) {
    const auto cache_path = ProviderCatalogCachePath();
    if (!cache_path.has_value()) return false;
    const json meta = ReadMeta(*cache_path);
    if (!meta.contains("checked_at") || !meta["checked_at"].is_number_integer()) return true;
    const auto checked = static_cast<std::time_t>(meta["checked_at"].get<long long>());
    return std::time(nullptr) - checked > std::chrono::duration_cast<std::chrono::seconds>(max_age).count();
}

std::expected<ProviderCatalogRefresh, std::string> RefreshProviderCatalog(int connect_timeout_ms,
                                                                           int request_timeout_secs) {
    const auto cache_string = ProviderCatalogCachePath();
    if (!cache_string.has_value()) return std::unexpected("找不到用户主目录，没法写 provider 目录缓存");
    const fs::path cache(*cache_string);
    const json old_meta = ReadMeta(cache);
    cpr::Header headers{{"User-Agent", "lubancode-provider-catalog/1"}};
    if (old_meta.contains("etag") && old_meta["etag"].is_string() && !old_meta["etag"].get<std::string>().empty()) {
        headers["If-None-Match"] = old_meta["etag"].get<std::string>();
    }
    cpr::Session session;
    session.SetUrl(cpr::Url{kProviderCatalogUrl});
    session.SetHeader(headers);
    session.SetConnectTimeout(cpr::ConnectTimeout{std::chrono::milliseconds(connect_timeout_ms)});
    session.SetTimeout(cpr::Timeout{std::chrono::seconds(request_timeout_secs)});
    if (const auto proxy = platform::SystemProxyForScheme("https"); proxy.has_value()) {
        session.SetProxies(cpr::Proxies{{"https", *proxy}});
    }
    const cpr::Response response = session.Get();
    if (response.error) return std::unexpected("拉取 provider 目录失败: " + response.error.message);

    ProviderCatalogRefresh result;
    result.cache_path = platform::PathToUtf8(cache);
    json meta = old_meta;
    meta["checked_at"] = static_cast<long long>(std::time(nullptr));
    const std::string etag = HeaderValue(response.header, "etag");
    if (!etag.empty()) meta["etag"] = etag;

    if (response.status_code == 304) {
        auto written = AtomicWrite(MetaPath(cache), meta.dump(2));
        if (!written.has_value()) return std::unexpected(written.error());
        result.not_modified = true;
        return result;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        return std::unexpected("拉取 provider 目录失败: HTTP " + std::to_string(response.status_code));
    }
    auto parsed = ParseProviderCatalogJson(response.text, kProviderCatalogUrl);
    if (!parsed.has_value()) return std::unexpected("远端 provider 目录校验失败: " + parsed.error());
    const ProviderCatalog current = LoadProviderCatalog();
    if (!current.revision.empty() && parsed->revision < current.revision) {
        return std::unexpected("远端 provider 目录版本倒退，拒绝用 " + parsed->revision +
                               " 覆盖 " + current.revision);
    }
    auto written = AtomicWrite(cache, response.text);
    if (!written.has_value()) return std::unexpected(written.error());
    auto meta_written = AtomicWrite(MetaPath(cache), meta.dump(2));
    if (!meta_written.has_value()) return std::unexpected(meta_written.error());
    result.updated = true;
    result.revision = parsed->revision;
    return result;
}

std::string DescribeReasoningDialect(const lubancode::api::ReasoningWireDialect& dialect) {
    if (dialect.empty()) return {};
    std::vector<std::string> parts;
    if (dialect.toggle == "enable_thinking_bool") {
        parts.push_back("enable_thinking=" + dialect.toggle_on + "/" + dialect.toggle_off);
    } else if (dialect.toggle == "thinking_type") {
        parts.push_back("thinking.type=" + dialect.toggle_on + "/" + dialect.toggle_off);
    } else if (dialect.toggle == "include_thoughts") {
        parts.push_back("generationConfig.thinkingConfig.includeThoughts");
    }
    if (dialect.effort_path == "reasoning_effort" && !dialect.effort_param.empty()) {
        parts.push_back(dialect.effort_param);
    } else if (!dialect.effort_path.empty()) {
        parts.push_back(dialect.effort_path);
    }
    if (!dialect.budget_path.empty()) parts.push_back(dialect.budget_path);
    if (!dialect.delta.empty()) parts.push_back("delta:" + dialect.delta);
    // 思考回传策略一并亮(never 不刷屏——那是缺省;always/tool_episode
    // 是正式契约,用户该看见"历史思考要不要送回去")。
    if (dialect.replay != "never" || !dialect.replay_field.empty()) {
        parts.push_back("replay:" + dialect.replay +
                        (dialect.replay_field.empty() ? std::string() : "->" + dialect.replay_field));
    }
    // 服务端历史控制(P1):可请求保留的模型亮出形状;没有声明就不刷屏
    // ——"不发请求字段"对 K3/K2.7 是"服务端固定",对 K2.5 是"不支持",
    // 分野由 /think history 的能力行说,这里只报方言自己声明的形状。
    if (dialect.history_control == "thinking_keep") {
        parts.push_back("history:" + dialect.history_control + "=" +
                        (dialect.history_all_value.empty() ? std::string("all") : dialect.history_all_value));
    }
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) out += " · ";
        out += part;
    }
    return out;
}

ProviderConfig ProviderConfigFromPreset(const ProviderPreset& preset) {
    ProviderConfig provider;
    provider.name = preset.id;
    provider.base_url = preset.base_url;
    provider.wire = preset.wire;
    provider.key_env = preset.key_env;
    provider.model = preset.default_model;
    provider.model_reasoning_effort = preset.model_reasoning_effort;
    provider.native_web_search = preset.native_web_search;
    provider.stream_usage = preset.stream_usage;
    // 目录预设的 stream_usage 是一份显式声明(哪怕值是 false——那家确认
    // 不支持),与"自定义端压根没写"区分开,启动提醒只找后者。
    provider.stream_usage_declared = true;
    provider.reasoning_replay = preset.reasoning_replay;
    provider.reasoning_delta_field = preset.reasoning_delta_field;
    provider.reasoning_replay_field = preset.reasoning_replay_field;
    provider.extra_body = preset.extra_body;
    provider.extra_headers = preset.extra_headers;
    if (const auto* model = preset.FindModel(preset.default_model); model != nullptr) {
        if (model->context_window_tokens.has_value()) provider.context_window_tokens = *model->context_window_tokens;
        if (model->max_output_tokens.has_value()) provider.max_output_tokens = *model->max_output_tokens;
        if (provider.model_reasoning_effort.empty()) provider.model_reasoning_effort = model->default_think;
    }
    return provider;
}

std::map<std::string, std::string> ResolveProviderHeaderTemplates(
    const std::map<std::string, std::string>& headers, const std::string& api_key) {
    auto resolved = headers;
    constexpr std::string_view marker = "${LUBANCODE_API_KEY}";
    for (auto& [_, value] : resolved) {
        std::size_t pos = 0;
        while ((pos = value.find(marker, pos)) != std::string::npos) {
            value.replace(pos, marker.size(), api_key);
            pos += api_key.size();
        }
    }
    return resolved;
}

}  // namespace lubancode::config
