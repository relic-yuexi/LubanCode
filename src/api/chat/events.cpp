#include "api/chat/events.hpp"

#include <iostream>

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

// usage 对象 -> 统一口径(api::Usage 文件头注释)。三副形状:
//   1) DeepSeek 顶层 prompt_cache_hit_tokens/prompt_cache_miss_tokens:
//      input=miss,cache_read=hit。与 prompt_tokens 同时出现时校验
//      hit+miss==prompt_tokens,不等保留 hit/miss 原数、打一行诊断,不崩。
//   2) OpenAI/Qwen 风格 prompt_tokens_details.cached_tokens(已含在
//      prompt_tokens 总数里):cache_read=cached,input=total-cached。
//   3) 光杆 prompt_tokens:input=total,cache_read=0。
// 每次带 usage 的帧整个覆盖(不是累加)——finish chunk 与 [DONE] 前的
// 独立 usage chunk 各来一次也只认最后一份数,不会重复累计。
Usage ParseUsage(const nlohmann::json& usage) {
    Usage out;
    out.output_tokens = usage.value("completion_tokens", static_cast<std::int64_t>(0));
    const std::int64_t prompt_total = usage.value("prompt_tokens", static_cast<std::int64_t>(0));
    // reasoning 拆账(OpenAI/vLLM 兼容):completion_tokens_details.reasoning_tokens,
    // 已含在 completion_tokens 总数里。Qwen 系有的版本顶层直接叫
    // reasoning_tokens,一并认。没拆账就是 0(语义见 api::Usage 注释)。
    if (auto details = usage.find("completion_tokens_details"); details != usage.end() && details->is_object()) {
        out.output_reasoning_tokens =
            details->value("reasoning_tokens", static_cast<std::int64_t>(0));
    }
    if (out.output_reasoning_tokens == 0) {
        out.output_reasoning_tokens = usage.value("reasoning_tokens", static_cast<std::int64_t>(0));
    }
    const bool has_deepseek_hit = usage.contains("prompt_cache_hit_tokens");
    const bool has_deepseek_miss = usage.contains("prompt_cache_miss_tokens");
    if (has_deepseek_hit || has_deepseek_miss) {
        const std::int64_t hit = usage.value("prompt_cache_hit_tokens", static_cast<std::int64_t>(0));
        const std::int64_t miss = usage.value("prompt_cache_miss_tokens", static_cast<std::int64_t>(0));
        if (prompt_total > 0 && hit + miss != prompt_total) {
            // 服务端账目不合:以 hit/miss 为准(它俩才是缓存口径的分项),
            // 保留原数、只记诊断,绝不崩会话。
            std::cerr << "[usage] DeepSeek prompt_cache_hit(" << hit << ")+miss(" << miss
                      << ") != prompt_tokens(" << prompt_total << "),按 hit/miss 记账\n";
        }
        out.input_tokens = miss;
        out.cache_read_tokens = hit;
        return out;
    }
    if (auto details = usage.find("prompt_tokens_details"); details != usage.end() && details->is_object()) {
        const std::int64_t cached = details->value("cached_tokens", static_cast<std::int64_t>(0));
        out.cache_read_tokens = cached;
        out.input_tokens = prompt_total > cached ? prompt_total - cached : 0;
        return out;
    }
    out.input_tokens = prompt_total;
    return out;
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
        usage_ = ParseUsage(*usage);
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
        // 思考增量(流式立即吐,不攒)。字段名两家方言:DeepSeek 系叫
        // reasoning_content,vLLM 0.27+/Qwen 系叫 reasoning(本机 vLLM
        // 0.27.1 + qwen3.8-27b 实测)。provider 声明了 reasoning_delta_field
        // 就只认那一个;没声明走自动兼容,两个只读别名都认。同一 chunk
        // 两者都有时必须去重(镜像字段的服务端会成对发),优先级定死:
        // 声明字段 > reasoning_content > reasoning,一只 chunk 最多吐一份
        // ThinkingDelta——绝不两份拼接,也不静默丢弃其中一个的判断依据
        // 藏在实现里(就是这里的固定次序)。
        {
            std::string reasoning_text;
            if (!reasoning_delta_field_.empty()) {
                if (auto declared = delta->find(reasoning_delta_field_); declared != delta->end() &&
                                                               declared->is_string()) {
                    reasoning_text = declared->get<std::string>();
                }
            } else {
                const std::string canonical =
                    delta->contains("reasoning_content") && delta->at("reasoning_content").is_string()
                        ? delta->at("reasoning_content").get<std::string>()
                        : std::string();
                const std::string alias = delta->contains("reasoning") && delta->at("reasoning").is_string()
                                               ? delta->at("reasoning").get<std::string>()
                                               : std::string();
                // 两者都非空:相等(镜像)按一份算;不等按优先级取
                // reasoning_content,另一份弃掉并在诊断里点名,不装没事。
                if (!canonical.empty() && !alias.empty() && canonical != alias) {
                    std::cerr << "[reasoning] 同一 chunk 里 reasoning_content 与 reasoning 内容不同,"
                              << "按声明优先级取 reasoning_content(" << canonical.size() << " 字节),"
                              << "弃 reasoning(" << alias.size() << " 字节)\n";
                }
                reasoning_text = !canonical.empty() ? canonical : alias;
            }
            if (!reasoning_text.empty()) {
                events.push_back(ThinkingDelta{reasoning_text, ""});
            }
        }
        // 结构化 reasoning_details(OpenAI 风格):当前版本不映射成
        // ThinkingDelta(映射另定,见规格),但也不静默吞掉——计数留账,
        // Finish() 里打一行诊断,fixture 里的原始形状留在测试里。
        if (auto details = delta->find("reasoning_details");
            details != delta->end() && details->is_array() && !details->empty()) {
            reasoning_details_blocks_ += static_cast<int>(details->size());
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
    // 结构化 reasoning_details 的留账诊断:不映射(映射另定)但必须说破,
    // 不让"模型回了结构化思考、客户端一个字没接"这件事无声发生。
    if (reasoning_details_blocks_ > 0 && !reasoning_details_diagnostic_printed_) {
        reasoning_details_diagnostic_printed_ = true;
        std::cerr << "[reasoning] 收到 " << reasoning_details_blocks_
                  << " 个结构化 reasoning_details 块,当前版本未映射成 thinking(计入思考链的映射另定),"
                  << "未混入正文\n";
    }
    return events;
}

}  // namespace lubancode::api::chat
