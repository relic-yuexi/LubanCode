#include "api/responses/request.hpp"

#include <type_traits>
#include <variant>

namespace lubancode::api::responses {

namespace {

using nlohmann::json;

std::string RoleToString(Role role) {
    return role == Role::User ? "user" : "assistant";
}

// 一个内容块翻译成一个 input item。text 块变成一条 message item(user 用
// input_text,assistant 用 output_text);assistant 发起的 tool_use 变成
// function_call item;user 侧回传的 tool_result 变成 function_call_output
// item。call_id 原样透传 —— Anthropic 的 toolu_xxx、Responses 的 call_xxx
// 中立层都只当字符串存,不用区分。
json ContentBlockToItem(const ContentBlock& block, Role role) {
    return std::visit(
        [role](const auto& b) -> json {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, TextBlock>) {
                const char* text_type = role == Role::User ? "input_text" : "output_text";
                return json{
                    {"type", "message"},
                    {"role", RoleToString(role)},
                    {"content", json::array({json{{"type", text_type}, {"text", b.text}}})},
                };
            } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                return json{
                    {"type", "function_call"},
                    {"call_id", b.id},
                    {"name", b.name},
                    {"arguments", b.input.dump()},
                };
            } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                return json{
                    {"type", "function_call_output"},
                    {"call_id", b.tool_use_id},
                    {"output", b.content},
                };
            }
        },
        block);
}

}  // namespace

nlohmann::json BuildRequestJson(const Request& request) {
    json body;
    body["model"] = request.model;
    body["max_output_tokens"] = request.max_tokens;
    body["stream"] = true;
    body["store"] = false;  // 无状态:历史全靠自己带,跟 Anthropic 后端行为一致

    if (!request.system.empty()) {
        body["instructions"] = request.system;
    }

    // M6.6:推理强度。实测(MiniMax-M3 真实 responses 端点)none/low/medium/
    // high 四档都能用,HTTP 200、reasoning_tokens 随档位递增,没有任何一档
    // 报 400——所以这里不做档位限制/回退,原样透传配置里写的那个字符串。
    if (!request.reasoning_effort.empty()) {
        body["reasoning"] = json{{"effort", request.reasoning_effort}};
    }

    json input = json::array();
    for (const auto& message : request.messages) {
        for (const auto& block : message.content) {
            input.push_back(ContentBlockToItem(block, message.role));
        }
    }
    body["input"] = input;

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& tool : request.tools) {
            tools.push_back(json{
                {"type", "function"},
                {"name", tool.name},
                {"description", tool.description},
                {"parameters", tool.input_schema},
            });
        }
        body["tools"] = tools;
    }

    return body;
}

}  // namespace lubancode::api::responses
