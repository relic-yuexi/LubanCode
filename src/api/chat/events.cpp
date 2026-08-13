#include "api/chat/events.hpp"

#include <nlohmann/json.hpp>

namespace lubancode::api::chat {

namespace {

std::string StopReason(const std::string& reason, bool has_tools) {
    if (reason == "length") {
        return "max_tokens";
    }
    if (reason == "tool_calls" || reason == "function_call" || has_tools) {
        return "tool_use";
    }
    return "end_turn";
}

}  // namespace

std::vector<StreamEvent> EventParser::Consume(const SseFrame& frame) try {
    if (frame.data == "[DONE]") {
        return Finish();
    }

    const nlohmann::json data = nlohmann::json::parse(frame.data);
    if (!data.is_object()) {
        return {};
    }

    if (auto error = data.find("error"); error != data.end() && error->is_object()) {
        return {StreamError{error->value("message", "未知错误")}};
    }

    std::vector<StreamEvent> events;
    if (!started_) {
        const std::string id = data.value("id", "");
        const std::string model = data.value("model", "");
        if (!id.empty() || !model.empty()) {
            started_ = true;
            events.push_back(MessageStart{id, model});
        }
    }

    if (auto usage = data.find("usage"); usage != data.end() && usage->is_object()) {
        usage_.input_tokens = usage->value("prompt_tokens", static_cast<std::int64_t>(0));
        usage_.output_tokens = usage->value("completion_tokens", static_cast<std::int64_t>(0));
        if (auto details = usage->find("prompt_tokens_details");
            details != usage->end() && details->is_object()) {
            usage_.cache_read_tokens = details->value("cached_tokens", static_cast<std::int64_t>(0));
        }
    }

    auto choices = data.find("choices");
    if (choices == data.end() || !choices->is_array()) {
        return events;
    }
    for (const auto& choice : *choices) {
        if (!choice.is_object()) {
            continue;
        }
        saw_payload_ = true;
        if (auto finish = choice.find("finish_reason"); finish != choice.end() && finish->is_string()) {
            finish_reason_ = finish->get<std::string>();
        }
        auto delta = choice.find("delta");
        if (delta == choice.end() || !delta->is_object()) {
            continue;
        }
        if (auto content = delta->find("content"); content != delta->end() && content->is_string()) {
            const std::string text = content->get<std::string>();
            if (!text.empty()) {
                events.push_back(TextDelta{text});
            }
        }
        // reasoning_content(DeepSeek 等模型的思考过程):流式立即吐,不攒。
        if (auto reasoning = delta->find("reasoning_content"); reasoning != delta->end() && reasoning->is_string()) {
            const std::string text = reasoning->get<std::string>();
            if (!text.empty()) {
                events.push_back(ThinkingDelta{text, ""});
            }
        }
        auto calls = delta->find("tool_calls");
        if (calls == delta->end() || !calls->is_array()) {
            continue;
        }
        for (const auto& call : *calls) {
            if (!call.is_object()) {
                continue;
            }
            const int index = call.value("index", 0);
            ToolCall& accumulated = tool_calls_[index];
            if (auto id = call.find("id"); id != call.end() && id->is_string()) {
                accumulated.id += id->get<std::string>();
            }
            if (auto function = call.find("function"); function != call.end() && function->is_object()) {
                if (auto name = function->find("name"); name != function->end() && name->is_string()) {
                    accumulated.name += name->get<std::string>();
                }
                if (auto args = function->find("arguments"); args != function->end() && args->is_string()) {
                    accumulated.arguments += args->get<std::string>();
                }
            }
        }
    }
    return events;
} catch (const nlohmann::json::exception&) {
    return {};
}

std::vector<StreamEvent> EventParser::Finish() {
    if (finished_) {
        return {};
    }
    finished_ = true;

    std::vector<StreamEvent> events;
    if (!tool_calls_.empty()) {
        events.push_back(ContentBlockDone{0});
        for (const auto& [index, call] : tool_calls_) {
            const std::string id = call.id.empty() ? "chat_tool_" + std::to_string(index) : call.id;
            events.push_back(ToolUseStart{index, id, call.name});
            events.push_back(ToolUseInputDelta{index, call.arguments});
            events.push_back(ContentBlockDone{index});
        }
    }
    if (saw_payload_ || started_ || !tool_calls_.empty()) {
        events.push_back(MessageDone{StopReason(finish_reason_, !tool_calls_.empty()), usage_});
    }
    return events;
}

}  // namespace lubancode::api::chat
