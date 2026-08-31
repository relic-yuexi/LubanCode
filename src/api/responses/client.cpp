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

    // 2xx 响应体 -> 分帧 -> 事件。终止事件/流错误/图片在途三枚标志给收尾
    // 那段检查(图片在途:单帧超限时报错要指名是图片帧超限,不甩含混的
    // 传输错——gpt-image 的 base64 result 一帧就几 MB,是最容易撞
    // SseFramer::kMaxFrameBytes 的载荷)。
    // raw_body 是非流式回退的底稿:还没解出任何 SSE 帧时把原文攒着——
    // 有些兼容端把 stream 请求当非流式答(整只 JSON 对象,一行 data: 都
    // 没有),流式路到收尾会一帧都解不出。真出了帧就不再攒,峰值内存
    // 与分帧器自身的缓冲同量级(vLLM 本地模型勘察单 P2)。
    SseFramer framer;
    bool saw_message_done = false;
    bool saw_stream_error = false;
    bool saw_image_generation = false;
    bool saw_any_frame = false;
    std::string raw_body;
    const StreamDataSink sink = [&](std::string_view data) -> bool {
        if (!saw_any_frame) {
            raw_body.append(data);
        }
        for (const SseFrame& frame : framer.feed(data)) {
            saw_any_frame = true;
            if (auto event = parse_event(frame); event.has_value()) {
                if (std::holds_alternative<MessageDone>(*event)) {
                    saw_message_done = true;
                } else if (std::holds_alternative<StreamError>(*event)) {
                    saw_stream_error = true;
                } else if (const auto* builtin = std::get_if<BuiltinToolStart>(&*event);
                           builtin != nullptr && builtin->name == "image_generation") {
                    saw_image_generation = true;
                } else if (std::holds_alternative<ImageOutput>(*event)) {
                    saw_image_generation = true;
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
        api::Error error = std::move(streamed.error());
        // 图片帧超限的定名(工单 P0):流里已经见过 image_generation_call
        // 却报"SSE 单帧超过大小上限",这只载荷几乎必是图片的 base64 result
        // ——把话说清(图过大、流式收不下),并指路(可换更小尺寸/质量档
        // 重试)。这里不改 8 MiB 的通用帧上限,也不做分段收图:HTTP chunk
        // 里没有可靠的"图片帧边界"约定,分段拼帧等于自己造协议;非流式
        // 下载另开端点改造,不在这一刀。
        if (error.kind == ErrorKind::Parse && saw_image_generation &&
            error.message.find("单帧超过大小上限") != std::string::npos) {
            error.message =
                "图片帧超过 SSE 单帧上限(8 MiB),已断开;这张图过大,流式收不下,可试更小的尺寸或更低的质量档";
        }
        return std::unexpected(std::move(error));
    }

    // 流"正常"走完却没等到 response.completed 翻出来的 MessageDone:响应不
    // 完整,当成功返回会把半截消息(空 stop_reason)当 end_turn 入历史,里头
    // 若有 tool_use,下一轮请求就 400。宁可明确报错。报错前先试一手非流式
    // 回退:兼容端把 stream 请求当非流式答时,响应体是整只 JSON 对象
    // (output 数组逐项),ExpandNonStreamResponse 展得开就当正常回合走。
    if (!saw_message_done && !saw_stream_error) {
        bool fallback_done = false;
        for (const StreamEvent& event : ExpandNonStreamResponse(raw_body)) {
            if (std::holds_alternative<MessageDone>(event)) {
                fallback_done = true;
            }
            on_event(event);
        }
        if (!fallback_done) {
            return std::unexpected(
                Error{ErrorKind::Parse, "流意外结束:未收到消息终止事件,响应不完整", 0});
        }
    }

    return {};
}

// 诊断模式的 wire 序列化(问题 9):与 send_stream 同一条拼装路(清洗 +
// 同参数 BuildRequestJson + 同一只 dump),保证"公共前缀字节"对账量的
// 就是真要上 wire 的字节。只在 LUBANCODE_DEBUG_PREFIX 打开时被调用。
std::string ResponsesBackend::SerializeForDiagnostics(const Request& request) const {
    Request sanitized_request = request;
    SanitizeRequest(sanitized_request);
    return DumpRequestBody("responses", BuildRequestJson(sanitized_request, native_web_search_, extra_body_));
}

}  // namespace lubancode::api::responses
