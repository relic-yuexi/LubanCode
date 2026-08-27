#include "api/gemini/request.hpp"

#include <cctype>
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
// MCP 富结果单 P0.6:structuredContent 有值时走原生对象(Gemini 的
// functionResponse 本就吃对象),不绕投影一圈再 parse 回来。
// content_text 由调用方给:默认 result.content;图片被降级时是投影加
// 明降级附注后的文本。
json FunctionResponseBody(const ToolResultBlock& result, const std::string& content_text) {
    if (result.is_error) {
        return json{{"error", content_text}};
    }
    if (result.structured_content.has_value() && result.structured_content->is_object()) {
        return *result.structured_content;
    }
    try {
        const json parsed = json::parse(content_text);
        if (parsed.is_object()) {
            return parsed;
        }
    } catch (const json::exception&) {
        // 不是合法 JSON,走下面的兜底包装。
    }
    return json{{"result", content_text}};
}

// 工具结果图片回喂单:模型代次门。多模态 functionResponse(Gemini 官方
// 文档 docs.cloud.google.com/gemini-enterprise-agent-platform/models/tools/
// function-calling 的 "Multimodal function responses" 一节)明说"For
// Gemini 3 and later models"——functionResponse.parts 里嵌 inlineData 只
// 有 Gemini 3+ 认。模型名里解析 "gemini-<主版本>",3 起真发;认不出或
// 老代次按明降级走(投影文本 + 降级附注),不硬造。
int GeminiMajorVersion(const std::string& model) {
    constexpr const char* kMarker = "gemini-";
    std::size_t pos = model.rfind(kMarker);
    if (pos == std::string::npos) {
        return 0;
    }
    pos += std::char_traits<char>::length(kMarker);
    if (pos >= model.size() || !std::isdigit(static_cast<unsigned char>(model[pos]))) {
        return 0;  // gemini-flash 这类无代次名:按不认得处理,保守降级
    }
    int major = 0;
    while (pos < model.size() && std::isdigit(static_cast<unsigned char>(model[pos]))) {
        major = major * 10 + (model[pos] - '0');
        ++pos;
    }
    return major;
}

// 工具结果里的图片块(重灌过的)翻成 functionResponse.parts 的 inlineData
// 数组。形状出处同上:parts 嵌在 functionResponse 里,每块 inlineData 带
// mimeType 与 base64 data,displayName 给模型一个可指认的名字(协议允许
// response 里用 {"$ref": displayName} 引用;不引用也能处理,这里不添)。
// 支持的图片 MIME:image/png、image/jpeg、image/webp(文档口径;GIF 不
// 在列,不硬发)。没有可随行的图返回空数组。
json ToolResultImageParts(const ToolResultBlock& result) {
    json parts = json::array();
    for (const auto& rich : result.blocks) {
        const auto* image = std::get_if<tools::ImageContent>(&rich);
        if (image == nullptr || image->wire_base64.empty()) {
            continue;
        }
        if (image->mime_type != "image/png" && image->mime_type != "image/jpeg" &&
            image->mime_type != "image/webp") {
            continue;  // Gemini 多模态 functionResponse 只认这三种
        }
        parts.push_back(json{{"inlineData",
                              json{{"displayName", image->artifact.filename},
                                   {"mimeType", image->mime_type},
                                   {"data", image->wire_base64}}}});
    }
    return parts;
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
                    } else if constexpr (std::is_same_v<T, ModelImageBlock>) {
                        // 模型输出图片的替身:引用翻短文本标记,base64 不回传。
                        parts.push_back(json{{"text", ModelImageReplayText(b)}});
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
                        // 工具结果图片回喂:Gemini 3+ 上 functionResponse.parts
                        // 嵌 inlineData 真发;老代次/认不出的模型名按明降级
                        // (投影文本 + 附注),投影短句里本就带着落盘路径。
                        json function_response{{"name", name}};
                        if (GeminiMajorVersion(request.model) >= 3) {
                            json image_parts = ToolResultImageParts(b);
                            if (!image_parts.empty()) {
                                function_response["parts"] = std::move(image_parts);
                            }
                            function_response["response"] = FunctionResponseBody(b, b.content);
                        } else {
                            const std::string content = b.content + ToolResultImageDegradedNote(b);
                            function_response["response"] = FunctionResponseBody(b, content);
                        }
                        contents.push_back(json{{"role", "user"},
                                                {"parts", json::array(
                                                              {json{{"functionResponse",
                                                                     std::move(function_response)}}})}});
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
    // 推理档案决定写 thinkingLevel 还是 thinkingBudget；none 关(minimal 在
    // 目录声明成档位的 Gemini 3 系上是一档真实的 thinkingLevel,不当关)。
    // P1 方言对 gemini 家只做账面(verified/delta/replay):level 与 budget
    // 的选择仍由 wireDialect 走向 + 模型档案决定——两键并发服务端吃 400,
    // 方言不会同时把两只键点亮,选择逻辑与改前一致。
    // 巡检单 P2:目录明说不吃推理的模型(纯生成类)整个 thinkingConfig
    // 不写——includeThoughts 也不发。
    if (!request.reasoning_effort.empty() && !request.reasoning.declined) {
        const bool off = ReasoningEffortIsOff(request.reasoning_effort, request.reasoning);
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
