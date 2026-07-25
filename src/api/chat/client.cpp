#include "api/chat/client.hpp"

#include <charconv>
#include <chrono>
#include <string_view>
#include <utility>
#include <variant>

#include <cpr/cpr.h>

#include "api/chat/events.hpp"
#include "api/chat/request.hpp"
#include "api/sse_framing.hpp"
#include "cli/i18n.hpp"

namespace lubancode::api::chat {

namespace {

int ExtractStatusCode(std::string_view line) {
    if (line.rfind("HTTP/", 0) != 0) return 0;
    const std::size_t first = line.find(' ');
    if (first == std::string_view::npos) return 0;
    const std::size_t second = line.find(' ', first + 1);
    const std::string_view raw = line.substr(first + 1, second == std::string_view::npos ? std::string_view::npos
                                                                                         : second - first - 1);
    int code = 0;
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), code);
    return parsed.ec == std::errc{} ? code : 0;
}

std::string NetworkError(const cpr::Error& error, bool received, int connect_ms, int idle_secs) {
    if (error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
        return received ? cli::trf("error.network.stream_idle_timeout", idle_secs)
                        : cli::trf("error.network.connect_timeout", connect_ms / 1000);
    }
    if (error.code == cpr::ErrorCode::COULDNT_CONNECT || error.code == cpr::ErrorCode::COULDNT_RESOLVE_HOST ||
        error.code == cpr::ErrorCode::COULDNT_RESOLVE_PROXY) {
        return cli::trf("error.network.connect_failed", error.message);
    }
    return error.message;
}

}  // namespace

ChatCompletionsBackend::ChatCompletionsBackend(std::string base_url, std::string auth_token,
                                               int connect_timeout_ms, int stream_idle_timeout_secs,
                                               nlohmann::json extra_body,
                                               std::map<std::string, std::string> extra_headers)
    : base_url_(std::move(base_url)),
      auth_token_(std::move(auth_token)),
      connect_timeout_ms_(connect_timeout_ms),
      stream_idle_timeout_secs_(stream_idle_timeout_secs),
      extra_body_(std::move(extra_body)),
      extra_headers_(std::move(extra_headers)) {}

std::expected<void, Error> ChatCompletionsBackend::send_stream(
    const Request& request, const std::function<void(const StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    const std::string body = BuildRequestJson(request, extra_body_).dump();
    SseFramer framer;
    EventParser parser;
    std::string error_body;
    int status_code = 0;
    bool status_known = false;
    bool cancelled = false;
    bool overflow = false;
    bool received = false;
    bool saw_done = false;
    bool saw_stream_error = false;

    const auto dispatch = [&](const std::vector<StreamEvent>& events) {
        for (const auto& event : events) {
            saw_done = saw_done || std::holds_alternative<MessageDone>(event);
            saw_stream_error = saw_stream_error || std::holds_alternative<StreamError>(event);
            on_event(event);
        }
    };

    cpr::HeaderCallback header_cb([&](const std::string_view& header, intptr_t) {
        if (const int code = ExtractStatusCode(header); code != 0) {
            status_code = code;
            status_known = true;
        }
        return true;
    });
    cpr::WriteCallback write_cb([&](const std::string_view& chunk, intptr_t) {
        received = true;
        if (cancel != nullptr && cancel->load()) {
            cancelled = true;
            return false;
        }
        if (!(status_known && status_code >= 200 && status_code < 300)) {
            error_body.append(chunk);
            return true;
        }
        for (const auto& frame : framer.feed(chunk)) {
            dispatch(parser.Consume(frame));
        }
        if (framer.overflowed()) {
            overflow = true;
            return false;
        }
        return true;
    });
    cpr::ProgressCallback progress_cb(
        [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) {
            if (cancel != nullptr && cancel->load()) {
                cancelled = true;
                return false;
            }
            return true;
        });

    std::map<std::string, std::string> headers{{"Content-Type", "application/json"},
                                               {"Authorization", "Bearer " + auth_token_}};
    for (const auto& [name, value] : extra_headers_) {
        if (value.empty()) headers.erase(name);
        else headers[name] = value;
    }
    cpr::Header cpr_headers;
    for (const auto& [name, value] : headers) cpr_headers[name] = value;

    const cpr::Response response = cpr::Post(
        cpr::Url{base_url_ + "/chat/completions"}, cpr_headers, cpr::Body{body},
        cpr::ConnectTimeout{std::chrono::milliseconds(connect_timeout_ms_)},
        cpr::LowSpeed{1, stream_idle_timeout_secs_}, header_cb, write_cb, progress_cb);

    if (cancelled || (cancel != nullptr && cancel->load())) {
        return std::unexpected(Error{ErrorKind::Cancelled, "用户按 ESC 打断了这次请求", 0});
    }
    if (overflow) {
        return std::unexpected(Error{ErrorKind::Parse, "SSE 单帧超过大小上限(8MB),协议错误,已断开", 0});
    }
    if (response.error) {
        return std::unexpected(Error{ErrorKind::Network,
                                     NetworkError(response.error, received, connect_timeout_ms_,
                                                  stream_idle_timeout_secs_),
                                     0});
    }
    const int final_status = static_cast<int>(response.status_code);
    if (final_status < 200 || final_status >= 300) {
        std::string message = !error_body.empty() ? error_body : response.text;
        if (message.empty()) message = "服务端返回了非 200 状态码,但响应体是空的";
        return std::unexpected(Error{ErrorKind::HttpStatus, std::move(message), final_status});
    }

    if (!parser.finished()) dispatch(parser.Finish());
    if (!saw_done && !saw_stream_error) {
        return std::unexpected(Error{ErrorKind::Parse, "流意外结束:未收到可用的 Chat Completions 响应", 0});
    }
    return {};
}

}  // namespace lubancode::api::chat
