#include "api/parsed_json.hpp"
#include "api/anthropic/events.hpp"
#include "api/usage_event_projection.hpp"
#include "api/usage_lexical.hpp"

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

std::optional<StreamEvent> HandleContentBlockStart(const json& data, bool parse_server_tool_search) {
    auto it = data.find("content_block");
    if (it == data.end() || !it->is_object()) {
        return std::nullopt;
    }
    const std::string type = it->value("type", "");
    if (type == "server_tool_use") {
        // 服务端执行的工具调用(动态工具 P3)。只在开了 server_tool_search 门
        // 且名字是我们声明的那两枚搜索工具时解析——web_search 等其他 server
        // tool 的块照旧跳过,旧路行为一字不动。入参照样走 input_json_delta
        // 增量,assembler 按"is_server"累积成 ServerToolUseBlock。
        if (!parse_server_tool_search) {
            return std::nullopt;
        }
        const std::string name = it->value("name", "");
        if (name != "tool_search_tool_regex" && name != "tool_search_tool_bm25") {
            return std::nullopt;
        }
        ServerToolUseStart event;
        event.index = data.value("index", 0);
        event.id = it->value("id", "");
        event.name = name;
        return event;
    }
    if (type == "tool_search_tool_result") {
        // 搜索结果块:官方流样例里整只(含嵌套 content)随 content_block_start
        // 一次到齐,没有增量。无损保存 tool_references / error,不压文本。
        if (!parse_server_tool_search) {
            return std::nullopt;
        }
        ServerToolResult event;
        event.index = data.value("index", 0);
        event.tool_use_id = it->value("tool_use_id", "");
        if (auto content = it->find("content"); content != it->end() && content->is_object()) {
            event.content = *content;
        }
        return event;
    }
    if (type == "redacted_thinking") {
        // 加密思考块(轨迹 v3 差距清单 §8.2 第 6 条、单子 §4.42):整只
        // (只有不透明 data)随 content_block_start 一次到齐,没有增量、
        // 没有 signature。无损进历史、下一轮原样回传——不压文本、不解密、
        // 不丢块。与 server_tool_search 无关,不设门:凡协议里真出现就认。
        RedactedThinking event;
        event.data = it->value("data", "");
        return event;
    }
    if (type != "tool_use") {
        // text / thinking 块的起始不单独发事件,文本内容靠后续
        // content_block_delta 里的 text_delta 一段段拼出来。
        return std::nullopt;
    }
    ToolUseStart event;
    event.index = data.value("index", 0);
    event.id = it->value("id", "");
    event.name = it->value("name", "");
    // PTC 的调用方标识(§7.2 "对应 id / caller / error"):wire 给了就带上,
    // 没给是空串——绝大多数请求都没有。
    event.caller = it->value("caller", "");
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
    // message_delta 里已经带了完整的 stop_reason 和 usage,
    // 这里直接凑出 MessageDone;随后的 message_stop 只是个哑的收尾标记,
    // 不需要再发一次。无状态路径只看本帧(缺字段落 0、旗标不置)——跨帧
    // 合并(开头快照 + 后续覆盖)是有状态 EventParser::Consume 的活。
    MessageDone event;
    if (auto it = data.find("delta"); it != data.end() && it->is_object()) {
        event.stop_reason = it->value("stop_reason", "");
    }
    if (const auto* usage=usage_wire::Find(data,{"usage"});usage && usage->is_object()) {
        auto snapshot=usage_wire::Anthropic(*usage);
        if (!snapshot) return StreamError{std::string(snapshot.error()),"usage.material.invalid"};
        usage_wire::Apply(event,*snapshot);
    }

    return event;
}

std::optional<StreamEvent> HandleError(const json& data) {
    StreamError event;
    if (auto it = data.find("error"); it != data.end() && it->is_object()) {
        event.message = it->value("message", "未知错误");
        event.code = it->value("code", it->value("type", std::string()));
    } else {
        event.message = "未知错误";
    }
    return event;
}

}  // namespace

std::optional<StreamEvent> parse_event(const SseFrame& frame, bool parse_server_tool_search) try {
    ParsedJson document;
    const auto& data = document.value();
    try {
        document.Parse(frame.data);
    } catch (const json::parse_error&) {
        // 帧里的数据不是合法 JSON,跳过,不崩。
        return std::nullopt;
    }
    return parse_event_json(data, parse_server_tool_search);
} catch (const json::exception&) {
    // 字段存在但类型不对时,.value()/.get() 抛的是 type_error(不是
    // parse_error)——这里跑在 libcurl 的 WriteCallback 栈上,异常穿透出去
    // 就是未定义行为/进程崩溃。坏帧一律当没看见;整条流缺了 MessageDone
    // 的兜底在 client 层(send_stream 末尾检查)。
    return std::nullopt;
}

// 无状态翻译的共用主体:parse_event(SSE 帧)与 EventParser::Consume(带
// usage 快照的有状态路)各自 parse 一棵 json 树后都走这里,判定逻辑一份。
std::optional<StreamEvent> parse_event_json(const json& data, bool parse_server_tool_search) try {
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
        return HandleContentBlockStart(data, parse_server_tool_search);
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
    // 与 parse_event 同一条兜底:坏帧当没看见,不崩。
    return std::nullopt;
}

void EventParser::ResetUsageState() {
    accounting_=usage_wire::AnthropicAccounting{};
    numeric_delivery_=usage_wire::NumericDeliveryOwner{};
    usage_seen_=false;provider_response_id_.reset();
}

void EventParser::AbsorbUsageObject(const json& usage, const std::vector<usage_wire::facts::RawField>* lexical) {
    usage_seen_=true;
    accounting_.Absorb(usage,lexical,
        [](void* context, const usage_wire::AnthropicAccounting::NumericValues& values) noexcept {
            static_cast<EventParser*>(context)->numeric_delivery_.Own(values);
        },this);
}

std::vector<StreamEvent> EventParser::Consume(const SseFrame& frame) {
    std::vector<StreamEvent> events;
    const usage_wire::LexicalUsage lexical(frame.data, usage_wire::Dialect::Anthropic,
        nullptr, this,
        [](void* context, const usage_wire::SourceNumbers& source, bool message_start, bool message_delta) noexcept {
            auto& parser = *static_cast<EventParser*>(context);
            if (message_start) parser.ResetUsageState();
            if ((message_start || message_delta) && source.is_object())
                parser.numeric_delivery_.Own(parser.accounting_.AbsorbNumbers(source));
        });
    ParsedJson document;
    const auto& data = document.value();
    try { document.Parse(frame.data); }
    catch (const json::exception&) {
        if (lexical.numbers.empty()) return events;
        if (lexical.event_type == "message_start") ResetUsageState();
        AbsorbUsageObject(lexical.NumericObject(), &lexical.numbers);
        if (lexical.response_id) {
            provider_response_id_ = lexical.response_id;
            events.push_back(ProviderResponseIdentity{*lexical.response_id});
        }
        auto partial = accounting_.View();
        if (partial) {
            numeric_delivery_.Own(*partial);
            usage_wire::LexicalUsage::MarkIncomplete(*partial);
            events.push_back(usage_wire::Nonterminal(*partial, provider_response_id_));
        }
        events.push_back(Fail(StreamError{"accounting recovered from an unparseable frame", "usage.frame.incomplete"}));
        return events;
    }
    if (!data.is_object()) return events;
    const auto* type=usage_wire::Find(data,{"type"});
    if (!type || !type->is_string()) return events;
    const auto& name=type->get_ref<const std::string&>();
    const json* usage=nullptr;
    const json* response_identity=nullptr;
    std::optional<std::string_view> id_error;
    if (name=="message_start") {
        ResetUsageState();
        if (const auto* message=usage_wire::Find(data,{"message"});message && message->is_object()) {
            response_identity=usage_wire::Find(*message,{"id"});
            usage=usage_wire::Find(*message,{"usage"});
        }
    } else if (name=="message_delta") {
        usage=usage_wire::Find(data,{"usage"});
    }
    if (usage && usage->is_object()) AbsorbUsageObject(*usage, &lexical.numbers);
    if (name=="message_start") {
        const auto id=usage_wire::ResponseId(response_identity);
        if (id) {
            provider_response_id_=*id;
            if (*id) events.push_back(ProviderResponseIdentity{**id});
        }
        else id_error=id.error();
    }
    std::optional<usage_wire::Snapshot> material;
    if (usage && usage->is_object()) {
        auto snapshot=accounting_.View();
        if (!snapshot) {
            events.push_back(Fail(StreamError{std::string(snapshot.error()),"usage.material.invalid"}));
            return events;
        }
        material=std::move(*snapshot);
        numeric_delivery_.Own(*material);
        if (!lexical.complete || lexical.duplicate) usage_wire::LexicalUsage::MarkIncomplete(*material);
        if (!material->material_error.empty()) {
            events.push_back(usage_wire::Nonterminal(*material, provider_response_id_));
            events.push_back(Fail(StreamError{std::string(material->material_error), "usage.material.invalid"}));
            return events;
        }
        events.push_back(usage_wire::Nonterminal(*material,provider_response_id_));
    }
    if (id_error) {
        events.push_back(Fail(StreamError{std::string(*id_error),"usage.response_id.invalid"}));
        return events;
    }
    if (failed_) return events;  // Keep late facts without translating a success.
    auto event=parse_event_json(data,parse_server_tool_search_);
    if (!event) {
        if (name=="message_delta")
            events.push_back(Fail(StreamError{"message_delta has an invalid payload","model.payload.invalid"}));
        return events;
    }
    if (auto* done=std::get_if<MessageDone>(&*event);done && usage_seen_) {
        if (!material) {
            auto snapshot=accounting_.View();
            if (!snapshot) {
                events.push_back(Fail(StreamError{std::string(snapshot.error()),"usage.material.invalid"}));
                return events;
            }
            material=std::move(*snapshot);
            numeric_delivery_.Own(*material);
        }
        usage_wire::Apply(*done,*material,provider_response_id_);
    }
    for (auto& translated : ConsumeParsed(std::move(*event))) {
        if (std::holds_alternative<StreamError>(translated)) failed_ = true;
        if (failed_ && !std::holds_alternative<StreamError>(translated)) continue;
        events.push_back(std::move(translated));
    }
    return events;
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
    if (failed_) return {};
    if (!recover_tagged_thinking_ ||
        (tagged_state_ != TaggedThinkingState::Probe && tagged_state_ != TaggedThinkingState::Thinking &&
         tagged_state_ != TaggedThinkingState::AwaitingAnswer)) {
        return {};
    }
    auto tail = CloseOpenProbe();
    for (const auto& event : tail) if (std::holds_alternative<StreamError>(event)) failed_ = true;
    return tail;
}

}  // namespace lubancode::api::anthropic
