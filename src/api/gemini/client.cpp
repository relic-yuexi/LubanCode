#include "api/gemini/client.hpp"

#include <charconv>
#include <chrono>
#include <string_view>
#include <utility>
#include <variant>

#include <cpr/cpr.h>

#include "api/gemini/events.hpp"
#include "api/gemini/request.hpp"
#include "api/sse_framing.hpp"
#include "cli/i18n.hpp"
#include "platform/json_safe.hpp"  // DescribeDumpFailure:请求体 dump 的窄边界
#include "platform/log_sink.hpp"

namespace lubancode::api::gemini {

namespace {

// 从 HTTP 状态行里抠状态码;抠不出来返回 0。(与 chat/responses/anthropic
// 三个 client 里的同名函数一个逻辑,各自小巧,不为共用几行代码另起公共头。)
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

// 把 cpr 的网络错误翻成人话,分型与三个既有 client 同一套。
std::string ClassifyNetworkError(const cpr::Error& error, bool received_any_bytes, int connect_timeout_ms,
                                 int stream_idle_timeout_secs, bool hard_timeout_hit, int hard_timeout_secs) {
    if (hard_timeout_hit) {
        return cli::trf("error.network.hard_timeout", hard_timeout_secs);
    }
    if (error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
        return received_any_bytes ? cli::trf("error.network.stream_idle_timeout", stream_idle_timeout_secs)
                                  : cli::trf("error.network.connect_timeout", connect_timeout_ms / 1000);
    }
    if (error.code == cpr::ErrorCode::COULDNT_CONNECT || error.code == cpr::ErrorCode::COULDNT_RESOLVE_HOST ||
        error.code == cpr::ErrorCode::COULDNT_RESOLVE_PROXY) {
        return cli::trf("error.network.connect_failed", error.message);
    }
    return error.message;
}

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
    std::string body;
    try {
        body = body_json.dump();
    } catch (const nlohmann::json::exception& e) {
        // 请求序列化的最后兜底:历史里漏网的非法 UTF-8 不掐回合,响亮记一笔
        // 日志,坏串按 U+FFFD 清洗后照发(与 chat/responses 同一政策)。
        platform::LogSink::Instance().Warn("gemini",
            platform::DescribeDumpFailure(body_json, e) + " -> 已按 U+FFFD 清洗后发出");
        body = platform::DumpJsonSanitized(body_json);
    }

    SseFramer framer;
    EventParser parser;
    std::string error_body;
    int status_code = 0;
    bool status_known = false;
    bool cancelled = false;
    bool frame_overflow = false;
    bool saw_message_done = false;
    bool saw_stream_error = false;
    bool received_any_bytes = false;
    bool hard_timeout_hit = false;

    const auto dispatch = [&](const std::vector<StreamEvent>& events) {
        for (const auto& event : events) {
            saw_message_done = saw_message_done || std::holds_alternative<MessageDone>(event);
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
        received_any_bytes = true;
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
            frame_overflow = true;
            return false;
        }
        return true;
    });
    // 硬墙钟(cpr 并发挂死单)挂在 ProgressCallback,理由与考证见
    // anthropic/client.cpp 同一处注释——libcurl 周期性调它,连接死寂也醒;
    // 不用 cpr::Timeout,那会把正常长流拦腰砍断。
    const bool hard_wall_on = request_hard_timeout_secs_ > 0;
    const auto hard_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(request_hard_timeout_secs_);
    cpr::ProgressCallback progress_cb(
        [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) {
            if (cancel != nullptr && cancel->load()) {
                cancelled = true;
                return false;
            }
            if (hard_wall_on && std::chrono::steady_clock::now() >= hard_deadline) {
                hard_timeout_hit = true;
                return false;
            }
            return true;
        });

    // extra_headers 覆盖/追加到基础头上,同名覆盖(含 x-goog-api-key)。
    std::map<std::string, std::string> headers = GeminiBaseHeaders(auth_token_);
    for (const auto& [name, value] : extra_headers_) {
        if (value.empty()) headers.erase(name);
        else headers[name] = value;
    }
    cpr::Header cpr_headers;
    for (const auto& [name, value] : headers) cpr_headers[name] = value;

    const cpr::Response response = cpr::Post(
        cpr::Url{StreamUrl(base_url_, request.model)}, cpr_headers, cpr::Body{body},
        cpr::ConnectTimeout{std::chrono::milliseconds(connect_timeout_ms_)},
        #if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // 新版 cpr 弃用 int 构造改 chrono;vendored 1.11 只有 int 形,值两边通用
#endif
        cpr::LowSpeed{1, stream_idle_timeout_secs_}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
, header_cb, write_cb, progress_cb);

    if (cancelled || (cancel != nullptr && cancel->load())) {
        return std::unexpected(Error{ErrorKind::Cancelled, "用户按 ESC 打断了这次请求", 0});
    }
    if (frame_overflow) {
        return std::unexpected(Error{ErrorKind::Parse, "SSE 单帧超过大小上限(8MB),协议错误,已断开", 0});
    }
    if (response.error) {
        return std::unexpected(Error{ErrorKind::Network,
                                     ClassifyNetworkError(response.error, received_any_bytes,
                                                          connect_timeout_ms_, stream_idle_timeout_secs_,
                                                          hard_timeout_hit, request_hard_timeout_secs_),
                                     0});
    }
    const int final_status = static_cast<int>(response.status_code);
    if (final_status < 200 || final_status >= 300) {
        std::string message = !error_body.empty() ? error_body : response.text;
        if (message.empty()) message = "服务端返回了非 200 状态码,但响应体是空的";
        return std::unexpected(Error{ErrorKind::HttpStatus, std::move(message), final_status});
    }

    if (!parser.finished()) dispatch(parser.Finish());
    if (!saw_message_done && !saw_stream_error) {
        return std::unexpected(Error{ErrorKind::Parse, "流意外结束:未收到可用的 Generate Content 响应", 0});
    }
    return {};
}

}  // namespace lubancode::api::gemini
