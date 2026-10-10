#include "api/chat/events.hpp"
#include "api/usage_event_projection.hpp"
#include "api/usage_lexical.hpp"

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

}  // namespace

std::vector<StreamEvent> EventParser::Consume(const SseFrame& frame) try {
    if (frame.data == "[DONE]") {
        return Finish();
    }

    const usage_wire::LexicalUsage lexical(frame.data, usage_wire::Dialect::Chat);
    nlohmann::json data;
    try { data = nlohmann::json::parse(frame.data); }
    catch (const nlohmann::json::exception&) {
        if (lexical.numbers.empty()) return {};
        std::vector<StreamEvent> recovered;
        auto partial = lexical.Partial(&usage_wire::NumericDeliveryOwner::Observe, &numeric_delivery_);
        if (partial) numeric_delivery_.Own(*partial);
        if (lexical.response_id) recovered.push_back(ProviderResponseIdentity{*lexical.response_id});
        if (partial) recovered.push_back(usage_wire::Nonterminal(*partial, lexical.response_id));
        recovered.push_back(Fail(StreamError{"accounting recovered from an unparseable frame", "usage.frame.incomplete"}));
        return recovered;
    }
    if (!data.is_object()) {
        return {};
    }

    std::vector<StreamEvent> events;
    // Capture accounting before body conversion. A later bad payload must not
    // erase returned usage, and this event never declares a success terminal.
    bool fresh_usage = false;
    if (auto usage = data.find("usage"); usage != data.end() && usage->is_object()) {
        auto observed = usage_wire::Chat(*usage, &lexical.numbers,
            &usage_wire::NumericDeliveryOwner::Observe, &numeric_delivery_);
        if (!observed) {
            events.push_back(Fail(StreamError{std::string(observed.error()), "usage.material.invalid"}));
            return events;
        }
        usage_material_ = std::move(*observed);
        numeric_delivery_.Own(*usage_material_);
        if (!lexical.complete || lexical.duplicate) usage_wire::LexicalUsage::MarkIncomplete(*usage_material_);
        fresh_usage = true;
    }
    const auto response_id=usage_wire::ResponseId(usage_wire::Find(data,{"id"}));
    const bool changed_response_id=response_id && *response_id && provider_response_id_ &&
                                   **response_id!=*provider_response_id_;
    if (response_id && *response_id) {
        if (changed_response_id) conflicting_response_id_ = **response_id;
        else provider_response_id_ = **response_id;
        events.push_back(ProviderResponseIdentity{**response_id});
    }
    if (fresh_usage && !usage_material_->material_error.empty()) {
        events.push_back(usage_wire::Nonterminal(*usage_material_, provider_response_id_));
        events.push_back(Fail(StreamError{std::string(usage_material_->material_error), "usage.material.invalid"}));
        return events;
    }
    if (usage_material_ && (fresh_usage || changed_response_id)) {
        if (conflicting_response_id_) {
            const auto annotated = usage_wire::IdentityConflict(*usage_material_, *provider_response_id_, *conflicting_response_id_);
            if (!annotated) {
                events.push_back(usage_wire::NumericUnknown(*usage_material_));
                events.push_back(Fail(StreamError{std::string(annotated.error()), "usage.identity.material_invalid"}));
                return events;
            }
        }
        const auto observed_id = response_id && *response_id ? *response_id
            : (conflicting_response_id_ ? conflicting_response_id_ : provider_response_id_);
        auto snapshot = usage_wire::Nonterminal(*usage_material_, observed_id);
        usage_ = snapshot.usage;
        usage_reported_ = true;
        cache_read_reported_ = snapshot.cache_read_reported;
        cache_creation_reported_ = snapshot.cache_creation_reported;
        usage_anomaly_ = snapshot.usage_anomaly;
        events.push_back(std::move(snapshot));
    }
    if (!response_id) {
        events.push_back(Fail(StreamError{std::string(response_id.error()),"usage.response_id.invalid"}));
        return events;
    }
    if (changed_response_id) {
        events.push_back(Fail(StreamError{"provider response ID changed within one stream","usage.response_id.changed"}));
        return events;
    }
    if (failed_) return events;  // Late accounting survives; body/success stay fenced.
    try {
    if (auto error = data.find("error"); error != data.end() && error->is_object()) {
        events.push_back(Fail(StreamError{error->value("message", "未知错误"),
                            error->value("code", error->value("type", std::string()))}));
        return events;
    }

    if (!started_) {
        const std::string id = data.value("id", "");
        const std::string model = data.value("model", "");
        if (!id.empty() || !model.empty()) {
            started_ = true;
            events.push_back(MessageStart{id, model});
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
    } catch (const nlohmann::json::exception&) {
        events.push_back(Fail(StreamError{"model payload has an invalid JSON field type","model.payload.invalid"}));
    }
    return events;
} catch (const nlohmann::json::exception&) {
    return {};
}

std::vector<StreamEvent> EventParser::Finish() {
    if (failed_) return {};
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
        if (usage_material_) usage_wire::Apply(done, *usage_material_, provider_response_id_);
        else done.provider_response_id = provider_response_id_;
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
