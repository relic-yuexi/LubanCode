#include "api/responses/events.hpp"
#include "api/usage_event_projection.hpp"
#include "api/usage_lexical.hpp"

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
// response.reasoning_text.delta:思考正文的流式增量(vLLM 扩展,OpenAI 新版
// API 同名)——字段同样叫 delta,同走这一枝;done/part 系不单独发事件。
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
    if (type == "image_generation_call") {
        // 服务端内置的图片生成:与 web_search 同款的"只展示、不本地执行"
        // 条目。真正的正文在 output_item.done 的 result(base64)里,由
        // ImageOutput 事件另走;这里先开一张卡,让终端看得到"正在生成"。
        BuiltinToolStart event;
        event.id = it->value("id", "");
        event.name = "image_generation";
        return event;
    }
    if (type != "function_call") {
        // message 类型的起始不单独发事件,文本内容靠后续
        // response.output_text.delta 一段段拼出来；reasoning 与尚未接线的
        // 其它内置工具静默跳过。web_search_call/image_generation_call 已在
        // 上面单独发展示事件。
        return std::nullopt;
    }
    ToolUseStart event;
    event.index = data.value("output_index", 0);
    event.id = it->value("call_id", "");
    event.name = it->value("name", "");
    // async 位原样保留(异步工具单 §3):responses 回包 function_call 的
    // async 标记随调用证据进中立层,不在这里解释。
    if (auto async_it = it->find("async"); async_it != it->end() && async_it->is_boolean()) {
        event.async_call = async_it->get<bool>();
    }
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
    if (type == "reasoning") {
        ThinkingDelta event;
        event.responses_item = *it;
        return event;
    }
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
    if (type == "image_generation_call") {
        // 图片正文到站:整个 result(base64)随这一帧到齐。翻成 ImageOutput
        // 交给宿主解码落盘;这里不发 BuiltinToolDone——卡片的收尾(带落盘
        // 路径与尺寸)由宿主落完盘自己发,落不了盘就以错误收尾,不冒充成功。
        // result 为空(status=failed 或被掐)时不发事件:没有正文可救。
        if (auto result = it->find("result"); result != it->end() && result->is_string() &&
                                             !result->get_ref<const std::string&>().empty()) {
            return ImageOutput{it->value("id", ""), result->get<std::string>()};
        }
        return std::nullopt;
    }
    if (type != "message" && type != "function_call") {
        // 没有对应的 ToolUseStart/文本块起始(reasoning、内置工具调用……),
        // 没什么可收尾的,跳过。
        return std::nullopt;
    }
    ContentBlockDone event;
    event.index = data.value("output_index", 0);
    // P0-F(子代理空轨迹单):兼容端早帧(output_item.added)可能缺
    // call_id,终帧(output_item.done 的 function_call 条目)才给全——
    // 终帧身份按 output index 交回 assembler 合并。到收尾仍无 id 的调用
    // 由 assembler 丢弃(不进 ToolUseBlock,不伪造 id)。
    if (type == "function_call") {
        event.tool_use_id = it->value("call_id", "");
    }
    return event;
}

// 收尾事件(response.completed 帧里的 response 对象,或非流式响应体顶层):
// stop_reason 三态 + usage 摊法,流式/非流式两路共用同一口径。
std::expected<MessageDone,std::string_view> DoneFromResponseObject(const json& response) {
    bool has_pending_function_call = false;
    if (auto output_it = response.find("output"); output_it != response.end() && output_it->is_array()) {
        for (const auto& item : *output_it) {
            if (!item.is_object()) {
                continue;
            }
            if (item.value("type", "") == "function_call") {
                has_pending_function_call = true;
            }
        }
    }

    MessageDone event;
    const std::string status = response.value("status", "");
    if (status=="failed") return std::unexpected("model.response.failed");
    if (status=="cancelled") return std::unexpected("model.response.cancelled");
    if (status=="queued" || status=="in_progress") return std::unexpected("model.response.not_terminal");

    if (status == "incomplete") {
        event.stop_reason = "max_tokens";
    } else if (has_pending_function_call) {
        event.stop_reason = "tool_use";
    } else {
        event.stop_reason = "end_turn";
    }

    if (auto usage=response.find("usage"); usage!=response.end() && usage->is_object()) {
        auto snapshot=usage_wire::Responses(*usage);
        if (!snapshot) return std::unexpected(snapshot.error());
        usage_wire::Apply(event,*snapshot);
    }
    const auto id=usage_wire::ResponseId(usage_wire::Find(response,{"id"}));
    if (!id) return std::unexpected(id.error());
    event.provider_response_id=*id;

    return event;
}

std::optional<StreamEvent> HandleCompleted(const json& data) {
    auto it = data.find("response");
    if (it == data.end() || !it->is_object()) {
        return std::nullopt;
    }

    // 图片正文只认 output_item.done 那一路——本函数单一返回值,发不出
    // "ImageOutput + MessageDone" 两枚,completed 里的 image_generation_call
    // 条目在收尾判定里当普通非工具条目看(不带 stop_reason 变化)。吞图
    // 冒充成功的防线在宿主端:ImageOutput 事件没人接、解码或落盘失败,
    // agent 层都会把回合明败,见 agent/loop.cpp 的 on_model_image 口。
    // 重复终帧(同一 completed 到两遍)也由宿主按 item id 去重。

    auto done=DoneFromResponseObject(*it);
    if (!done) return StreamError{std::string(done.error()),"model.response.invalid"};
    return std::move(*done);
}

// 错误体的人话拼装(ccmoon 真机巡检单 P1):message 为主,type/code 有就
// 带上——只回一句 "Upstream request failed" 的中转,把 type 拼进去才指得
// 上路。正文本体不进消息(防泄漏),这里只动 error 对象自己的短字段。
std::string ComposeErrorMessage(const json& error) {
    const std::string message = error.value("message", std::string());
    const std::string type = error.value("type", std::string());
    const std::string code = error.value("code", std::string());
    std::string out = message.empty() ? std::string("未知错误") : message;
    if (!type.empty()) {
        out += " (type=" + type;
        if (!code.empty()) {
            out += ", code=" + code;
        }
        out += ")";
    } else if (!code.empty()) {
        out += " (code=" + code + ")";
    }
    return out;
}

std::optional<StreamEvent> HandleFailed(const json& data) {
    StreamError event;
    if (auto response_it = data.find("response"); response_it != data.end() && response_it->is_object()) {
        if (auto error_it = response_it->find("error"); error_it != response_it->end() && error_it->is_object()) {
            event.message = ComposeErrorMessage(*error_it);
            event.code = error_it->value("code", error_it->value("type", std::string()));
            return event;
        }
    }
    event.message = "未知错误";
    return event;
}

std::optional<StreamEvent> HandleError(const json& data) {
    StreamError event;
    if (auto it = data.find("error"); it != data.end() && it->is_object()) {
        event.message = ComposeErrorMessage(*it);
        event.code = it->value("code", it->value("type", std::string()));
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
    if (type == "response.reasoning_text.delta") {
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
    // response.function_call_arguments.done、reasoning 的 done/part 系
    // (reasoning_text.done、reasoning_part.added/done、summary 的 done 系)、
    // 其它内置工具/MCP 相关……):静默跳过,别崩。
    return std::nullopt;
} catch (const json::exception&) {
    // 字段存在但类型不对时,.value()/.get() 抛的是 type_error(不是
    // parse_error)——这里跑在 libcurl 的 WriteCallback 栈上,异常穿透出去
    // 就是未定义行为/进程崩溃。坏帧一律当没看见;整条流缺了 MessageDone
    // 的兜底在 client 层(send_stream 末尾检查)。
    return std::nullopt;
}


std::vector<StreamEvent> EventParser::Consume(const SseFrame& frame) {
    std::vector<StreamEvent> events;
    const usage_wire::LexicalUsage lexical(frame.data, usage_wire::Dialect::Responses);
    json data;
    try { data=json::parse(frame.data); }
    catch (const json::exception&) {
        if (lexical.numbers.empty()) return events;
        if (lexical.response_id) events.push_back(ProviderResponseIdentity{*lexical.response_id});
        auto partial = lexical.Partial();
        if (partial) events.push_back(usage_wire::Nonterminal(*partial, lexical.response_id));
        events.push_back(Fail(StreamError{"accounting recovered from an unparseable frame", "usage.frame.incomplete"}));
        return events;
    }
    if (!data.is_object()) return events;
    const auto* response=usage_wire::Find(data,{"response"});
    if (response!=nullptr && response->is_object()) {
        const auto id=usage_wire::ResponseId(usage_wire::Find(*response,{"id"}));
        const bool changed=id && *id && provider_response_id_ && **id!=*provider_response_id_;
        if (id && *id) {
            if (changed) conflicting_response_id_ = **id;
            else provider_response_id_ = **id;
            events.push_back(ProviderResponseIdentity{**id});
        }
        bool fresh_usage = false;
        if (const auto* usage = usage_wire::Find(*response, {"usage"}); usage && usage->is_object()) {
            auto snapshot = usage_wire::Responses(*usage, &lexical.numbers);
            if (!snapshot) {
                events.push_back(Fail(StreamError{std::string(snapshot.error()), "usage.material.invalid"}));
                return events;
            }
            usage_material_ = std::move(*snapshot);
            if (!lexical.complete || lexical.duplicate) usage_wire::LexicalUsage::MarkIncomplete(*usage_material_);
            if (!usage_material_->material_error.empty()) {
                events.push_back(usage_wire::Nonterminal(*usage_material_, provider_response_id_));
                events.push_back(Fail(StreamError{std::string(usage_material_->material_error), "usage.material.invalid"}));
                return events;
            }
            fresh_usage = true;
        }
        if (usage_material_ && (fresh_usage || changed)) {
            if (conflicting_response_id_) {
                const auto annotated = usage_wire::IdentityConflict(*usage_material_, *provider_response_id_, *conflicting_response_id_);
                if (!annotated) {
                    events.push_back(usage_wire::NumericUnknown(*usage_material_));
                    events.push_back(Fail(StreamError{std::string(annotated.error()), "usage.identity.material_invalid"}));
                    return events;
                }
            }
            const auto observed_id = id && *id ? *id
                : (conflicting_response_id_ ? conflicting_response_id_ : provider_response_id_);
            events.push_back(usage_wire::Nonterminal(*usage_material_, observed_id));
        }
        if (!id) {
            events.push_back(Fail(StreamError{std::string(id.error()),"usage.response_id.invalid"}));
            return events;
        }
        if (changed) {
            events.push_back(Fail(StreamError{"provider response ID changed within one stream","usage.response_id.changed"}));
            return events;
        }
    }
    if (failed_) return events;  // Late accounting cannot revive this response.
    // The compatibility translator can reject a malformed payload. Accounting
    // already captured above stays in the batch instead of vanishing with it.
    auto event=parse_event(frame);
    if (event) {
        if (std::holds_alternative<StreamError>(*event)) failed_ = true;
        if (auto* done=std::get_if<MessageDone>(&*event)) {
            if (usage_material_) usage_wire::Apply(*done,*usage_material_,provider_response_id_);
            else if (!done->provider_response_id) done->provider_response_id=provider_response_id_;
        }
        events.push_back(std::move(*event));
    } else {
        const auto* type=usage_wire::Find(data,{"type"});
        if (type && type->is_string() &&
            (type->get_ref<const std::string&>()=="response.completed" ||
             type->get_ref<const std::string&>()=="response.failed"))
            events.push_back(Fail(StreamError{"terminal response has an invalid payload","model.payload.invalid"}));
    }
    return events;
}

std::vector<StreamEvent> ExpandNonStreamResponse(const std::string& body) try {
    const usage_wire::LexicalUsage lexical(body, usage_wire::Dialect::ResponsesNonStream);
    json response;
    try { response = json::parse(body); }
    catch (const json::exception&) {
        if (lexical.numbers.empty()) return {};
        std::vector<StreamEvent> recovered;
        if (lexical.response_id) recovered.push_back(ProviderResponseIdentity{*lexical.response_id});
        auto partial = lexical.Partial();
        if (partial) recovered.push_back(usage_wire::Nonterminal(*partial, lexical.response_id));
        recovered.push_back(StreamError{"accounting recovered from an unparseable response body", "usage.frame.incomplete"});
        return recovered;
    }
    if (!response.is_object()) {
        return {};
    }
    std::vector<StreamEvent> events;
    const auto id=usage_wire::ResponseId(usage_wire::Find(response,{"id"}));
    if (id && *id) events.push_back(ProviderResponseIdentity{**id});
    std::optional<usage_wire::Snapshot> material;
    if (const auto* usage=usage_wire::Find(response,{"usage"});usage && usage->is_object()) {
        auto snapshot=usage_wire::Responses(*usage, &lexical.numbers);
        if (!snapshot) {
            events.push_back(StreamError{std::string(snapshot.error()),"usage.material.invalid"});
            return events;
        }
        material = std::move(*snapshot);
        if (!lexical.complete || lexical.duplicate) usage_wire::LexicalUsage::MarkIncomplete(*material);
        if (!material->material_error.empty()) {
            events.push_back(usage_wire::Nonterminal(*material, id ? *id : std::optional<std::string>{}));
            events.push_back(StreamError{std::string(material->material_error), "usage.material.invalid"});
            return events;
        }
        events.push_back(usage_wire::Nonterminal(*material,id ? *id : std::optional<std::string>{}));
    }
    if (!id) {
        events.push_back(StreamError{std::string(id.error()),"usage.response_id.invalid"});
        return events;
    }
    try {
    auto output_it = response.find("output");
    if (output_it == response.end() || !output_it->is_array()) {
        events.push_back(StreamError{"nonstream response has no output array","model.payload.invalid"});
        return events;
    }
    // 非流式条目身上没有 output_index(那是流式帧的字段),编号按数组位置
    // ——与流式路的 output_index 同一语义(条目在 output 里的下标)。
    for (std::size_t i = 0; i < output_it->size(); ++i) {
        const json& item = (*output_it)[i];
        if (!item.is_object()) {
            continue;
        }
        const std::string type = item.value("type", "");
        const int index = static_cast<int>(i);
        if (type == "reasoning") {
            ThinkingDelta event;
            event.responses_item = item;
            const auto content = item.find("content");
            const auto summary = item.find("summary");
            const json* parts = content != item.end() && content->is_array() && !content->empty()
                ? &*content : summary != item.end() && summary->is_array() ? &*summary : nullptr;
            if (parts != nullptr) for (const auto& part : *parts) {
                if (part.is_object() && part.contains("text") && part["text"].is_string())
                    event.text += part["text"].get<std::string>();
            }
            events.push_back(std::move(event));
        } else if (type == "message") {
            if (auto content = item.find("content"); content != item.end() && content->is_array()) {
                for (const auto& part : *content) {
                    if (part.value("type", "") == "output_text") {
                        events.push_back(TextDelta{part.value("text", "")});
                    }
                }
            }
            events.push_back(ContentBlockDone{index});
        } else if (type == "function_call") {
            ToolUseStart start;
            start.index = index;
            start.id = item.value("call_id", "");
            start.name = item.value("name", "");
            // async 位原样保留(异步工具单 §2/§3:OpenAI async tool calling
            // 的回包调用携带 async)。中立层只存不解释;能力 unknown 时宿主
            // 按同步配对,不凭它留悬空调用(单 §4)。
            if (auto async_it = item.find("async"); async_it != item.end() && async_it->is_boolean()) {
                start.async_call = async_it->get<bool>();
            }
            events.push_back(std::move(start));
            const std::string arguments = item.value("arguments", "");
            if (!arguments.empty()) {
                events.push_back(ToolUseInputDelta{index, arguments});
            }
            // 终帧身份随 done 帧走(P0-F):call_id 为空时 assembler 丢弃
            // 该调用,不伪造 id。
            ContentBlockDone done;
            done.index = index;
            done.tool_use_id = item.value("call_id", "");
            events.push_back(std::move(done));
        }
        // 别的条目类型(web_search_call 等)非流式不接线:流式路怎么翻,
        // 这路将来照着补;眼下静默跳过,收尾事件照发。
    }
    auto done=DoneFromResponseObject(response);
    if (!done) events.push_back(StreamError{std::string(done.error()),"model.response.invalid"});
    else {
        if (material) usage_wire::Apply(*done, *material, id ? *id : std::optional<std::string>{});
        events.push_back(std::move(*done));
    }
    } catch (const json::exception&) {
        events.push_back(StreamError{"nonstream response has an invalid payload","model.payload.invalid"});
    }
    return events;
} catch (const json::exception&) {
    // 坏 JSON/坏形状:当"没有 MessageDone 的不完整响应"处理,不抛。
    return {};
}

}  // namespace lubancode::api::responses
