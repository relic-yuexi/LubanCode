#include "api/anthropic/events.hpp"

#include <cctype>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace lubancode::api::anthropic {

namespace {

using nlohmann::json;

std::optional<StreamEvent> HandleMessageStart(const json& data) {
    MessageStart event;
    if (auto it = data.find("message"); it != data.end() && it->is_object()) {
        event.id = it->value("id", "");
        event.model = it->value("model", "");
    }
    return event;
}

std::optional<StreamEvent> HandleContentBlockStart(const json& data) {
    auto it = data.find("content_block");
    if (it == data.end() || !it->is_object()) {
        return std::nullopt;
    }
    const std::string type = it->value("type", "");
    if (type != "tool_use") {
        // text / thinking 块的起始不单独发事件,文本内容靠后续
        // content_block_delta 里的 text_delta 一段段拼出来。
        return std::nullopt;
    }
    ToolUseStart event;
    event.index = data.value("index", 0);
    event.id = it->value("id", "");
    event.name = it->value("name", "");
    return event;
}

std::optional<StreamEvent> HandleContentBlockDelta(const json& data) {
    auto it = data.find("delta");
    if (it == data.end() || !it->is_object()) {
        return std::nullopt;
    }
    const std::string type = it->value("type", "");
    if (type == "text_delta") {
        TextDelta event;
        event.text = it->value("text", "");
        return event;
    }
    if (type == "input_json_delta") {
        ToolUseInputDelta event;
        event.index = data.value("index", 0);
        event.partial_json = it->value("partial_json", "");
        return event;
    }
    // thinking_delta:思考正文的一段流式增量。
    if (type == "thinking_delta") {
        ThinkingDelta event;
        event.text = it->value("thinking", "");
        return event;
    }
    // signature_delta:思考块的签名片段,续会话重放历史时必须带。
    if (type == "signature_delta") {
        ThinkingDelta event;
        event.signature = it->value("signature", "");
        return event;
    }
    return std::nullopt;
}

std::optional<StreamEvent> HandleContentBlockStop(const json& data) {
    ContentBlockDone event;
    event.index = data.value("index", 0);
    return event;
}

std::optional<StreamEvent> HandleMessageDelta(const json& data) {
    // message_delta 里已经带了完整的 stop_reason 和 4 个字段的 usage,
    // 这里直接凑出 MessageDone;随后的 message_stop 只是个哑的收尾标记,
    // 不需要再发一次。
    MessageDone event;
    if (auto it = data.find("delta"); it != data.end() && it->is_object()) {
        event.stop_reason = it->value("stop_reason", "");
    }
    if (auto it = data.find("usage"); it != data.end() && it->is_object()) {
        // 帧里真有 usage 对象才算 provider 明报(Token 账本单 A0):明报全零
        // 也是真,没这对象才是没报。
        event.usage_reported = true;
        event.usage.input_tokens = it->value("input_tokens", static_cast<std::int64_t>(0));
        event.usage.output_tokens = it->value("output_tokens", static_cast<std::int64_t>(0));
        // 实测 MiniMax 在 message_delta 的顶层 usage 里回这两个字段;没有就是 0。
        event.usage.cache_read_tokens = it->value("cache_read_input_tokens", static_cast<std::int64_t>(0));
        event.usage.cache_creation_tokens = it->value("cache_creation_input_tokens", static_cast<std::int64_t>(0));
    }
    return event;
}

std::optional<StreamEvent> HandleError(const json& data) {
    StreamError event;
    if (auto it = data.find("error"); it != data.end() && it->is_object()) {
        event.message = it->value("message", "未知错误");
    } else {
        event.message = "未知错误";
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

    if (type == "message_start") {
        return HandleMessageStart(data);
    }
    if (type == "content_block_start") {
        return HandleContentBlockStart(data);
    }
    if (type == "content_block_delta") {
        return HandleContentBlockDelta(data);
    }
    if (type == "content_block_stop") {
        return HandleContentBlockStop(data);
    }
    if (type == "message_delta") {
        return HandleMessageDelta(data);
    }
    if (type == "message_stop") {
        return std::nullopt;  // 已经在 message_delta 里发过 MessageDone 了
    }
    if (type == "ping") {
        return std::nullopt;  // 心跳,忽略
    }
    if (type == "error") {
        return HandleError(data);
    }

    // 没见过的事件类型:静默跳过,别崩。
    return std::nullopt;
} catch (const json::exception&) {
    // 字段存在但类型不对时,.value()/.get() 抛的是 type_error(不是
    // parse_error)——这里跑在 libcurl 的 WriteCallback 栈上,异常穿透出去
    // 就是未定义行为/进程崩溃。坏帧一律当没看见;整条流缺了 MessageDone
    // 的兜底在 client 层(send_stream 末尾检查)。
    return std::nullopt;
}

std::vector<StreamEvent> EventParser::Consume(const SseFrame& frame) {
    auto event = parse_event(frame);
    if (!event.has_value()) {
        return {};
    }
    return ConsumeParsed(std::move(*event));
}

std::vector<StreamEvent> EventParser::ConsumeParsed(StreamEvent event) {
    if (!recover_tagged_thinking_) {
        return {std::move(event)};
    }
    if (auto* text = std::get_if<TextDelta>(&event); text != nullptr) {
        return ConsumeText(std::move(text->text));
    }

    // 正规 thinking_delta 已经到了，说明端点这轮守协议。其后的正文即使
    // 恰从 `<think>` 开头，也该按正文保留。
    if (std::holds_alternative<ThinkingDelta>(event) && tagged_state_ == TaggedThinkingState::Probe &&
        pending_.empty()) {
        tagged_state_ = TaggedThinkingState::Passthrough;
    }

    std::vector<StreamEvent> out;
    if ((std::holds_alternative<ContentBlockDone>(event) || std::holds_alternative<MessageDone>(event)) &&
        (tagged_state_ == TaggedThinkingState::Probe || tagged_state_ == TaggedThinkingState::Thinking ||
         tagged_state_ == TaggedThinkingState::AwaitingAnswer)) {
        out = CloseOpenProbe();
    }
    out.push_back(std::move(event));
    return out;
}

std::vector<StreamEvent> EventParser::ConsumeText(std::string text) {
    static constexpr std::string_view kOpen = "<think>";
    static constexpr std::string_view kClose = "</think>";

    if (tagged_state_ == TaggedThinkingState::Passthrough ||
        tagged_state_ == TaggedThinkingState::AfterThinking ||
        tagged_state_ == TaggedThinkingState::Failed) {
        return {TextDelta{std::move(text)}};
    }

    std::vector<StreamEvent> out;
    pending_ += text;
    if (tagged_state_ == TaggedThinkingState::AwaitingAnswer) {
        const std::size_t close_at = pending_.find(kClose);
        const std::size_t after_close = close_at + kClose.size();
        const std::string_view suffix(pending_.data() + after_close, pending_.size() - after_close);
        std::size_t first_text = 0;
        std::size_t newlines = 0;
        while (first_text < suffix.size() &&
               std::isspace(static_cast<unsigned char>(suffix[first_text])) != 0) {
            if (suffix[first_text] == '\n') ++newlines;
            ++first_text;
        }
        if (first_text == suffix.size()) {
            return out;  // 眼下只有分隔空白，等下一枚 delta 再定。
        }
        if (newlines < 2) {
            tagged_state_ = TaggedThinkingState::Passthrough;
            out.push_back(TextDelta{std::exchange(pending_, {})});
            return out;
        }
        recovered_tagged_thinking_ = true;
        out.push_back(TextDelta{pending_.substr(after_close)});
        pending_.clear();
        tagged_state_ = TaggedThinkingState::AfterThinking;
        return out;
    }

    if (tagged_state_ == TaggedThinkingState::Probe) {
        if (kOpen.starts_with(pending_)) {
            return out;  // 开标签可能横跨多个 text_delta，先扣住。
        }
        if (!pending_.starts_with(kOpen)) {
            tagged_state_ = TaggedThinkingState::Passthrough;
            out.push_back(TextDelta{std::exchange(pending_, {})});
            return out;
        }
        pending_.erase(0, kOpen.size());
        tagged_state_ = TaggedThinkingState::Thinking;
    }

    const std::size_t close_at = pending_.find(kClose);
    if (close_at != std::string::npos) {
        pending_.insert(0, kOpen);
        tagged_state_ = TaggedThinkingState::AwaitingAnswer;
        return ConsumeText({});
    }

    // 原始标签里的字没有可信 signature，不能伪装成 ThinkingBlock 入历史：
    // 下一轮按 Anthropic wire 重放会拿空签名撞 400。整段扣到闭标签再丢，
    // 只放行后面的真正正文。
    return out;
}

std::vector<StreamEvent> EventParser::CloseOpenProbe() {
    std::vector<StreamEvent> out;
    if (tagged_state_ == TaggedThinkingState::Probe) {
        if (!pending_.empty()) {
            out.push_back(TextDelta{std::exchange(pending_, {})});
        }
        tagged_state_ = TaggedThinkingState::Passthrough;
    } else if (tagged_state_ == TaggedThinkingState::Thinking) {
        pending_.clear();
        tagged_state_ = TaggedThinkingState::Failed;
        out.push_back(StreamError{"Messages 兼容端返回了未闭合的 <think> 标签，已拦下这段异常输出"});
    } else if (tagged_state_ == TaggedThinkingState::AwaitingAnswer) {
        // 没等到“空行 + 正文”，证据不足，按用户真要输出的 XML 原样放行。
        out.push_back(TextDelta{std::exchange(pending_, {})});
        tagged_state_ = TaggedThinkingState::Passthrough;
    }
    return out;
}

std::vector<StreamEvent> EventParser::Finish() {
    if (!recover_tagged_thinking_ ||
        (tagged_state_ != TaggedThinkingState::Probe && tagged_state_ != TaggedThinkingState::Thinking &&
         tagged_state_ != TaggedThinkingState::AwaitingAnswer)) {
        return {};
    }
    return CloseOpenProbe();
}

}  // namespace lubancode::api::anthropic
