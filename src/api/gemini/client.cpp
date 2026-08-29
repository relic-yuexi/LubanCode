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
    Request sanitized_request = request;
    SanitizeRequest(sanitized_request);
    const nlohmann::json body_json = BuildRequestJson(sanitized_request, extra_body_);
    const std::string body = DumpRequestBody("gemini", body_json);

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

// 诊断模式的 wire 序列化(问题 9):与 send_stream 同一条拼装路(清洗 +
// 同参数 BuildRequestJson + 同一只 dump)。只在 LUBANCODE_DEBUG_PREFIX
// 打开时被调用。
std::string GeminiBackend::SerializeForDiagnostics(const Request& request) const {
    Request sanitized_request = request;
    SanitizeRequest(sanitized_request);
    return DumpRequestBody("gemini", BuildRequestJson(sanitized_request, extra_body_));
}

}  // namespace lubancode::api::gemini
