#include "api/gemini/request.hpp"

#include <map>
#include <type_traits>
#include <variant>

namespace lubancode::api::gemini {

namespace {

using nlohmann::json;

// Role -> wire 角色名,共用件(批六归一):gemini 的另一角叫 "model"。
std::string WireRole(Role role) {
    return RoleToString(role, "model");
}

// 工具结果回传(functionResponse)只认函数名,中立层 ToolResultBlock 里却
// 只有 tool_use_id——先扫一遍全部历史,把 assistant 发起过的每次调用按
// id 记下函数名,后面翻 ToolResultBlock 时对回来。历史里对不上号的(上游
// 少给了那条 assistant 消息)退回用 id 本身当名字:请求不至于拼坏,服务端
// 要真不认会以 400 说清楚,好过本地悄悄丢结果。
std::map<std::string, std::string> ToolNameByUseId(const std::vector<Message>& messages) {
    std::map<std::string, std::string> names;
    for (const auto& message : messages) {
        for (const auto& block : message.content) {
            if (const auto* call = std::get_if<ToolUseBlock>(&block)) {
                names[call->id] = call->name;
            }
        }
    }
    return names;
}

// ToolResultBlock.content(字符串)翻成 functionResponse.response(必须是
// object):内容本身是 JSON object 就原样用;否则包一层 {"result": ...}——
// Google 官方 SDK 对非结构化结果就是这副形状。is_error 的结果换
// {"error": ...} 让模型看得见这是失败回执。
json FunctionResponseBody(const ToolResultBlock& result) {
    if (result.is_error) {
        return json{{"error", result.content}};
    }
    try {
        const json parsed = json::parse(result.content);
        if (parsed.is_object()) {
            return parsed;
        }
    } catch (const json::exception&) {
        // 不是合法 JSON,走下面的兜底包装。
    }
    return json{{"result", result.content}};
}

}  // namespace

nlohmann::json BuildRequestJson(const Request& request, const json& extra_body) {
    json body;

    // 系统提示走 systemInstruction(角色外置),不掺进 contents——Gemini 的
    // contents 里没有 system 这一角。
    if (!request.system.empty()) {
        body["systemInstruction"] = json{{"parts", json::array({json{{"text", request.system}}})}};
    }

    json contents = json::array();

    const std::map<std::string, std::string> tool_names = ToolNameByUseId(request.messages);

    for (const auto& message : request.messages) {
        // 普通内容(文本/图片)攒成一条 content,工具块各自单独成条——
        // Gemini 的 parts 可以混装,但 functionCall/functionResponse 单独
        // 一条 content 语义最清楚,也不依赖服务端对混合 parts 的容忍度。
        json parts = json::array();
        const auto flush_parts = [&] {
            if (!parts.empty()) {
                contents.push_back(json{{"role", WireRole(message.role)}, {"parts", std::move(parts)}});
                parts = json::array();
            }
        };
        for (const auto& block : message.content) {
            std::visit(
                [&](const auto& b) {
                    using T = std::decay_t<decltype(b)>;
                    if constexpr (std::is_same_v<T, TextBlock>) {
                        parts.push_back(json{{"text", b.text}});
                    } else if constexpr (std::is_same_v<T, ImageBlock>) {
                        parts.push_back(json{{"inlineData", json{{"mimeType", b.media_type}, {"data", b.data}}}});
                    } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                        // 思考不回传:Gemini 的 thought 一次性,续会话不重放。
                    } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                        flush_parts();
                        contents.push_back(
                            json{{"role", "model"},
                                 {"parts", json::array({json{{"functionCall",
                                                              json{{"name", b.name},
                                                                   {"args", b.input.is_object() ? b.input : json::object()}}}}})}});
                    } else {
                        flush_parts();
                        const std::string name = tool_names.count(b.tool_use_id) != 0
                                                     ? tool_names.at(b.tool_use_id)
                                                     : b.tool_use_id;
                        contents.push_back(
                            json{{"role", "user"},
                                 {"parts", json::array({json{{"functionResponse",
                                                              json{{"name", name},
                                                                   {"response", FunctionResponseBody(b)}}}}})}});
                    }
                },
                block);
        }
        flush_parts();
    }
    body["contents"] = std::move(contents);

    if (!request.tools.empty()) {
        json declarations = json::array();
        for (const auto& tool : request.tools) {
            declarations.push_back(json{{"name", tool.name},
                                        {"description", tool.description},
                                        {"parameters", ToolSchemaForWire(tool.input_schema)}});
        }
        body["tools"] = json::array({json{{"functionDeclarations", std::move(declarations)}}});
    }

    json generation_config = json::object();
    // maxOutputTokens 可省略:unset 就整个不带字段,交服务端/模型默认
    // (与 chat/responses 同一取舍,Request::max_tokens 注释)。
    if (request.max_tokens.has_value()) {
        generation_config["maxOutputTokens"] = *request.max_tokens;
    }
    // 推理档案决定写 thinkingLevel 还是 thinkingBudget；none/minimal 关。
    if (!request.reasoning_effort.empty()) {
        const bool off = ReasoningEffortIsOff(request.reasoning_effort);
        json thinking_config{{"includeThoughts", !off}};
        if (off && request.reasoning.supports_toggle) {
            thinking_config["thinkingBudget"] = 0;
        } else if (!off && (request.reasoning.wire_dialect == "effort" ||
                            request.reasoning.supports_effort)) {
            thinking_config["thinkingLevel"] = LowerReasoningEffort(request.reasoning_effort);
        } else if (!off && (request.reasoning.wire_dialect == "budget" ||
                            request.reasoning.budget_max.has_value())) {
            thinking_config["thinkingBudget"] =
                LowerReasoningEffort(request.reasoning_effort) == "auto"
                    ? -1
                    : ReasoningBudgetForEffort(request.reasoning, request.reasoning_effort,
                                               request.max_tokens.value_or(0));
        }
        generation_config["thinkingConfig"] = std::move(thinking_config);
    }
    if (!generation_config.empty()) {
        body["generationConfig"] = std::move(generation_config);
    }

    // extra_body 合并:顶层浅合并,同名键整个覆盖;唯独 generationConfig
    // 深一层(理由见函数头注释)。provider 级先合,Request::extra_body(模型
    // variant)后合,后者同级再压前者。
    const auto merge_extra = [&](const json& source) {
        if (!source.is_object()) {
            return;
        }
        for (auto it = source.begin(); it != source.end(); ++it) {
            if (it.key() == "generationConfig" && it.value().is_object() &&
                body.contains("generationConfig") && body["generationConfig"].is_object()) {
                for (auto sub = it.value().begin(); sub != it.value().end(); ++sub) {
                    body["generationConfig"][sub.key()] = sub.value();
                }
                continue;
            }
            body[it.key()] = it.value();
        }
    };
    merge_extra(extra_body);
    merge_extra(request.extra_body);

    return body;
}

std::string StreamUrl(const std::string& base_url, const std::string& model) {
    std::string trimmed = model;
    constexpr const char* kModelsPrefix = "models/";
    if (trimmed.rfind(kModelsPrefix, 0) == 0) {
        trimmed = trimmed.substr(std::char_traits<char>::length(kModelsPrefix));
    }
    while (!trimmed.empty() && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    return base_url + "/v1beta/models/" + trimmed + ":streamGenerateContent?alt=sse";
}

}  // namespace lubancode::api::gemini
