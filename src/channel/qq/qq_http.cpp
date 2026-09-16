#include "channel/qq/qq_http.hpp"

#include <utility>

#include "net/http_transport.hpp"

namespace lubancode::channel::qq {

namespace {

// 诊断头白名单(§四:trace ID / Retry-After)。平台文档未钉死 trace 头名,
// 常见几种全收,值长掐 128(诊断够用,长值多半不是 trace)。
constexpr const char* kDiagnosticHeaderWhitelist[] = {
    "retry-after", "x-trace-id", "trace-id", "x-traceid", "x-request-id",
    "x-tencent-traceid",
};
constexpr std::size_t kMaxDiagnosticHeaders = 4;
constexpr std::size_t kDiagnosticHeaderValueCap = 128;

std::string ToLowerName(std::string name) {
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return name;
}

// 响应头表 -> 白名单诊断投影(限条数与值长)。找不到给空表——诊断头是
// 可选增强,绝不因缺失改变成败判定。
std::vector<std::pair<std::string, std::string>> ProjectDiagnosticHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [name, value] : headers) {
        if (out.size() >= kMaxDiagnosticHeaders) {
            break;
        }
        const std::string lower = ToLowerName(name);
        for (const char* allowed : kDiagnosticHeaderWhitelist) {
            if (lower == allowed) {
                std::string capped = value.substr(0, kDiagnosticHeaderValueCap);
                out.emplace_back(lower, std::move(capped));
                break;
            }
        }
    }
    return out;
}

// 共用底座:net::PerformFullHttpRequest 一笔适配;分型文案不带请求内容
//(头里是 token,预签名 url 在 query)。
QqHttpFunc MakeHttpFuncWithLimits(net::FullHttpLimits limits) {
    return [limits = std::move(limits)](
               const QqHttpRequest& request) -> std::expected<QqHttpResponse, std::string> {
        net::FullHttpRequest full;
        full.method = request.method;
        full.url = request.url;
        full.headers = request.headers;
        full.body = request.body;
        // A08:停止旗穿到 net 层——DNS/TCP/TLS/上传/收体各阶段可被打断
        //(适配器恒在请求上盖章 &stop_)。
        const auto response =
            net::PerformFullHttpRequest(full, limits, request.cancel, /*pinned=*/nullptr);
        if (!response.has_value()) {
            switch (response.error().kind) {
                case net::FullHttpErrorKind::Cancelled:
                    return std::unexpected("http cancelled");
                case net::FullHttpErrorKind::Timeout:
                    return std::unexpected("http timeout");
                case net::FullHttpErrorKind::DnsFailed:
                    return std::unexpected("http dns failed");
                case net::FullHttpErrorKind::TlsFailed:
                    return std::unexpected("http tls failed");
                case net::FullHttpErrorKind::ResponseHeaderTooLarge:
                case net::FullHttpErrorKind::ResponseBodyTooLarge:
                    return std::unexpected("http response over cap");
                default:
                    return std::unexpected("http network failed");
            }
        }
        QqHttpResponse out;
        out.status = response->status;
        out.body = std::move(response->body);
        out.diagnostic_headers = ProjectDiagnosticHeaders(response->headers);
        return out;
    };
}

}  // namespace

QqHttpFunc MakeDefaultHttpFunc() {
    return MakeHttpFuncWithLimits(net::FullHttpLimits{});
}

QqHttpFunc MakeMediaHttpFunc(std::int64_t hard_timeout_ms,
                             std::int64_t max_response_body_bytes) {
    net::FullHttpLimits limits;
    if (hard_timeout_ms > 0) {
        limits.hard_timeout_ms = hard_timeout_ms;
    }
    if (max_response_body_bytes > 0) {
        limits.response_body_bytes = max_response_body_bytes;
    }
    return MakeHttpFuncWithLimits(limits);
}

}  // namespace lubancode::channel::qq
