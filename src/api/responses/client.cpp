#include "api/responses/client.hpp"

#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "api/http_stream_transport.hpp"  // 批六:PostSseStream/DumpRequestBody,四家共用的传输骨架
#include "api/responses/events.hpp"
#include "api/responses/request.hpp"
#include "api/sse_framing.hpp"

namespace lubancode::api::responses {

using nlohmann::json;

std::map<std::string, std::string> ApplyExtraHeaders(std::map<std::string, std::string> base,
                                                        const std::map<std::string, std::string>& extra_headers) {
    // 薄壳:实逻辑在 api/types.hpp 的共用件里(见 client.hpp 注释)。
    return api::ApplyExtraHeaders(std::move(base), extra_headers);
}

ResponsesBackend::ResponsesBackend(std::string base_url, std::string auth_token, int connect_timeout_ms,
                                    int stream_idle_timeout_secs, bool native_web_search,
                                    nlohmann::json extra_body, std::map<std::string, std::string> extra_headers,
                                    int request_hard_timeout_secs)
    : base_url_(std::move(base_url)),
      auth_token_(std::move(auth_token)),
      connect_timeout_ms_(connect_timeout_ms),
      stream_idle_timeout_secs_(stream_idle_timeout_secs),
      native_web_search_(native_web_search),
      extra_body_(std::move(extra_body)),
      extra_headers_(std::move(extra_headers)),
      request_hard_timeout_secs_(request_hard_timeout_secs) {}

std::expected<void, Error> ResponsesBackend::send_stream(
    const Request& request,
    const std::function<void(const StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    Request sanitized_request = request;
    SanitizeRequest(sanitized_request);
    const json body = BuildRequestJson(sanitized_request, native_web_search_, extra_body_);
    const std::string body_str = DumpRequestBody("responses", body);

    // 2xx 响应体 -> 分帧 -> 事件。终止事件/流错误两枚标志给收尾那段检查。
    SseFramer framer;
    bool saw_message_done = false;
    bool saw_stream_error = false;
    const StreamDataSink sink = [&](std::string_view data) -> bool {
        for (const SseFrame& frame : framer.feed(data)) {
            if (auto event = parse_event(frame); event.has_value()) {
                if (std::holds_alternative<MessageDone>(*event)) {
                    saw_message_done = true;
                } else if (std::holds_alternative<StreamError>(*event)) {
                    saw_stream_error = true;
                }
                on_event(*event);
            }
        }
        // 单帧超过上限,协议已不可信:返回 false 让传输层掐断。
        return !framer.overflowed();
    };

    // extra_headers 覆盖/追加到基础头上,同名覆盖(含 Authorization)。鉴权
    // 三态:auth_token 空(无鉴权)时基础头里压根没有 Authorization。
    HttpStreamCall call;
    call.url = base_url_ + "/responses";
    call.headers = ApplyExtraHeaders(RequestBaseHeaders(auth_token_), extra_headers_);
    call.body = body_str;
    call.connect_timeout_ms = connect_timeout_ms_;
    call.stream_idle_timeout_secs = stream_idle_timeout_secs_;
    call.request_hard_timeout_secs = request_hard_timeout_secs_;

    auto streamed = PostSseStream(call, sink, cancel);
    if (!streamed.has_value()) {
        return std::unexpected(std::move(streamed.error()));
    }

    // 流"正常"走完却没等到 response.completed 翻出来的 MessageDone:响应不
    // 完整,当成功返回会把半截消息(空 stop_reason)当 end_turn 入历史,里头
    // 若有 tool_use,下一轮请求就 400。宁可明确报错。
    if (!saw_message_done && !saw_stream_error) {
        return std::unexpected(Error{ErrorKind::Parse, "流意外结束:未收到消息终止事件,响应不完整", 0});
    }

    return {};
}

}  // namespace lubancode::api::responses
