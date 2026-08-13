#include "api/responses/request.hpp"

#include <type_traits>
#include <variant>

namespace lubancode::api::responses {

namespace {

using nlohmann::json;

std::string RoleToString(Role role) {
    return role == Role::User ? "user" : "assistant";
}

// 没有图片的旧消息继续沿用逐块转 item 的写法，避免把既有请求形状悄悄
// 合并。含图片时另走下面的成组分支，input_text 和 input_image 才能同框。
json ContentBlockToItem(const ContentBlock& block, Role role) {
    return std::visit(
        [role](const auto& b) -> json {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, TextBlock>) {
                const char* text_type = role == Role::User ? "input_text" : "output_text";
                return json{{"type", "message"},
                            {"role", RoleToString(role)},
                            {"content", json::array({json{{"type", text_type}, {"text", b.text}}})}};
            } else if constexpr (std::is_same_v<T, ImageBlock>) {
                return json{{"type", "message"},
                            {"role", RoleToString(role)},
                            {"content", json::array({json{{"type", "input_image"},
                                                       {"image_url", "data:" + b.media_type + ";base64," + b.data}}})}};
            } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                return json{{"type", "function_call"},
                            {"call_id", b.id},
                            {"name", b.name},
                            {"arguments", b.input.dump()}};
            } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                // responses wire 的 reasoning 是一次性的,不参与续会话重放。
                // 这里给一个 reasoning 占位,调用方(BuildRequestJson)会跳过。
                return json{{"type", "__thinking_skip__"}};
            } else {
                return json{{"type", "function_call_output"},
                            {"call_id", b.tool_use_id},
                            {"output", b.content}};
            }
        },
        block);
}

}  // namespace

nlohmann::json BuildRequestJson(const Request& request, bool native_web_search, const json& extra_body) {
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
        bool has_image = false;
        for (const auto& block : message.content) {
            if (std::holds_alternative<ImageBlock>(block)) {
                has_image = true;
                break;
            }
        }
        if (!has_image) {
            for (const auto& block : message.content) {
                if (std::holds_alternative<ThinkingBlock>(block)) {
                    continue;  // 思考块不回传:responses wire 的 reasoning 是一次性的
                }
                input.push_back(ContentBlockToItem(block, message.role));
            }
            continue;
        }

        // Responses 的 input_image 必须跟 input_text 一样塞进 message.content。
        // 遇到工具块才把已经攒着的普通内容吐成一条 message，守住块顺序。
        json content = json::array();
        const auto flush_content = [&] {
            if (content.empty()) {
                return;
            }
            input.push_back(json{{"type", "message"}, {"role", RoleToString(message.role)}, {"content", content}});
            content = json::array();
        };
        for (const auto& block : message.content) {
            std::visit(
                [&](const auto& b) {
                    using T = std::decay_t<decltype(b)>;
                    if constexpr (std::is_same_v<T, TextBlock>) {
                        content.push_back(json{{"type", message.role == Role::User ? "input_text" : "output_text"},
                                               {"text", b.text}});
                    } else if constexpr (std::is_same_v<T, ImageBlock>) {
                        content.push_back(json{{"type", "input_image"},
                                               {"image_url", "data:" + b.media_type + ";base64," + b.data}});
                    } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                        // 思考块不回传:responses wire 的 reasoning 是一次性的
                    } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                        flush_content();
                        input.push_back(ContentBlockToItem(block, message.role));
                    } else {
                        flush_content();
                        input.push_back(ContentBlockToItem(block, message.role));
                    }
                },
                block);
        }
        flush_content();
    }
    body["input"] = input;

    // native_web_search 是服务端原生能力声明,跟 request.tools(本地函数
    // 工具)是两码事——就算本地工具表是空的,只要开关开着也要能声明,所以
    // 这里不能再用 "!request.tools.empty()" 当建不建 tools 字段的唯一门槛。
    if (!request.tools.empty() || native_web_search) {
        json tools = json::array();
        for (const auto& tool : request.tools) {
            tools.push_back(json{
                {"type", "function"},
                {"name", tool.name},
                {"description", tool.description},
                {"parameters", tool.input_schema},
            });
        }
        if (native_web_search) {
            tools.push_back(json{{"type", "web_search"}});
        }
        body["tools"] = tools;
    }

    // extra_body 永远在最后合并(见 client.hpp 里 BuildRequestJson 的注释):
    // 键冲突时整个覆盖前面算出来的值,只做顶层浅合并,不做深合并。
    if (extra_body.is_object()) {
        for (auto it = extra_body.begin(); it != extra_body.end(); ++it) {
            body[it.key()] = it.value();
        }
    }
    if (request.extra_body.is_object()) {
        for (auto it = request.extra_body.begin(); it != request.extra_body.end(); ++it) {
            body[it.key()] = it.value();
        }
    }

    return body;
}

}  // namespace lubancode::api::responses
