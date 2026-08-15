#include "api/chat/request.hpp"

#include <type_traits>
#include <variant>

namespace lubancode::api::chat {

namespace {

using nlohmann::json;

json TextAndImages(const Message& message) {
    json parts = json::array();
    for (const auto& block : message.content) {
        if (const auto* text = std::get_if<TextBlock>(&block)) {
            parts.push_back(json{{"type", "text"}, {"text", text->text}});
        } else if (const auto* image = std::get_if<ImageBlock>(&block)) {
            parts.push_back(json{{"type", "image_url"},
                                 {"image_url", json{{"url", "data:" + image->media_type + ";base64," + image->data}}}});
        }
    }
    return parts;
}

bool HasImage(const Message& message) {
    for (const auto& block : message.content) {
        if (std::holds_alternative<ImageBlock>(block)) {
            return true;
        }
    }
    return false;
}

std::string JoinedText(const Message& message) {
    std::string text;
    for (const auto& block : message.content) {
        if (const auto* part = std::get_if<TextBlock>(&block)) {
            text += part->text;
        }
    }
    return text;
}

}  // namespace

nlohmann::json BuildRequestJson(const Request& request, const nlohmann::json& extra_body,
                                const ChatRequestOptions& options) {
    json body{{"model", request.model},
              {"stream", true},
              {"max_tokens", request.max_tokens}};

    json messages = json::array();
    if (!request.system.empty()) {
        messages.push_back(json{{"role", "system"}, {"content", request.system}});
    }

    for (const auto& message : request.messages) {
        if (message.role == Role::User) {
            std::string text = JoinedText(message);
            const bool has_image = HasImage(message);
            if (!text.empty() || has_image) {
                messages.push_back(json{{"role", "user"},
                                        {"content", has_image ? TextAndImages(message) : json(text)}});
            }
            for (const auto& block : message.content) {
                if (const auto* result = std::get_if<ToolResultBlock>(&block)) {
                    messages.push_back(json{{"role", "tool"},
                                            {"tool_call_id", result->tool_use_id},
                                            {"content", result->content}});
                }
            }
            continue;
        }

        json assistant{{"role", "assistant"}};
        const std::string text = JoinedText(message);
        assistant["content"] = text.empty() ? json(nullptr) : json(text);
        json tool_calls = json::array();
        for (const auto& block : message.content) {
            if (const auto* call = std::get_if<ToolUseBlock>(&block)) {
                tool_calls.push_back(json{{"id", call->id},
                                          {"type", "function"},
                                          {"function", json{{"name", call->name},
                                                            {"arguments", call->input.dump()}}}});
            }
        }
        if (!tool_calls.empty()) {
            assistant["tool_calls"] = std::move(tool_calls);
        }
        messages.push_back(std::move(assistant));
    }
    body["messages"] = std::move(messages);

    // provider 声明了 stream_usage capability 才带 stream_options(有些兼容端
    // 不认这个字段,乱发会被拒);extra_body 在最后浅合并,用户显式写的
    // stream_options 整个压过这里的默认值。
    if (options.stream_usage) {
        body["stream_options"] = json{{"include_usage", true}};
    }

    if (!request.reasoning_effort.empty()) {
        body["reasoning_effort"] = request.reasoning_effort;
    }

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& tool : request.tools) {
            tools.push_back(json{{"type", "function"},
                                 {"function", json{{"name", tool.name},
                                                   {"description", tool.description},
                                                   {"parameters", tool.input_schema}}}});
        }
        body["tools"] = std::move(tools);
    }

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

}  // namespace lubancode::api::chat
