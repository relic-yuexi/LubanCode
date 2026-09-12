#include "api/anthropic/events.hpp"

#include <cctype>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace lubancode::api::anthropic {

namespace {

using nlohmann::json;

// 从 usage 对象读一枚整数字段(缓存用量按 Wire 归一单 C4 的判型规矩):
//   缺席     -> nullopt,不置 seen——"没报"与"报零"从这里分家;
//   在场合法 -> 取值并置 seen(显式零一样置);
//   在场类型错/负数 -> 记人话进 anomaly(矛盾点名,不吞帧不崩),值不取
//   (消费端见 seen=true 而 anomaly 非空便知"报了但读不出/自相矛盾")。
// const json 只走 find/contains,绝不用 operator[] 查不存在键(那是 UB)。
std::optional<std::int64_t> ReadUsageInt(const json& usage, const char* key, bool* seen,
                                         std::string* anomaly) {
    const auto it = usage.find(key);
    if (it == usage.end()) {
        return std::nullopt;
    }
    *seen = true;
    if (!it->is_number_integer()) {
        if (anomaly != nullptr && anomaly->empty()) {
            *anomaly = std::string("usage.") + key + " 类型不是整数";
        }
        return std::nullopt;
    }
    const std::int64_t value = it->get<std::int64_t>();
    if (value < 0 && anomaly != nullptr && anomaly->empty()) {
        *anomaly = std::string("usage.") + key + " 为负(" + std::to_string(value) + ")";
    }
    return value;
}

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
    if (auto it = data.find("usage"); it != data.end() && it->is_object()) {
        // 帧里真有 usage 对象才算 provider 明报(Token 账本单 A0):明报全零
        // 也是真,没这对象才是没报。实测 MiniMax 在 message_delta 的顶层
        // usage 里回缓存字段;字段在场与否分别置读/写旗标。
        event.usage_reported = true;
        bool input_seen = false;
        std::string anomaly;
        if (auto v = ReadUsageInt(*it, "input_tokens", &input_seen, &anomaly)) {
            event.usage.input_tokens = *v;
        }
        if (auto v = ReadUsageInt(*it, "output_tokens", &input_seen, &anomaly)) {
            event.usage.output_tokens = *v;
        }
        if (auto v = ReadUsageInt(*it, "cache_read_input_tokens", &event.cache_read_reported, &anomaly)) {
            event.usage.cache_read_tokens = *v;
        }
        if (auto v = ReadUsageInt(*it, "cache_creation_input_tokens", &event.cache_creation_reported,
                                  &anomaly)) {
            event.usage.cache_creation_tokens = *v;
        }
        event.usage_anomaly = std::move(anomaly);
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
    json data;
    try {
        data = json::parse(frame.data);
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
    usage_snapshot_ = UsageSnapshot{};
    usage_seen_ = false;
    cache_read_seen_ = false;
    cache_creation_seen_ = false;
    usage_anomaly_.clear();
}

void EventParser::AbsorbUsageObject(const json& usage) {
    // 字段级吸收(C1):出现的字段覆盖快照(显式零一样覆盖),缺席的保留
    // 旧值——绝不相加(官方 output_tokens 本就是累计值)。矛盾账(类型
    // 错/负数)只记首条,不刷屏。
    usage_seen_ = true;
    bool dummy_seen = false;
    std::string anomaly;
    if (auto v = ReadUsageInt(usage, "input_tokens", &dummy_seen, &anomaly)) {
        usage_snapshot_.input_tokens = *v;
    }
    if (auto v = ReadUsageInt(usage, "output_tokens", &dummy_seen, &anomaly)) {
        usage_snapshot_.output_tokens = *v;
    }
    if (auto v = ReadUsageInt(usage, "cache_read_input_tokens", &cache_read_seen_, &anomaly)) {
        usage_snapshot_.cache_read = *v;
    }
    if (auto v = ReadUsageInt(usage, "cache_creation_input_tokens", &cache_creation_seen_, &anomaly)) {
        usage_snapshot_.cache_creation = *v;
    }
    if (usage_anomaly_.empty()) {
        usage_anomaly_ = std::move(anomaly);
    }
}

std::vector<StreamEvent> EventParser::Consume(const SseFrame& frame) try {
    // C1:先在 json 层吸收 usage 快照(message_start/message_delta 两类帧),
    // 再走无状态翻译——同一棵树只 parse 一遍。吸收只认"帧里真有 usage
    // 对象"的路;翻译结果里的 MessageDone 出口换成合并账。
    json data;
    try {
        data = json::parse(frame.data);
    } catch (const json::parse_error&) {
        return {};
    }
    if (data.is_object()) {
        const auto type_it = data.find("type");
        if (type_it != data.end() && type_it->is_string()) {
            const std::string type = type_it->get<std::string>();
            if (type == "message_start") {
                // 新响应开始:先清旧账,绝不串上一条流的数字(parser 复用、
                // 连发两条流的测试场景都靠这一下)。
                ResetUsageState();
                if (auto msg = data.find("message"); msg != data.end() && msg->is_object()) {
                    if (auto u = msg->find("usage"); u != msg->end() && u->is_object()) {
                        AbsorbUsageObject(*u);
                    }
                }
            } else if (type == "message_delta") {
                if (auto u = data.find("usage"); u != data.end() && u->is_object()) {
                    AbsorbUsageObject(*u);
                }
            }
        }
    }

    auto event = parse_event_json(data, parse_server_tool_search_);
    if (!event.has_value()) {
        return {};
    }
    if (auto* done = std::get_if<MessageDone>(&*event); done != nullptr) {
        // 出口换合并账:快照里出现过的字段覆盖帧内缺省值(帧内本来就有
        // 的,吸收时已被本帧值覆盖,等价);旗标带全流的看见账。半截流
        // (没有 message_delta)不会走到这里,MessageDone 不发——未完成的
        // 响应不伪装完整账,取消/错误路径由上层按"没收到终帧"收口。
        if (usage_snapshot_.input_tokens.has_value()) {
            done->usage.input_tokens = *usage_snapshot_.input_tokens;
        }
        if (usage_snapshot_.output_tokens.has_value()) {
            done->usage.output_tokens = *usage_snapshot_.output_tokens;
        }
        if (usage_snapshot_.cache_read.has_value()) {
            done->usage.cache_read_tokens = *usage_snapshot_.cache_read;
        }
        if (usage_snapshot_.cache_creation.has_value()) {
            done->usage.cache_creation_tokens = *usage_snapshot_.cache_creation;
        }
        done->usage_reported = usage_seen_;
        done->cache_read_reported = cache_read_seen_;
        done->cache_creation_reported = cache_creation_seen_;
        done->usage_anomaly = usage_anomaly_;
    }
    return ConsumeParsed(std::move(*event));
} catch (const json::exception&) {
    // 坏帧当没看见:与 parse_event 同一条兜底,不崩。
    return {};
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
