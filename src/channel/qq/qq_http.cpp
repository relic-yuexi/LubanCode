#include "channel/qq/qq_http.hpp"

#include <utility>

#include "net/http_transport.hpp"

namespace lubancode::channel::qq {

namespace {

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
        const auto response =
            net::PerformFullHttpRequest(full, limits, /*cancel=*/nullptr, /*pinned=*/nullptr);
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
