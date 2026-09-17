#include "channel/feishu/feishu_http.hpp"

#include <utility>

#include "net/http_transport.hpp"

namespace lubancode::channel::feishu {

FeishuHttpFunc MakeDefaultFeishuHttpFunc() {
    // 共用底座:net::PerformFullHttpRequest 一笔适配(照 qq_http 的
    // MakeHttpFuncWithLimits,信令路用默认限额)。分型文案不带请求内容
    //(引导体里有 AppSecret,发送头里有 token)。
    return [](const FeishuHttpRequest& request)
               -> std::expected<FeishuHttpResponse, std::string> {
        net::FullHttpRequest full;
        full.method = request.method;
        full.url = request.url;
        full.headers = request.headers;
        full.body = request.body;
        // 停止旗穿到 net 层:DNS/TCP/TLS/上传/收体各阶段可被打断。
        const auto response =
            net::PerformFullHttpRequest(full, net::FullHttpLimits{}, request.cancel,
                                        /*pinned=*/nullptr);
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
        FeishuHttpResponse out;
        out.status = response->status;
        out.body = std::move(response->body);
        return out;
    };
}

}  // namespace lubancode::channel::feishu
