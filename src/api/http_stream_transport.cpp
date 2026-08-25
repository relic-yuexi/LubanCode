// HttpStreamTransport 的实现:四家 wire client 共用的 cpr 传输骨架。
// 设计与行为契约见 http_stream_transport.hpp 文件头。

#include "api/http_stream_transport.hpp"

#include <charconv>
#include <chrono>
#include <utility>

#include "cli/i18n.hpp"
#include "platform/json_safe.hpp"  // DescribeDumpFailure/DumpJsonSanitized:请求体 dump 的窄边界
#include "platform/log_sink.hpp"

namespace lubancode::api {

int ExtractStatusCode(std::string_view header_line) {
    if (header_line.rfind("HTTP/", 0) != 0) {
        return 0;
    }
    const std::size_t space1 = header_line.find(' ');
    if (space1 == std::string_view::npos) {
        return 0;
    }
    const std::size_t space2 = header_line.find(' ', space1 + 1);
    const std::string_view code_sv =
        header_line.substr(space1 + 1, space2 == std::string_view::npos ? std::string_view::npos : space2 - space1 - 1);
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
// 不丢细节。ABORTED_BY_CALLBACK 可能是我们自己在 ProgressCallback 里掐流
// 的结果(cpr 并发挂死单的硬墙钟,与 WriteCallback 掐流共用这个码),须与
// "用户取消"分开报——hard_timeout_hit 标志就是为这一步分型留的旁证。别的
// 错误码原样透传 curl 的 message(留着排查用,不做过度包装)。
std::string ClassifyNetworkError(const cpr::Error& error, bool received_any_bytes, int connect_timeout_ms,
                                 int stream_idle_timeout_secs, bool hard_timeout_hit, int hard_timeout_secs) {
    if (hard_timeout_hit) {
        return cli::trf("error.network.hard_timeout", hard_timeout_secs);
    }
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

std::string DumpRequestBody(const char* wire_tag, const nlohmann::json& body) {
    try {
        return body.dump();
    } catch (const nlohmann::json::exception& e) {
        // 请求序列化的最后兜底:历史里漏网的非法 UTF-8(旧会话文件、上游
        // 新开的口子)不再回传错误掐回合——那会把带病历史原样留在会话里,
        // 往后每回合都在这里挂,会话等于砖死。响亮记一笔日志,坏串按
        // U+FFFD 清洗后照发(与落盘边界同一政策),会话活着、模型看得见
        // 替换符。
        platform::LogSink::Instance().Warn(wire_tag,
                                            platform::DescribeDumpFailure(body, e) + " -> 已按 U+FFFD 清洗后发出");
        return platform::DumpJsonSanitized(body);
    }
}

std::expected<void, Error> PostSseStream(const HttpStreamCall& call, const StreamDataSink& sink,
                                         const std::atomic<bool>* cancel) {
    std::string error_body;
    int status_code = 0;
    bool status_known = false;
    bool cancelled = false;
    bool frame_overflow = false;
    // 有没有收到过响应体的第一个字节——网络错误发生时靠这个旁证分清
    // 是连接阶段就卡死了,还是流中途假死(见 ClassifyNetworkError 注释)。
    bool received_any_bytes = false;
    // 硬墙钟触发标志(cpr 并发挂死单):ProgressCallback 里比期限掐流后
    // 置位,收场分型据此把"我们主动杀的流"与 curl 自报的网络错分开——
    // cpr 只给一个共用的 ABORTED_BY_CALLBACK,不分青红皂白。
    bool hard_timeout_hit = false;

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
            if (!sink(data)) {
                // sink 报了协议绝境(分帧器单帧溢出):掐断传输,后面按
                // 协议错误报。
                frame_overflow = true;
                return false;
            }
            return true;
        });

    // ProgressCallback 在连接/TLS 握手阶段(还没有响应体)也会被周期性调用,
    // 返回 false 即中止——WriteCallback 只有数据到达才触发,光靠它,ESC 在
    // 连不上的服务器面前毫无办法。总超时不设死(流式回复可以很长),连接
    // 阶段单独给 connect_timeout_ms 上限。
    // 硬墙钟(cpr 并发挂死单)也挂在这里:request_hard_timeout_secs > 0 时
    // 对 steady_clock 记一枚期限,超期返回 false 掐流。真机现场(本机代理/
    // TUN 截胡 127.0.0.1 回环)出现过请求进 cpr::Post 再不返、connect/idle
    // 两道闸都不触发的挂死——libcurl 的 easy_transfer 循环每至多 1s 醒一拍
    // 调 Curl_pgrsUpdate,连接死寂也照醒,所以这道墙在"一个字节都不来"的
    // 绝境里也走得动。不用 cpr::Timeout 实现:那是 CURLOPT_TIMEOUT,会把
    // 正常的长流拦腰砍断,墙必须只掐"挂死",不掐"长"。
    const bool hard_wall_on = call.request_hard_timeout_secs > 0;
    const auto hard_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(call.request_hard_timeout_secs);
    cpr::ProgressCallback progress_cb(
        [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) -> bool {
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

    cpr::Header cpr_headers;
    for (const auto& [name, value] : call.headers) {
        cpr_headers[name] = value;
    }

    // LowSpeed{1, stream_idle_timeout_secs} 是"空闲读超时",不是总时长
    // 上限——libcurl 语义是"持续 stream_idle_timeout_secs 秒平均速率低于
    // 1 字节/秒就判超时",拿它当"连续 N 秒一个字节没收到"的等价检测(流式
    // 回答本身可以很长,故意不设总 Timeout)。
    cpr::Response response = cpr::Post(
        cpr::Url{call.url},
        cpr_headers,
        cpr::Body{call.body},
        cpr::ConnectTimeout{std::chrono::milliseconds(call.connect_timeout_ms)},
        #if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // 新版 cpr 弃用 int 构造改 chrono;vendored 1.11 只有 int 形,值两边通用
#endif
        cpr::LowSpeed{1, call.stream_idle_timeout_secs}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        ,
        header_cb,
        write_cb,
        progress_cb);

    // 收场分型,顺序有讲究:用户取消 > 帧溢出 > 网络错 > HTTP 状态。
    if (cancelled || (cancel != nullptr && cancel->load())) {
        return std::unexpected(Error{ErrorKind::Cancelled, "用户按 ESC 打断了这次请求", 0});
    }

    if (frame_overflow) {
        return std::unexpected(Error{ErrorKind::Parse, "SSE 单帧超过大小上限(8MB),协议错误,已断开", 0});
    }

    if (response.error) {
        const std::string message =
            ClassifyNetworkError(response.error, received_any_bytes, call.connect_timeout_ms,
                                 call.stream_idle_timeout_secs, hard_timeout_hit, call.request_hard_timeout_secs);
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

    return {};
}

}  // namespace lubancode::api
