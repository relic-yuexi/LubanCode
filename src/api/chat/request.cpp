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

// 消息里全部思考正文的原字节拼接(块序不动,不加标签不摘要)。
std::string JoinedThinking(const Message& message) {
    std::string text;
    for (const auto& block : message.content) {
        if (const auto* part = std::get_if<ThinkingBlock>(&block)) {
            text += part->text;
        }
    }
    return text;
}

bool HasToolUse(const Message& message) {
    for (const auto& block : message.content) {
        if (std::holds_alternative<ToolUseBlock>(block)) {
            return true;
        }
    }
    return false;
}

// 一条"真正的用户输入"消息:role 是 User 且内容里有 Text/Image——区别于
// 同样顶着 User 角色、全是 ToolResultBlock 的"把工具结果喂回去"中间消息。
// (agent/context.cpp 等处的同名语义,这里独立一份,语义钉死在注释里。)
bool IsUserTurnStart(const Message& message) {
    if (message.role != Role::User) {
        return false;
    }
    for (const auto& block : message.content) {
        if (std::holds_alternative<TextBlock>(block) || std::holds_alternative<ImageBlock>(block)) {
            return true;
        }
    }
    return false;
}

// user-to-user 交互段划分 + 工具段标记:segment_has_tool_use[i] 为真表示
// 第 i 条消息所在的交互段(从一条真 user 输入到下一条真 user 输入之前)
// 里有任何 assistant 消息发起过工具调用。reasoning 回传(tool_episode
// 策略)只认这个标记——单条消息有没有 ToolUseBlock 不够,得看整段交互
// 是否启用了工具(DeepSeek 规矩:走过工具的交互段,后续请求须完整回传
// 相关 reasoning_content,到下一条 user 轮仍保留)。
std::vector<bool> SegmentToolUseFlags(const std::vector<Message>& messages) {
    std::vector<bool> flags(messages.size(), false);
    bool segment_has_tool = false;
    std::size_t segment_begin = 0;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i > segment_begin && IsUserTurnStart(messages[i])) {
            // 新一段开始:给刚结束的那段落标记。
            for (std::size_t j = segment_begin; j < i; ++j) {
                flags[j] = segment_has_tool;
            }
            segment_begin = i;
            segment_has_tool = false;
        }
        if (messages[i].role == Role::Assistant && HasToolUse(messages[i])) {
            segment_has_tool = true;
        }
    }
    for (std::size_t j = segment_begin; j < messages.size(); ++j) {
        flags[j] = segment_has_tool;
    }
    return flags;
}

}  // namespace

nlohmann::json BuildRequestJson(const Request& request, const nlohmann::json& extra_body,
                                const ChatRequestOptions& options) {
    json body{{"model", request.model}, {"stream", true}};
    // max_tokens 可省略(chat 协议):unset 就整个不带字段,交服务端/模型
    // 默认——vLLM 这类端的默认上限远大于旧版写死的 4096,reasoning 模型
    // 思考不至于一步撞墙(规格根因一)。显式声明了才落键。
    if (request.max_tokens.has_value()) {
        body["max_tokens"] = *request.max_tokens;
    }

    json messages = json::array();
    if (!request.system.empty()) {
        messages.push_back(json{{"role", "system"}, {"content", request.system}});
    }

    // tool_episode 策略的段标记(never 策略不用,不算)。
    const std::vector<bool> segment_tool_use =
        options.reasoning_replay == ReasoningReplayPolicy::ToolEpisode
            ? SegmentToolUseFlags(request.messages)
            : std::vector<bool>{};

    for (std::size_t message_index = 0; message_index < request.messages.size(); ++message_index) {
        const auto& message = request.messages[message_index];
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
        // reasoning 回传(tool_episode):这段交互走了工具,段内 assistant 的
        // 思考按原字节回传——字段名按 provider 声明走(默认
        // reasoning_content,DeepSeek 协议;vLLM/Qwen 这类端声明成
        // reasoning),一条消息只写一份(多枚 tool call 也不拆不重),不混进
        // content。纯对话段照旧略过。
        if (options.reasoning_replay == ReasoningReplayPolicy::ToolEpisode &&
            message_index < segment_tool_use.size() && segment_tool_use[message_index]) {
            const std::string reasoning = JoinedThinking(message);
            if (!reasoning.empty()) {
                const std::string field =
                    options.reasoning_replay_field.empty() ? std::string("reasoning_content")
                                                           : options.reasoning_replay_field;
                assistant[field] = reasoning;
            }
        }
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

    if (!request.reasoning_effort.empty() &&
        (request.reasoning.empty() || request.reasoning.supports_effort)) {
        // 参数名按 provider 声明走(默认 reasoning_effort);空档位仍然整个
        // 缺席字段——"不填"就是真的不发,不偷偷塞默认档。
        const std::string param = options.reasoning_param.empty() ? std::string("reasoning_effort")
                                                                  : options.reasoning_param;
        body[param] = request.reasoning_effort;
    }
    if (!request.reasoning_effort.empty() && request.reasoning.supports_toggle) {
        body["thinking"] = json{{"type", ReasoningEffortIsOff(request.reasoning_effort)
                                             ? "disabled" : "enabled"}};
    }

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& tool : request.tools) {
            tools.push_back(json{{"type", "function"},
                                 {"function", json{{"name", tool.name},
                                                   {"description", tool.description},
                                                   {"parameters", ToolSchemaForWire(tool.input_schema)}}}});
        }
        body["tools"] = std::move(tools);
    }

    // extra_body 在最后浅合并:provider 级先、Request::extra_body(模型
    // variant)后,同名顶层键后者压前者(共用件 api::MergeExtraBody)。
    MergeExtraBody(body, extra_body);
    MergeExtraBody(body, request.extra_body);
    return body;
}

}  // namespace lubancode::api::chat
