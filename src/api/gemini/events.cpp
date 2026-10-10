#include "api/gemini/events.hpp"
#include "api/usage_event_projection.hpp"
#include "api/usage_lexical.hpp"

#include <string>
#include <utility>

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

}  // namespace

std::vector<StreamEvent> EventParser::Consume(const SseFrame& frame) try {
    const usage_wire::LexicalUsage lexical(frame.data, usage_wire::Dialect::Gemini);
    json data;
    try {
        data = json::parse(frame.data);
    } catch (const json::exception&) {
        if (lexical.numbers.empty()) return {};
        std::vector<StreamEvent> recovered;
        if (lexical.response_id) recovered.push_back(ProviderResponseIdentity{*lexical.response_id});
        auto partial = lexical.Partial();
        if (partial) recovered.push_back(usage_wire::Nonterminal(*partial, lexical.response_id));
        recovered.push_back(Fail(StreamError{"accounting recovered from an unparseable frame", "usage.frame.incomplete"}));
        return recovered;
    }
    if (!data.is_object()) {
        return {};
    }

    // 服务端业务错误:{"error":{"code":429,"message":"...","status":"..."}}。
    std::vector<StreamEvent> events;
    // Capture accounting before body conversion. A later bad payload must not
    // erase returned usage, and this event never declares a success terminal.
    const auto response_id=usage_wire::ResponseId(usage_wire::Find(data,{"responseId"}));
    const bool changed_response_id=response_id && *response_id && provider_response_id_ &&
                                   **response_id!=*provider_response_id_;
    if (response_id && *response_id) {
        if (changed_response_id) conflicting_response_id_ = **response_id;
        else provider_response_id_ = **response_id;
        events.push_back(ProviderResponseIdentity{**response_id});
    }
    bool fresh_usage = false;
    if (auto usage = data.find("usageMetadata"); usage != data.end() && usage->is_object()) {
        auto observed = usage_wire::Gemini(*usage, &lexical.numbers);
        if (!observed) {
            events.push_back(Fail(StreamError{std::string(observed.error()), "usage.material.invalid"}));
            return events;
        }
        usage_material_ = std::move(*observed);
        if (!lexical.complete || lexical.duplicate) usage_wire::LexicalUsage::MarkIncomplete(*usage_material_);
        if (!usage_material_->material_error.empty()) {
            events.push_back(usage_wire::Nonterminal(*usage_material_, provider_response_id_));
            events.push_back(Fail(StreamError{std::string(usage_material_->material_error), "usage.material.invalid"}));
            return events;
        }
        fresh_usage = true;
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
        std::string code;
        if (auto code_it = error->find("code"); code_it != error->end()) {
            code = code_it->is_string() ? code_it->get<std::string>() : code_it->dump();
        }
        if (code.empty()) {
            code = error->value("status", std::string());
        }
        events.push_back(Fail(StreamError{error->value("message", std::string("未知错误")), std::move(code)}));
        return events;
    }


    if (!started_) {
        const std::string model = data.value("modelVersion", std::string());
        if (!model.empty()) {
            started_ = true;
            model_ = model;
            events.push_back(MessageStart{provider_response_id_.value_or(std::string()), model});
        }
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
                events.push_back(ThinkingDelta{text->get<std::string>(), part.value("thoughtSignature", std::string())});
                if (part.contains("thoughtSignature")) events.push_back(ContentBlockDone{0});
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
    } catch (const nlohmann::json::exception&) {
        events.push_back(Fail(StreamError{"model payload has an invalid JSON field type","model.payload.invalid"}));
    }
    return events;
} catch (const json::exception&) {
    // 字段在但类型不对时 .value()/.get() 抛 type_error——这里跑在 libcurl
    // 的 WriteCallback 栈上,异常穿透出去就是未定义行为。坏帧一律当没看见;
    // 整条流缺了 MessageDone 的兜底在 client 层(send_stream 末尾检查)。
    return {};
}

std::vector<StreamEvent> EventParser::Finish() {
    if (failed_) return {};
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
            // (子代理空轨迹单 P0-F 注:这与 chat/anthropic/responses 的
            // "空 id 禁伪造"不冲突——Gemini 协议根本不发 id,本地序号是
            // 唯一身份方案且从不参与 provider 回喂配对,不是掩盖协议故障
            // 的假 id。其余三条 wire 的空 id 一律由 assembler 挡下。)
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
        if (usage_material_) usage_wire::Apply(done,*usage_material_,provider_response_id_);
        else done.provider_response_id=provider_response_id_;
        events.push_back(std::move(done));
    }
    return events;
}

}  // namespace lubancode::api::gemini
