#include "api/responses/client.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <utility>
#include <variant>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "api/responses/events.hpp"
#include "api/responses/request.hpp"
#include "api/sse_framing.hpp"

namespace lubancode::api::responses {

namespace {

using nlohmann::json;

// 从 HTTP 状态行(形如 "HTTP/1.1 200 OK")里抠出状态码。抠不出来返回 0。
// (跟 anthropic/client.cpp 里的同名函数逻辑一样,两边各自小巧,没必要
// 为了共用几行代码搭一个新的公共头。)
int ExtractStatusCode(std::string_view header_line) {
    if (header_line.rfind("HTTP/", 0) != 0) {
        return 0;
    }
    const std::size_t space1 = header_line.find(' ');
    if (space1 == std::string_view::npos) {
        return 0;
    }
    const std::size_t space2 = header_line.find(' ', space1 + 1);
    const std::string_view code_sv = header_line.substr(space1 + 1, space2 == std::string_view::npos ? std::string_view::npos : space2 - space1 - 1);
    int code = 0;
    const auto result = std::from_chars(code_sv.data(), code_sv.data() + code_sv.size(), code);
    if (result.ec != std::errc{}) {
        return 0;
    }
    return code;
}

}  // namespace

ResponsesBackend::ResponsesBackend(std::string base_url, std::string auth_token)
    : base_url_(std::move(base_url)), auth_token_(std::move(auth_token)) {}

std::expected<void, Error> ResponsesBackend::send_stream(
    const Request& request,
    const std::function<void(const StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    const json body = BuildRequestJson(request);
    const std::string body_str = body.dump();

    SseFramer framer;
    std::string error_body;
    int status_code = 0;
    bool status_known = false;
    bool cancelled = false;
    bool frame_overflow = false;
    bool saw_message_done = false;
    bool saw_stream_error = false;

    cpr::HeaderCallback header_cb(
        [&](const std::string_view& header, intptr_t) -> bool {
            if (const int code = ExtractStatusCode(header); code != 0) {
                status_code = code;
                status_known = true;
            }
            return true;
        });

    cpr::WriteCallback write_cb(
        [&](const std::string_view& data, intptr_t) -> bool {
            if (cancel != nullptr && cancel->load()) {
                cancelled = true;
                return false;
            }
            const bool is_success = status_known && status_code >= 200 && status_code < 300;
            if (!is_success) {
                // 非 2xx:这不是 SSE 流,是普通的错误响应体,原样攒起来
                // 好塞进 Error 里,不要喂给分帧器瞎解析。
                error_body.append(data);
                return true;
            }
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
            if (framer.overflowed()) {
                // 单帧超过上限,协议已不可信:掐断传输,后面按协议错误报。
                frame_overflow = true;
                return false;
            }
            return true;
        });

    // ProgressCallback 在连接/TLS 握手阶段(还没有响应体)也会被周期性调用,
    // 返回 false 即中止——WriteCallback 只有数据到达才触发,光靠它,ESC 在
    // 连不上的服务器面前毫无办法。总超时不设死(流式回复可以很长),连接
    // 阶段单独给 15s 上限。
    cpr::ProgressCallback progress_cb(
        [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) -> bool {
            if (cancel != nullptr && cancel->load()) {
                cancelled = true;
                return false;
            }
            return true;
        });

    const std::string url = base_url_ + "/responses";

    cpr::Response response = cpr::Post(
        cpr::Url{url},
        cpr::Header{
            {"Content-Type", "application/json"},
            {"Authorization", "Bearer " + auth_token_},
        },
        cpr::Body{body_str},
        cpr::ConnectTimeout{std::chrono::milliseconds(15000)},
        header_cb,
        write_cb,
        progress_cb);

    if (cancelled || (cancel != nullptr && cancel->load())) {
        return std::unexpected(Error{ErrorKind::Cancelled, "用户按 ESC 打断了这次请求", 0});
    }

    if (frame_overflow) {
        return std::unexpected(Error{ErrorKind::Parse, "SSE 单帧超过大小上限(8MB),协议错误,已断开", 0});
    }

    if (response.error) {
        return std::unexpected(Error{ErrorKind::Network, response.error.message, 0});
    }

    const int final_status = static_cast<int>(response.status_code);
    if (final_status < 200 || final_status >= 300) {
        std::string message = !error_body.empty() ? error_body : response.text;
        if (message.empty()) {
            message = "服务端返回了非 200 状态码,但响应体是空的";
        }
        return std::unexpected(Error{ErrorKind::HttpStatus, std::move(message), final_status});
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
