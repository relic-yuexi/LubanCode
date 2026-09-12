#include "api/chat/events.hpp"

#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include "platform/log_sink.hpp"

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
//      hit+miss==prompt_tokens,不等保留 hit/miss 原数、anomaly 点名,不崩。
//      只报一项时按明确规则推算/标异常,不拿缺项零值造总量(缓存用量按
//      Wire 归一单 C4):另一项在而 prompt_tokens 也在 -> 缺项=T-已知项
//      (推算进 anomaly 说明);prompt_tokens 缺 -> 已知项照记,总量未知,
//      anomaly 点名,样本按异常排除出精确比例。
//   2) OpenAI/Qwen 风格 prompt_tokens_details.cached_tokens(已含在
//      prompt_tokens 总数里):cache_read=cached,input=total-cached。cached
//      超过 total 是矛盾账:原数照记、input 为负也照记,anomaly 点名,
//      不截零不截到 100%。
//   3) 光杆 prompt_tokens:input=total,cache_read=0。
// 与 DeepSeek 顶层同现的 details.cached_tokens(有的网关两副都回):只算
// 一次——DeepSeek 分项优先,cached 与 hit 不等时 anomaly 点名,绝不相加。
// 每次带 usage 的帧整个覆盖(不是累加)——finish chunk 与 [DONE] 前的
// 独立 usage chunk 各来一次也只认最后一份数,不会重复累计。
struct ParsedUsage {
    Usage usage;
    bool cache_reported = false;  // 读取明细字段真在场(provider 明报;推算值不算)
    std::string anomaly;          // 空 = 自洽;非空 = 矛盾账的人话,数字保留原数
};

// 判型读整数(const json 只 find/contains,operator[] 查缺键是 UB):在场
// 且类型对才取;类型错/负数记 anomaly,不吞帧不崩。
std::optional<std::int64_t> ReadIntField(const nlohmann::json& obj, const char* key, std::string* anomaly) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return std::nullopt;
    }
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

ParsedUsage ParseUsage(const nlohmann::json& usage) {
    ParsedUsage parsed;
    Usage& out = parsed.usage;
    if (auto v = ReadIntField(usage, "completion_tokens", &parsed.anomaly)) {
        out.output_tokens = *v;
    }
    const std::optional<std::int64_t> prompt_total = ReadIntField(usage, "prompt_tokens", &parsed.anomaly);
    const std::int64_t total = prompt_total.value_or(0);
    // reasoning 拆账(OpenAI/vLLM 兼容):completion_tokens_details.reasoning_tokens,
    // 已含在 completion_tokens 总数里。Qwen 系有的版本顶层直接叫
    // reasoning_tokens,一并认。没拆账就是 0(语义见 api::Usage 注释)。
    if (auto details = usage.find("completion_tokens_details"); details != usage.end() && details->is_object()) {
        if (auto v = ReadIntField(*details, "reasoning_tokens", &parsed.anomaly)) {
            out.output_reasoning_tokens = *v;
        }
    }
    if (out.output_reasoning_tokens == 0) {
        if (auto v = ReadIntField(usage, "reasoning_tokens", &parsed.anomaly)) {
            out.output_reasoning_tokens = *v;
        }
    }
    const std::optional<std::int64_t> deepseek_hit = ReadIntField(usage, "prompt_cache_hit_tokens", &parsed.anomaly);
    const std::optional<std::int64_t> deepseek_miss =
        ReadIntField(usage, "prompt_cache_miss_tokens", &parsed.anomaly);
    if (deepseek_hit.has_value() || deepseek_miss.has_value()) {
        const std::int64_t hit = deepseek_hit.value_or(0);
        const std::int64_t miss = deepseek_miss.value_or(0);
        if (deepseek_hit.has_value() && deepseek_miss.has_value()) {
            // 两项齐:以 hit/miss 为准(它俩才是缓存口径的分项),保留原数。
            if (prompt_total.has_value() && prompt_total.value() > 0 && hit + miss != prompt_total.value()) {
                // 服务端账目不合:保留原数、anomaly 点名,绝不崩会话。
                parsed.anomaly = "prompt_cache_hit(" + std::to_string(hit) + ")+miss(" + std::to_string(miss) +
                                 ") != prompt_tokens(" + std::to_string(prompt_total.value()) + ")";
                platform::LogSink::Instance().Warn(
                    "chat", "DeepSeek prompt_cache_hit(" + std::to_string(hit) + ")+miss(" +
                                std::to_string(miss) + ") != prompt_tokens(" +
                                std::to_string(prompt_total.value()) + "),按 hit/miss 记账,样本标异常");
            }
            out.input_tokens = miss;
            out.cache_read_tokens = hit;
            parsed.cache_reported = true;
        } else if (deepseek_hit.has_value()) {
            // 只报命中:prompt_tokens 在就推算 miss(=T-hit,负数照记,矛盾
            // 由 anomaly 说);不在则总量未知——绝不拿 0 补 miss 造出 100%。
            if (prompt_total.has_value()) {
                const std::int64_t inferred_miss = prompt_total.value() - hit;
                if (parsed.anomaly.empty()) {
                    parsed.anomaly = "prompt_cache_miss_tokens 缺席,按 prompt_tokens(" +
                                     std::to_string(prompt_total.value()) + ")-hit(" + std::to_string(hit) +
                                     ") 推算 miss=" + std::to_string(inferred_miss);
                }
                out.input_tokens = inferred_miss;
            } else {
                parsed.anomaly = "prompt_cache_miss_tokens/prompt_tokens 均缺席,普通输入未知";
                out.input_tokens = 0;
            }
            out.cache_read_tokens = hit;
            parsed.cache_reported = true;
        } else {
            // 只报普通输入:命中量未知,比例无从算——记 miss、标异常,不把
            // 读取冒充已知零。
            if (prompt_total.has_value()) {
                const std::int64_t inferred_hit = prompt_total.value() - miss;
                if (parsed.anomaly.empty()) {
                    parsed.anomaly = "prompt_cache_hit_tokens 缺席,按 prompt_tokens(" +
                                     std::to_string(prompt_total.value()) + ")-miss(" + std::to_string(miss) +
                                     ") 推算 hit=" + std::to_string(inferred_hit);
                }
                out.cache_read_tokens = inferred_hit;
            } else {
                parsed.anomaly = "prompt_cache_hit_tokens/prompt_tokens 均缺席,命中量未知";
            }
            out.input_tokens = miss;
            // 只报 miss 时读取明细并未明报(推算值不算):read 位留 false,
            // "未知"与"明报零"分家,消费端靠 anomaly 知道有推算。
            parsed.cache_reported = false;
        }
        // 兼容网关两副字段同现:details.cached_tokens 与 DeepSeek 分项并存时
        // 只认一份,数值不一致点名,绝不把 cached 再加进读取。
        if (auto details = usage.find("prompt_tokens_details"); details != usage.end() && details->is_object()) {
            if (auto cached_it = details->find("cached_tokens");
                cached_it != details->end() && cached_it->is_number_integer()) {
                const std::int64_t cached = cached_it->get<std::int64_t>();
                if (cached != out.cache_read_tokens && parsed.anomaly.empty()) {
                    parsed.anomaly = "prompt_cache_hit(" + std::to_string(out.cache_read_tokens) +
                                     ") 与 prompt_tokens_details.cached_tokens(" + std::to_string(cached) +
                                     ")不一致,按 DeepSeek 分项记,读取只算一次";
                }
            }
        }
        return parsed;
    }
    if (auto details = usage.find("prompt_tokens_details"); details != usage.end() && details->is_object()) {
        if (auto cached_it = details->find("cached_tokens"); cached_it != details->end()) {
            if (cached_it->is_number_integer()) {
                const std::int64_t cached = cached_it->get<std::int64_t>();
                out.cache_read_tokens = cached;
                // cached>T 是矛盾账:input=T-cached 为负照记,anomaly 点名,
                // 不截零、不截到 100% 掩盖。
                if (cached > total && parsed.anomaly.empty()) {
                    parsed.anomaly = "cached_tokens(" + std::to_string(cached) + ") > prompt_tokens(" +
                                     std::to_string(total) + ")";
                }
                out.input_tokens = total - cached;
                parsed.cache_reported = true;
                return parsed;
            }
            if (parsed.anomaly.empty()) {
                parsed.anomaly = "prompt_tokens_details.cached_tokens 类型不是整数";
            }
        }
    }
    out.input_tokens = total;
    return parsed;
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
        return {StreamError{error->value("message", "未知错误"),
                            error->value("code", error->value("type", std::string()))}};
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
        // 帧里真有 usage 对象才算 provider 明报(Token 账本单 A0):后面
        // 凑 MessageDone 时把这位带出去,明报全零与没报分家。整个覆盖
        //(不是累加),最后一帧说了算;异常账随最后一帧的解析走。
        usage_reported_ = true;
        const ParsedUsage parsed = ParseUsage(*usage);
        usage_ = parsed.usage;
        cache_read_reported_ = parsed.cache_reported;
        cache_creation_reported_ = false;  // chat wire 无缓存写入概念:未报,不冒充
        usage_anomaly_ = parsed.anomaly;
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
                // reasoning_content,另一份弃掉并打一行诊断(整场只打一次,
                // 逐 chunk 念叨只会刷屏),不装没事。
                if (!canonical.empty() && !alias.empty() && canonical != alias &&
                    !reasoning_conflict_diagnostic_printed_) {
                    reasoning_conflict_diagnostic_printed_ = true;
                    platform::LogSink::Instance().Warn(
                        "chat",
                        "同一 chunk 里 reasoning_content 与 reasoning 内容不同,"
                        "按固定优先级取 reasoning_content,弃 reasoning(此诊断整场只报一次)");
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
            // P0-F(子代理空轨迹单):id 缺席不再本地造 "chat_tool_N" 假 id——
            // 假 id 会把故障推迟到下一次回喂(provider 不认,配对断裂)。
            // 终帧身份随 done 帧交回 assembler;仍无 id 的调用由 assembler
            // 丢弃并计数,消费端把这份输出按畸形收口。
            events.push_back(ToolUseStart{index, call.id, call.name});
            events.push_back(ToolUseInputDelta{index, call.arguments});
            ContentBlockDone done;
            done.index = index;
            done.tool_use_id = call.id;
            events.push_back(std::move(done));
        }
    }
    if (saw_payload_ || started_ || !tool_calls_.empty()) {
        MessageDone done;
        done.stop_reason = StopReason(finish_reason_, !tool_calls_.empty());
        done.usage = usage_;
        done.usage_reported = usage_reported_;
        done.cache_read_reported = cache_read_reported_;
        done.cache_creation_reported = cache_creation_reported_;
        done.usage_anomaly = usage_anomaly_;
        events.push_back(std::move(done));
    }
    // 结构化 reasoning_details 的留账诊断:不映射(映射另定)但必须说破,
    // 不让"模型回了结构化思考、客户端一个字没接"这件事无声发生。
    if (reasoning_details_blocks_ > 0 && !reasoning_details_diagnostic_printed_) {
        reasoning_details_diagnostic_printed_ = true;
        platform::LogSink::Instance().Info(
            "chat", "收到 " + std::to_string(reasoning_details_blocks_) +
                        " 个结构化 reasoning_details 块,当前版本未映射成 thinking"
                        "(计入思考链的映射另定),未混入正文");
    }
    return events;
}

}  // namespace lubancode::api::chat
