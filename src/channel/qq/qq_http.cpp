#include "channel/qq/qq_http.hpp"

#include "net/http_transport.hpp"

namespace lubancode::channel::qq {

QqHttpFunc MakeDefaultHttpFunc() {
    return [](const QqHttpRequest& request) -> std::expected<QqHttpResponse, std::string> {
        net::FullHttpRequest full;
        full.method = request.method;
        full.url = request.url;
        full.headers = request.headers;
        full.body = request.body;
        net::FullHttpLimits limits;
        const auto response = net::PerformFullHttpRequest(full, limits, /*cancel=*/nullptr,
                                                          /*pinned=*/nullptr);
        if (!response.has_value()) {
            // 分型文案;不带请求内容(头里是 token)。
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

}  // namespace lubancode::channel::qq
