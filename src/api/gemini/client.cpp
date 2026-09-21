#include "api/gemini/client.hpp"

#include <utility>
#include <variant>

#include "api/gemini/events.hpp"
#include "api/gemini/request.hpp"
#include "api/http_stream_transport.hpp"  // 批六:PostSseStream/DumpRequestBody,四家共用的传输骨架
#include "api/sse_framing.hpp"

namespace lubancode::api::gemini {

namespace {

// Gemini 的基础头:Content-Type + x-goog-api-key。不走 RequestBaseHeaders
// ——那个给的是 Authorization: Bearer,是 OpenAI 兼容三件套的规矩;Gemini
// 的 native 鉴权头只有 x-goog-api-key,token 空时彻底不带,不发空头。
std::map<std::string, std::string> GeminiBaseHeaders(const std::string& auth_token) {
    std::map<std::string, std::string> headers{{"Content-Type", "application/json"}};
    if (!auth_token.empty()) {
        headers["x-goog-api-key"] = auth_token;
    }
    return headers;
}

}  // namespace

GeminiBackend::GeminiBackend(std::string base_url, std::string auth_token, int connect_timeout_ms,
                             int stream_idle_timeout_secs, nlohmann::json extra_body,
                             std::map<std::string, std::string> extra_headers, int request_hard_timeout_secs)
    : base_url_(std::move(base_url)),
      auth_token_(std::move(auth_token)),
      connect_timeout_ms_(connect_timeout_ms),
      stream_idle_timeout_secs_(stream_idle_timeout_secs),
      extra_body_(std::move(extra_body)),
      extra_headers_(std::move(extra_headers)),
      request_hard_timeout_secs_(request_hard_timeout_secs) {}

std::expected<void, Error> GeminiBackend::send_stream(
    const Request& request, const std::function<void(const StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    // FD-02:出门体从最终出站状态取——发送字节、有效上限、映射同源,
    // 不再另拼一份。
    const std::string body = DumpRequestBody("gemini", PrepareWireRequest(request).body);

    // 2xx 响应体 -> 分帧 -> 事件。终止事件/流错误两枚标志给收尾那段检查。
    SseFramer framer;
    EventParser parser;
    bool saw_message_done = false;
    bool saw_stream_error = false;
    const auto dispatch = [&](const std::vector<StreamEvent>& events) {
        for (const auto& event : events) {
            saw_message_done = saw_message_done || std::holds_alternative<MessageDone>(event);
            saw_stream_error = saw_stream_error || std::holds_alternative<StreamError>(event);
            on_event(event);
        }
    };
    const StreamDataSink sink = [&](std::string_view chunk) -> bool {
        for (const auto& frame : framer.feed(chunk)) {
            dispatch(parser.Consume(frame));
        }
        // 单帧超过上限,协议已不可信:返回 false 让传输层掐断。
        return !framer.overflowed();
    };

    // extra_headers 覆盖/追加到基础头上,同名覆盖(含 x-goog-api-key)。
    HttpStreamCall call;
    call.url = StreamUrl(base_url_, request.model);
    call.headers = ApplyExtraHeaders(GeminiBaseHeaders(auth_token_), extra_headers_);
    call.body = body;
    call.connect_timeout_ms = connect_timeout_ms_;
    call.stream_idle_timeout_secs = stream_idle_timeout_secs_;
    call.request_hard_timeout_secs = request_hard_timeout_secs_;

    auto streamed = PostSseStream(call, sink, cancel);
    if (!streamed.has_value()) {
        return std::unexpected(std::move(streamed.error()));
    }

    if (!parser.finished()) dispatch(parser.Finish());
    if (!saw_message_done && !saw_stream_error) {
        return std::unexpected(Error{ErrorKind::Parse, "流意外结束:未收到可用的 Generate Content 响应", 0});
    }
    return {};
}

// 诊断模式的 wire 序列化(问题 9):与 send_stream 同一条拼装路
//(PrepareWireRequest)。只在 LUBANCODE_DEBUG_PREFIX 打开时被调用。
std::string GeminiBackend::SerializeForDiagnostics(const Request& request) const {
    return DumpRequestBody("gemini", PrepareWireRequest(request).body);
}

namespace {

// gemini 的输出上限键深一层:最终 body 的 generationConfig.
// maxOutputTokens。generationConfig 非 object 或键不在场 → nullopt;
// 非整数(null/字符串/浮点,只会来自 extra_body 手笔)→ 同样 nullopt
//(unknown,不回退旧值)——口径与中立层 IntLimitFromBody 一致,只是
// 这只管深一层的自家键。
std::optional<int> MaxOutputTokensFromBody(const nlohmann::json& body) {
    if (!body.is_object()) {
        return std::nullopt;
    }
    const auto config = body.find("generationConfig");
    if (config == body.end() || !config->is_object()) {
        return std::nullopt;
    }
    const auto sub = config->find("maxOutputTokens");
    if (sub == config->end() || !sub->is_number_integer()) {
        return std::nullopt;
    }
    const std::int64_t value = sub->get<std::int64_t>();
    constexpr std::int64_t kIntMax = 2147483647;
    if (value < 0) {
        return 0;
    }
    return static_cast<int>(value > kIntMax ? kIntMax : value);
}

// extra_body 两级里有没有手笔碰过 generationConfig.maxOutputTokens:
// 任一级的 generationConfig(object)写了 maxOutputTokens 键(值类型
// 不限),或任一级把 generationConfig 整块写成了非 object(顶层整块
// 覆盖会把内置 maxOutputTokens 冲出门外)——两种都算覆盖在场。
// generationConfig(object)不带 maxOutputTokens 的深合并不碰这只键,
// 不算(出门值原样,是内置手笔)。
bool MaxOutputTokensOverridePresent(const nlohmann::json& provider_extra,
                                    const nlohmann::json& request_extra) {
    const auto touches = [](const nlohmann::json& source) {
        if (!source.is_object()) {
            return false;
        }
        const auto it = source.find("generationConfig");
        if (it == source.end()) {
            return false;
        }
        return !it->is_object() || it->contains("maxOutputTokens");
    };
    return touches(provider_extra) || touches(request_extra);
}

}  // namespace

// FD-02:最终出站状态一次拼成——清洗、拼装(带映射指针)、extra_body
// 尾部合并(generationConfig 深一层特例在内)、上限解析全在这条路上。
// send_stream 与三口诊断/预算/映射都从这份结果取数,同一请求的出站
// 事实只有这一个来源。
PreparedWireRequest GeminiBackend::PrepareWireRequest(const Request& request) const {
    Request sanitized_request = request;
    SanitizeRequest(sanitized_request);
    PreparedWireRequest prepared;
    WireMessageMap map;
    prepared.body = BuildRequestJson(sanitized_request, extra_body_, &map);
    // 容器被 extra_body 覆盖时 BuildRequestJson 把 map 置空(container 空
    // 串)——不可得,不冒充。
    if (!map.container.empty()) {
        prepared.wire_map = std::move(map);
    }
    prepared.output_limit = MaxOutputTokensFromBody(prepared.body);
    prepared.output_limit_overridden =
        MaxOutputTokensOverridePresent(extra_body_, sanitized_request.extra_body);
    return prepared;
}

// 拍平对照(差距清单 §8.2 第 7 条):边界账对账用,从最终出站状态取
//(容器被 extra_body 覆盖时如实 nullopt,不指替换前的旧数组)。
std::optional<WireMessageMap> GeminiBackend::BuildWireMessageMap(const Request& request) const {
    return PrepareWireRequest(request).wire_map;
}

// 差距清单 §8.2 第 8 条(FD-02 收敛后):上限从最终 body 的
// generationConfig.maxOutputTokens 解析,unset 交服务端默认,如实
// nullopt;覆盖成了非整数时 unknown,不回退旧值。
Backend::EffectiveOutputLimit GeminiBackend::GetEffectiveOutputLimit(const Request& request) const {
    const PreparedWireRequest prepared = PrepareWireRequest(request);
    return {prepared.output_limit, prepared.output_limit_overridden};
}

// 差距清单 §8.2 第 8 条写侧:覆盖在场(值类型不限)才动请求级覆盖位;
// 写进请求级 generationConfig.maxOutputTokens(合并序最后的深合并层,
// 压过 provider 级),没写过不造键。
void GeminiBackend::ForceMaxOutputTokensOverride(Request& request, int tokens) const {
    if (MaxOutputTokensOverridePresent(extra_body_, request.extra_body)) {
        if (!request.extra_body.contains("generationConfig") ||
            !request.extra_body["generationConfig"].is_object()) {
            request.extra_body["generationConfig"] = nlohmann::json::object();
        }
        request.extra_body["generationConfig"]["maxOutputTokens"] = tokens;
    }
}

}  // namespace lubancode::api::gemini
