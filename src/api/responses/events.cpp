#include "api/responses/events.hpp"

#include <nlohmann/json.hpp>

namespace lubancode::api::responses {

namespace {

using nlohmann::json;

std::optional<StreamEvent> HandleOutputTextDelta(const json& data) {
    TextDelta event;
    event.text = data.value("delta", "");
    return event;
}

// response.reasoning_summary_text.delta:推理摘要的流式增量(OpenAI o-series
// / 兼容端的 reasoning 事件)。翻成 ThinkingDelta 让界面即时展示"思考中"。
std::optional<StreamEvent> HandleReasoningDelta(const json& data) {
    ThinkingDelta event;
    event.text = data.value("delta", "");
    if (event.text.empty()) {
        return std::nullopt;
    }
    return event;
}

std::optional<StreamEvent> HandleOutputItemAdded(const json& data) {
    auto it = data.find("item");
    if (it == data.end() || !it->is_object()) {
        return std::nullopt;
    }
    const std::string type = it->value("type", "");
    if (type == "web_search_call") {
        BuiltinToolStart event;
        event.id = it->value("id", "");
        event.name = "web_search";
        if (auto action = it->find("action"); action != it->end() && action->is_object()) {
            event.input = *action;
        }
        return event;
    }
    if (type != "function_call") {
        // message 类型的起始不单独发事件,文本内容靠后续
        // response.output_text.delta 一段段拼出来；reasoning 与尚未接线的
        // 其它内置工具静默跳过。web_search_call 已在上面单独发展示事件。
        return std::nullopt;
    }
    ToolUseStart event;
    event.index = data.value("output_index", 0);
    event.id = it->value("call_id", "");
    event.name = it->value("name", "");
    return event;
}

std::optional<StreamEvent> HandleFunctionCallArgumentsDelta(const json& data) {
    ToolUseInputDelta event;
    event.index = data.value("output_index", 0);
    event.partial_json = data.value("delta", "");
    return event;
}

std::optional<StreamEvent> HandleOutputItemDone(const json& data) {
    auto it = data.find("item");
    if (it == data.end() || !it->is_object()) {
        return std::nullopt;
    }
    const std::string type = it->value("type", "");
    if (type == "web_search_call") {
        BuiltinToolDone event;
        event.id = it->value("id", "");
        event.name = "web_search";
        event.summary = "服务端搜索完成";
        if (auto action = it->find("action"); action != it->end() && action->is_object()) {
            event.input = *action;
            if (auto query = action->find("query"); query != action->end() && query->is_string()) {
                event.summary = "查询: " + query->get<std::string>();
            } else if (auto queries = action->find("queries"); queries != action->end() && queries->is_array()) {
                event.summary = "服务端完成 " + std::to_string(queries->size()) + " 条搜索";
            }
        }
        return event;
    }
    if (type != "message" && type != "function_call") {
        // 没有对应的 ToolUseStart/文本块起始(reasoning、内置工具调用……),
        // 没什么可收尾的,跳过。
        return std::nullopt;
    }
    ContentBlockDone event;
    event.index = data.value("output_index", 0);
    return event;
}

std::optional<StreamEvent> HandleCompleted(const json& data) {
    auto it = data.find("response");
    if (it == data.end() || !it->is_object()) {
        return std::nullopt;
    }
    const json& response = *it;

    MessageDone event;
    const std::string status = response.value("status", "");

    bool has_pending_function_call = false;
    if (auto output_it = response.find("output"); output_it != response.end() && output_it->is_array()) {
        for (const auto& item : *output_it) {
            if (item.is_object() && item.value("type", "") == "function_call") {
                has_pending_function_call = true;
                break;
            }
        }
    }

    if (status == "incomplete") {
        event.stop_reason = "max_tokens";
    } else if (has_pending_function_call) {
        event.stop_reason = "tool_use";
    } else {
        event.stop_reason = "end_turn";
    }

    if (auto usage_it = response.find("usage"); usage_it != response.end() && usage_it->is_object()) {
        // 统一口径(api::Usage 文件头):input_tokens 总数已含 cached_tokens,
        // 摊开成 input=total-cached、cache_read=cached,消费端不再加两遍。
        // responses wire 没有"缓存写入"概念,cache_creation_tokens 恒为 0。
        event.usage.output_tokens = usage_it->value("output_tokens", static_cast<std::int64_t>(0));
        const std::int64_t input_total = usage_it->value("input_tokens", static_cast<std::int64_t>(0));
        std::int64_t cached = 0;
        if (auto details_it = usage_it->find("input_tokens_details");
            details_it != usage_it->end() && details_it->is_object()) {
            cached = details_it->value("cached_tokens", static_cast<std::int64_t>(0));
        }
        event.usage.cache_read_tokens = cached;
        event.usage.input_tokens = input_total > cached ? input_total - cached : 0;
        // reasoning 拆账:output_tokens_details.reasoning_tokens(已含在
        // output_tokens 总数里)。没拆账就是 0(语义见 api::Usage 注释)。
        if (auto out_details = usage_it->find("output_tokens_details");
            out_details != usage_it->end() && out_details->is_object()) {
            event.usage.output_reasoning_tokens =
                out_details->value("reasoning_tokens", static_cast<std::int64_t>(0));
        }
    }

    return event;
}

std::optional<StreamEvent> HandleFailed(const json& data) {
    StreamError event;
    if (auto response_it = data.find("response"); response_it != data.end() && response_it->is_object()) {
        if (auto error_it = response_it->find("error"); error_it != response_it->end() && error_it->is_object()) {
            event.message = error_it->value("message", "未知错误");
            return event;
        }
    }
    event.message = "未知错误";
    return event;
}

std::optional<StreamEvent> HandleError(const json& data) {
    StreamError event;
    if (auto it = data.find("error"); it != data.end() && it->is_object()) {
        event.message = it->value("message", "未知错误");
    } else {
        event.message = data.value("message", "未知错误");
    }
    return event;
}

}  // namespace

std::optional<StreamEvent> parse_event(const SseFrame& frame) try {
    json data;
    try {
        data = json::parse(frame.data);
    } catch (const json::parse_error&) {
        // 帧里的数据不是合法 JSON,跳过,不崩。
        return std::nullopt;
    }

    if (!data.is_object()) {
        return std::nullopt;
    }
    auto type_it = data.find("type");
    if (type_it == data.end() || !type_it->is_string()) {
        return std::nullopt;
    }
    const std::string type = type_it->get<std::string>();

    if (type == "response.output_text.delta") {
        return HandleOutputTextDelta(data);
    }
    if (type == "response.reasoning_summary_text.delta") {
        return HandleReasoningDelta(data);
    }
    if (type == "response.output_item.added") {
        return HandleOutputItemAdded(data);
    }
    if (type == "response.function_call_arguments.delta") {
        return HandleFunctionCallArgumentsDelta(data);
    }
    if (type == "response.output_item.done") {
        return HandleOutputItemDone(data);
    }
    if (type == "response.completed") {
        return HandleCompleted(data);
    }
    if (type == "response.failed") {
        return HandleFailed(data);
    }
    if (type == "error") {
        return HandleError(data);
    }

    // 没见过的、或者语义上不需要单独发事件的类型(response.created、
    // response.in_progress、response.content_part.*、response.output_text.done、
    // response.function_call_arguments.done、reasoning 相关、其它内置工具/MCP
    // 相关……):静默跳过,别崩。
    return std::nullopt;
} catch (const json::exception&) {
    // 字段存在但类型不对时,.value()/.get() 抛的是 type_error(不是
    // parse_error)——这里跑在 libcurl 的 WriteCallback 栈上,异常穿透出去
    // 就是未定义行为/进程崩溃。坏帧一律当没看见;整条流缺了 MessageDone
    // 的兜底在 client 层(send_stream 末尾检查)。
    return std::nullopt;
}

}  // namespace lubancode::api::responses
