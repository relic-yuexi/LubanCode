// QQ 侧 HTTP seam(QQ 机器人接入单 Q1)。
//
// auth(token)与 messages(v2 发送)只做整请求/整响应的受控 POST/GET——
// 恰是 src/net/http_transport 的合同面,生产实现一行适配,不动 Provider
// 请求路径(§十五:PUT/DELETE/流式属 Q4 媒体批次)。测试注入假账,零网络。
//
// 泄露禁令:错误文案不带请求头/请求体(token、secret 都在那两处)。
#pragma once

#include <expected>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::channel::qq {

struct QqHttpRequest {
    std::string method;  // "GET"/"POST"
    std::string url;     // 绝对 https URL
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;  // 可空
};

struct QqHttpResponse {
    int status = 0;
    std::string body;
};

// 一笔完整请求;错误文案是脱敏人话(网络/TLS/超时/取消)。
using QqHttpFunc =
    std::function<std::expected<QqHttpResponse, std::string>(const QqHttpRequest&)>;

// 生产实现:net::PerformFullHttpRequest 适配(连接 10s/硬墙 30s/响应体帽 4 MiB)。
QqHttpFunc MakeDefaultHttpFunc();

}  // namespace lubancode::channel::qq
