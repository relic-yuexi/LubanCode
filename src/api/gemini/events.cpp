#include "api/gemini/events.hpp"

#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::api::gemini {

namespace {

using nlohmann::json;

// finishReason -> 中立 stop_reason。MAX_TOKENS 是撞了输出上限;带着工具
// 调用收场的(STOP 或别的什么)按工具轮算;其余(STOP/SAFETY/RECITATION/
// PROHIBITED_CONTENT/…)一律 end_turn——Gemini 的截断族原因没有对应的
// 中立语义,硬翻只会让上层误判重试。
std::string StopReason(const std::string& reason, bool has_calls) {
    if (reason == "MAX_TOKENS") {
        return "max_tokens";
    }
    if (has_calls) {
        return "tool_use";
    }
    return "end_turn";
}

// usageMetadata -> 统一口径(api::Usage 文件头注释)。Gemini 的账本:
//   promptTokenCount        输入总数(官方手册:cachedContent 命中时它仍报
//                           完整有效的 prompt 大小,即已含缓存部分)
//   cachedContentTokenCount 从隐式上下文缓存命中读走的输入
//   candidatesTokenCount    生成的候选正文
//   thoughtsTokenCount      思考输出
//   totalTokenCount         总数
// 摊开:input = prompt - cached,cache_read = cached(服务端没报 cached
// 字段就是 0,不许拿 0 冒充"未命中")。
//
// 思考账的两代口径(模型怪癖矩阵单查实):2.5 时代 Gemini API 的
// candidatesTokenCount 已含思考(total = prompt + candidates);现行
// v1beta 参考手册写的是 total = prompt + thoughts + candidates(Gemini 3
// 思考另计一笔,Interactions API 的 total_thought_tokens 同口径)。中立
// 契约要求"reasoning 含在 output_tokens 里,不是另加的一笔",所以拿
// 服务端自己报的 totalTokenCount 对账:对得上"另计"就把 thoughts 并进
// output;对得上"已含"照旧;两者都对不上或没报 total(旧端、账目不合),
// 按 2.5 旧口径,不改判——宁可少算不瞎加。
Usage ParseUsage(const json& usage) {
    Usage out;
    const std::int64_t prompt_total = usage.value("promptTokenCount", static_cast<std::int64_t>(0));
    const std::int64_t cached = usage.value("cachedContentTokenCount", static_cast<std::int64_t>(0));
    out.cache_read_tokens = cached;
    out.input_tokens = prompt_total > cached ? prompt_total - cached : 0;
    const std::int64_t candidates = usage.value("candidatesTokenCount", static_cast<std::int64_t>(0));
    const std::int64_t thoughts = usage.value("thoughtsTokenCount", static_cast<std::int64_t>(0));
    out.output_tokens = candidates;
    out.output_reasoning_tokens = thoughts;
    const std::int64_t total = usage.value("totalTokenCount", static_cast<std::int64_t>(0));
    if (total > 0 && thoughts > 0) {
        const std::int64_t base = prompt_total + candidates;
        if (total == base + thoughts && total != base) {
            out.output_tokens = candidates + thoughts;  // 思考另计的一代,并进 output
        }
    }
    return out;
}

}  // namespace

std::vector<StreamEvent> EventParser::Consume(const SseFrame& frame) try {
    json data;
    try {
        data = json::parse(frame.data);
    } catch (const json::parse_error&) {
        return {};  // 坏帧当没看见,不崩流
    }
    if (!data.is_object()) {
        return {};
    }

    // 服务端业务错误:{"error":{"code":429,"message":"...","status":"..."}}。
    if (auto error = data.find("error"); error != data.end() && error->is_object()) {
        return {StreamError{error->value("message", std::string("未知错误"))}};
    }

    std::vector<StreamEvent> events;

    if (!started_) {
        const std::string model = data.value("modelVersion", std::string());
        if (!model.empty()) {
            started_ = true;
            model_ = model;
            events.push_back(MessageStart{std::string(), model});
        }
    }

    if (auto usage = data.find("usageMetadata"); usage != data.end() && usage->is_object()) {
        // 帧里真有 usageMetadata 才算 provider 明报(Token 账本单 A0)。
        usage_reported_ = true;
        usage_ = ParseUsage(*usage);
    }

    auto candidates = data.find("candidates");
    if (candidates == data.end() || !candidates->is_array() || candidates->empty()) {
        return events;
    }
    // 多 candidates(候选数>1 的配置)不看:中立层只有一条消息,取第 0 只。
    const json& candidate = (*candidates)[0];
    if (!candidate.is_object()) {
        return events;
    }
    saw_payload_ = true;
    if (auto finish = candidate.find("finishReason"); finish != candidate.end() && finish->is_string()) {
        finish_reason_ = finish->get<std::string>();
    }
    auto content = candidate.find("content");
    if (content == candidate.end() || !content->is_object()) {
        return events;
    }
    auto parts = content->find("parts");
    if (parts == content->end() || !parts->is_array()) {
        return events;
    }
    for (const auto& part : *parts) {
        if (!part.is_object()) {
            continue;
        }
        if (auto call = part.find("functionCall"); call != part.end() && call->is_object()) {
            PendingCall pending;
            pending.name = call->value("name", std::string());
            pending.args = call->contains("args") && (*call)["args"].is_object()
                               ? (*call)["args"]
                               : json::object();
            if (!pending.name.empty()) {
                calls_.push_back(std::move(pending));
            }
            continue;
        }
        if (auto text = part.find("text"); text != part.end() && text->is_string()) {
            // thought:true 的 part 是思考正文,映射成 ThinkingDelta;
            // 普通文本走 TextDelta。到帧就吐,不攒。
            if (part.value("thought", false)) {
                events.push_back(ThinkingDelta{text->get<std::string>(), std::string()});
            } else {
                events.push_back(TextDelta{text->get<std::string>()});
            }
        }
    }

    // finishReason 只在收尾帧出现:一到就落锤,后面的帧(协议上不该再有
    // 有用的载荷)不再重复吐 MessageDone。
    if (!finish_reason_.empty() && !finished_) {
        std::vector<StreamEvent> flushed = Flush();
        events.insert(events.end(), flushed.begin(), flushed.end());
    }
    return events;
} catch (const json::exception&) {
    // 字段在但类型不对时 .value()/.get() 抛 type_error——这里跑在 libcurl
    // 的 WriteCallback 栈上,异常穿透出去就是未定义行为。坏帧一律当没看见;
    // 整条流缺了 MessageDone 的兜底在 client 层(send_stream 末尾检查)。
    return {};
}

std::vector<StreamEvent> EventParser::Finish() {
    if (finished_) {
        return {};
    }
    return Flush();
}

std::vector<StreamEvent> EventParser::Flush() {
    finished_ = true;
    std::vector<StreamEvent> events;
    if (!calls_.empty()) {
        // 先给可能还开着的文本块收个尾(index 0),再按次序吐工具事件。
        events.push_back(ContentBlockDone{0});
        for (std::size_t index = 0; index < calls_.size(); ++index) {
            // Gemini 的 functionCall 没有调用 id,本地按次序造一枚;回传时
            // functionResponse 只认函数名,这枚 id 纯粹是中立层的账。
            const std::string id = "gemini_tool_" + std::to_string(index);
            events.push_back(ToolUseStart{static_cast<int>(index), id, calls_[index].name});
            events.push_back(ToolUseInputDelta{static_cast<int>(index), calls_[index].args.dump()});
            events.push_back(ContentBlockDone{static_cast<int>(index)});
        }
    }
    if (saw_payload_ || started_ || !calls_.empty()) {
        MessageDone done;
        done.stop_reason = StopReason(finish_reason_, !calls_.empty());
        done.usage = usage_;
        done.usage_reported = usage_reported_;
        events.push_back(std::move(done));
    }
    return events;
}

}  // namespace lubancode::api::gemini
