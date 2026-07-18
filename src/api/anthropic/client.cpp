#include "api/anthropic/client.hpp"

#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <iostream>
#include <optional>
#include <type_traits>
#include <utility>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "api/anthropic/events.hpp"
#include "api/sse_framing.hpp"
#include "cli/i18n.hpp"

namespace lubancode::api::anthropic {

using nlohmann::json;

namespace {

std::string RoleToString(Role role) {
    return role == Role::User ? "user" : "assistant";
}

json ContentBlockToJson(const ContentBlock& block) {
    return std::visit(
        [](const auto& b) -> json {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, TextBlock>) {
                return json{{"type", "text"}, {"text", b.text}};
            } else if constexpr (std::is_same_v<T, ImageBlock>) {
                return json{{"type", "image"},
                            {"source", json{{"type", "base64"}, {"media_type", b.media_type}, {"data", b.data}}}};
            } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                return json{{"type", "tool_use"}, {"id", b.id}, {"name", b.name}, {"input", b.input}};
            } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                json j{{"type", "tool_result"}, {"tool_use_id", b.tool_use_id}, {"content", b.content}};
                if (b.is_error) {
                    j["is_error"] = true;
                }
                return j;
            }
        },
        block);
}

// M6.6/M10:think 档位 -> Anthropic 风格 thinking 参数。实测(MiniMax-M3 真实
// anthropic 兼容端点 /anthropic/v1/messages)确认支持
// {"type":"enabled","budget_tokens":N} / {"type":"disabled"},HTTP 200,
// 且 enabled 时真的返回 thinking 内容块——所以这里走"接受并映射"这条路,
// 不是"协议不支持,打警告跳过"那条路。
// M10:config/命令行层不再限死档位取值(responses wire 原样透传任意字符串,
// 由服务商自己判断合不合法);但 anthropic wire 走的是自家的 thinking 参数,
// 只认 budget_tokens 这个数字,没法"原样透传"一个任意字符串,所以内置一张
// 映射表:none/low/medium/high/xhigh/max。档位 -> budget_tokens 的具体数字
// 是任务明确交给这里自己定的一个设计选择(原话"档位→budget 映射你定"),
// 选的是 1k/4k/16k/32k/48k 这组常见量级,越往上思考预算越宽裕;唯一的硬
// 约束是 Anthropic 要求 budget_tokens 必须小于 max_tokens(不然思考预算比
// 整个回复上限还大,没意义、也可能被端点拒绝),所以这里按
// request.max_tokens 兜底夹一下,budget 超过 max_tokens 时退化成
// "max_tokens 留 256 给正文,剩下全给思考",绝不出现 budget >= max_tokens
// 的组合。映射表之外的档位名字(比如用户在 responses wire 下用惯了、后来
// 切到 anthropic wire 的自定义字符串):不报错、不崩,只打一行警告到
// stderr,当没设置处理(不发 thinking 字段),留给上层继续跑完这一轮。
std::optional<json> BuildThinkingJson(const Request& request) {
    if (request.reasoning_effort.empty()) {
        return std::nullopt;
    }
    std::string lower = request.reasoning_effort;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower == "none") {
        return json{{"type", "disabled"}};
    }

    int budget = 0;
    if (lower == "low") {
        budget = 1024;    // 1k
    } else if (lower == "medium") {
        budget = 4096;    // 4k
    } else if (lower == "high") {
        budget = 16384;   // 16k
    } else if (lower == "xhigh") {
        budget = 32768;   // 32k
    } else if (lower == "max") {
        budget = 49152;   // 48k
    } else {
        std::cerr << "[警告] anthropic 协议下无此档映射,已忽略: " << request.reasoning_effort << "\n";
        return std::nullopt;
    }

    if (budget >= request.max_tokens) {
        budget = request.max_tokens > 256 ? request.max_tokens - 256 : request.max_tokens / 2;
        if (budget < 1) {
            budget = 1;
        }
    }

    return json{{"type", "enabled"}, {"budget_tokens", budget}};
}

}  // namespace

// 拼出 Anthropic Messages API 的请求体(stream: true 恒开,M1 只走流式)。
// 声明在 client.hpp 里,单测用;线上代码路径(send_stream)也是调这个函数。
json BuildRequestJson(const Request& request) {
    json body;
    body["model"] = request.model;
    body["max_tokens"] = request.max_tokens;
    body["stream"] = true;

    if (!request.system.empty()) {
        body["system"] = request.system;
    }

    if (const auto thinking = BuildThinkingJson(request); thinking.has_value()) {
        body["thinking"] = *thinking;
    }

    json messages = json::array();
    for (const auto& message : request.messages) {
        json content = json::array();
        for (const auto& block : message.content) {
            content.push_back(ContentBlockToJson(block));
        }
        messages.push_back(json{{"role", RoleToString(message.role)}, {"content", content}});
    }
    body["messages"] = messages;

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto& tool : request.tools) {
            tools.push_back(json{
                {"name", tool.name},
                {"description", tool.description},
                {"input_schema", tool.input_schema},
            });
        }
        body["tools"] = tools;
    }

    return body;
}

namespace {

// 从 HTTP 状态行(形如 "HTTP/1.1 200 OK")里抠出状态码。抠不出来返回 0。
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

// 把 cpr 的网络错误翻成人话。OPERATION_TIMEDOUT 是 CURLOPT_CONNECTTIMEOUT
// 和 CURLOPT_LOW_SPEED_TIME/LIMIT 共用的错误码(libcurl 底层就是同一个
// CURLE_OPERATION_TIMEDOUT),没法直接从错误码分清"连接没建立起来"还是
// "连上了半路断流"——靠 received_any_bytes(有没有收到过响应体的第一个
// 字节)这个旁证区分:一个字节都没收到,大概率是连接/握手阶段卡死;收到
// 过数据又停了,是流中途假死。COULDNT_CONNECT/COULDNT_RESOLVE_HOST/
// COULDNT_RESOLVE_PROXY 是明确的"连不上"类错误,原始 curl 错误信息拼进去
// 不丢细节。别的错误码原样透传 curl 的 message(留着排查用,不做过度包装)。
std::string ClassifyNetworkError(const cpr::Error& error, bool received_any_bytes, int connect_timeout_ms,
                                  int stream_idle_timeout_secs) {
    if (error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
        if (received_any_bytes) {
            return cli::trf("error.network.stream_idle_timeout", stream_idle_timeout_secs);
        }
        return cli::trf("error.network.connect_timeout", connect_timeout_ms / 1000);
    }
    if (error.code == cpr::ErrorCode::COULDNT_CONNECT || error.code == cpr::ErrorCode::COULDNT_RESOLVE_HOST ||
        error.code == cpr::ErrorCode::COULDNT_RESOLVE_PROXY) {
        return cli::trf("error.network.connect_failed", error.message);
    }
    return error.message;
}

}  // namespace

AnthropicBackend::AnthropicBackend(std::string base_url, std::string auth_token, int connect_timeout_ms,
                                    int stream_idle_timeout_secs)
    : base_url_(std::move(base_url)),
      auth_token_(std::move(auth_token)),
      connect_timeout_ms_(connect_timeout_ms),
      stream_idle_timeout_secs_(stream_idle_timeout_secs) {}

std::expected<void, Error> AnthropicBackend::send_stream(
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
    // M11:有没有收到过响应体的第一个字节——网络错误发生时靠这个旁证分清
    // 是连接阶段就卡死了,还是流中途假死(见 ClassifyNetworkError 注释)。
    bool received_any_bytes = false;

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
            received_any_bytes = true;
            if (cancel != nullptr && cancel->load()) {
                // 返回 false 让 cpr/libcurl 就地掐断这次传输;response.error
                // 会因此被置位,靠上面这个 cancelled 标志把"用户主动打断"
                // 和"真网络错"分开,不走到 ErrorKind::Network 那条报错路。
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
    // 阶段单独给 connect_timeout_ms_ 上限(M11:可配,默认 15s)。
    cpr::ProgressCallback progress_cb(
        [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) -> bool {
            if (cancel != nullptr && cancel->load()) {
                cancelled = true;
                return false;
            }
            return true;
        });

    const std::string url = base_url_ + "/v1/messages";

    // M11:LowSpeed{1, stream_idle_timeout_secs_} 是"空闲读超时",不是总时长
    // 上限——libcurl 语义是"持续 stream_idle_timeout_secs_ 秒平均速率低于
    // 1 字节/秒就判超时",拿它当"连续 N 秒一个字节没收到"的等价检测(流式
    // 回答本身可以很长,故意不设总 Timeout)。
    cpr::Response response = cpr::Post(
        cpr::Url{url},
        cpr::Header{
            {"Content-Type", "application/json"},
            {"Authorization", "Bearer " + auth_token_},
        },
        cpr::Body{body_str},
        cpr::ConnectTimeout{std::chrono::milliseconds(connect_timeout_ms_)},
        cpr::LowSpeed{1, stream_idle_timeout_secs_},
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
        const std::string message =
            ClassifyNetworkError(response.error, received_any_bytes, connect_timeout_ms_, stream_idle_timeout_secs_);
        return std::unexpected(Error{ErrorKind::Network, message, 0});
    }

    const int final_status = static_cast<int>(response.status_code);
    if (final_status < 200 || final_status >= 300) {
        std::string message = !error_body.empty() ? error_body : response.text;
        if (message.empty()) {
            message = "服务端返回了非 200 状态码,但响应体是空的";
        }
        return std::unexpected(Error{ErrorKind::HttpStatus, std::move(message), final_status});
    }

    // 流"正常"走完却没等到 MessageDone(message_delta):终止帧丢了或被当坏帧
    // 跳过了。这时 assembler 攒出来的消息不完整(stop_reason 是空的),当成功
    // 返回的话,上层会把半截消息当 end_turn 入历史——里头若有 tool_use,下一轮
    // 请求就 400。宁可明确报错。saw_stream_error 时不报:错误事件本身已经把
    // "这条流失败了"传给上层了。
    if (!saw_message_done && !saw_stream_error) {
        return std::unexpected(Error{ErrorKind::Parse, "流意外结束:未收到消息终止事件,响应不完整", 0});
    }

    return {};
}

}  // namespace lubancode::api::anthropic
