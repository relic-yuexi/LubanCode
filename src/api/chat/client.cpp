#include "api/chat/client.hpp"

#include <utility>
#include <variant>

#include "api/chat/events.hpp"
#include "api/chat/request.hpp"
#include "api/http_stream_transport.hpp"  // 批六:PostSseStream/DumpRequestBody,四家共用的传输骨架
#include "api/sse_framing.hpp"

namespace lubancode::api::chat {

ChatCompletionsBackend::ChatCompletionsBackend(std::string base_url, std::string auth_token,
                                               int connect_timeout_ms, int stream_idle_timeout_secs,
                                               nlohmann::json extra_body,
                                               std::map<std::string, std::string> extra_headers,
                                               ChatRequestOptions options, int request_hard_timeout_secs)
    : base_url_(std::move(base_url)),
      auth_token_(std::move(auth_token)),
      connect_timeout_ms_(connect_timeout_ms),
      stream_idle_timeout_secs_(stream_idle_timeout_secs),
      extra_body_(std::move(extra_body)),
      extra_headers_(std::move(extra_headers)),
      options_(std::move(options)),
      request_hard_timeout_secs_(request_hard_timeout_secs) {}

std::expected<void, Error> ChatCompletionsBackend::send_stream(
    const Request& request, const std::function<void(const StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    // FD-02:出门体从最终出站状态取——发送字节、有效上限、映射同源,
    // 不再另拼一份。
    const std::string body = DumpRequestBody("chat", PrepareWireRequest(request).body);

    // 2xx 响应体 -> 分帧 -> 事件。终止事件/流错误两枚标志给收尾那段检查。
    SseFramer framer;
    EventParser parser{options_.reasoning_delta_field};
    bool saw_done = false;
    bool saw_stream_error = false;
    const auto dispatch = [&](const std::vector<StreamEvent>& events) {
        for (const auto& event : events) {
            saw_done = saw_done || std::holds_alternative<MessageDone>(event);
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

    // 鉴权三态:auth_token 空(无鉴权)时基础头里压根没有 Authorization,
    // 不发空 Bearer(与 anthropic/responses/ListModels 同一份规矩)。
    HttpStreamCall call;
    call.url = base_url_ + "/chat/completions";
    call.headers = ApplyExtraHeaders(RequestBaseHeaders(auth_token_), extra_headers_);
    call.body = body;
    call.connect_timeout_ms = connect_timeout_ms_;
    call.stream_idle_timeout_secs = stream_idle_timeout_secs_;
    call.request_hard_timeout_secs = request_hard_timeout_secs_;

    auto streamed = PostSseStream(call, sink, cancel);
    if (!streamed.has_value()) {
        return std::unexpected(std::move(streamed.error()));
    }

    if (!parser.finished()) dispatch(parser.Finish());
    if (!saw_done && !saw_stream_error) {
        return std::unexpected(Error{ErrorKind::Parse, "流意外结束:未收到可用的 Chat Completions 响应", 0});
    }
    return {};
}

// FD-02:最终出站状态一次拼成——清洗、拼装(带映射指针)、extra_body
// 尾部合并、上限解析全在这条路上。send_stream 与三口诊断/预算/映射都
// 从这份结果取数,同一请求的出站事实只有这一个来源。
PreparedWireRequest ChatCompletionsBackend::PrepareWireRequest(const Request& request) const {
    Request sanitized_request = request;
    SanitizeRequest(sanitized_request);
    PreparedWireRequest prepared;
    WireMessageMap map;
    prepared.body = BuildRequestJson(sanitized_request, extra_body_, options_, &map);
    // 容器被 extra_body 覆盖时 BuildRequestJson 把 map 置空(container 空
    // 串)——不可得,不冒充。
    if (!map.container.empty()) {
        prepared.wire_map = std::move(map);
    }
    prepared.output_limit = IntLimitFromBody(prepared.body, "max_tokens");
    prepared.output_limit_overridden = ExtraBodyHasKey(extra_body_, sanitized_request.extra_body, "max_tokens");
    return prepared;
}

// 诊断模式的 wire 序列化(问题 9):与 send_stream 同一条拼装路
//(PrepareWireRequest)。只在 LUBANCODE_DEBUG_PREFIX 打开时被调用,默认
// 不序列化。
std::string ChatCompletionsBackend::SerializeForDiagnostics(const Request& request) const {
    return DumpRequestBody("chat", PrepareWireRequest(request).body);
}

// 拍平对照(差距清单 §8.2 第 7 条):边界账对账用,从最终出站状态取
//(容器被 extra_body 覆盖时如实 nullopt,不指替换前的旧数组)。
std::optional<WireMessageMap> ChatCompletionsBackend::BuildWireMessageMap(const Request& request) const {
    return PrepareWireRequest(request).wire_map;
}

// 差距清单 §8.2 第 8 条(FD-02 收敛后):上限从最终 body 上解析——
// chat 的 max_tokens 可省略,没有覆盖时如实 nullopt(交服务端默认),
// 不落兜底;覆盖成了非整数(null/字符串)时 unknown,不回退旧值。
Backend::EffectiveOutputLimit ChatCompletionsBackend::GetEffectiveOutputLimit(const Request& request) const {
    const PreparedWireRequest prepared = PrepareWireRequest(request);
    return {prepared.output_limit, prepared.output_limit_overridden};
}

// 差距清单 §8.2 第 8 条写侧:extra_body 写过 max_tokens(值类型不限——
// 整数、null、字符串都算覆盖在场)才动请求级键,没写过不造键,出口
// 形状与从前逐字节一致。
void ChatCompletionsBackend::ForceMaxOutputTokensOverride(Request& request, int tokens) const {
    if (ExtraBodyHasKey(extra_body_, request.extra_body, "max_tokens")) {
        request.extra_body["max_tokens"] = tokens;
    }
}

}  // namespace lubancode::api::chat
