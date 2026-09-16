// QQ 侧 HTTP seam(QQ 机器人接入单 Q1)。
//
// auth(token)与 messages(v2 发送)只做整请求/整响应的受控 POST/GET——
// 恰是 src/net/http_transport 的合同面,生产实现一行适配,不动 Provider
// 请求路径(§十五:PUT/DELETE/流式属 Q4 媒体批次)。测试注入假账,零网络。
//
// 泄露禁令:错误文案不带请求头/请求体(token、secret 都在那两处)。
#pragma once

#include <atomic>
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
    // 停止旗(A08):非空时生产实现把它递给 net 层——连接/上传/等首字节/
    // 收体任一阶段置位即就地掐流,分型 Cancelled。归发起方所有(适配器
    // 恒传 &stop_),HTTP seam 不保管寿命。假实现可忽略。
    const std::atomic<bool>* cancel = nullptr;
};

struct QqHttpResponse {
    int status = 0;
    std::string body;
    // 白名单诊断头投影(网关 400 诊断单 §四):只收平台 trace ID /
    // Retry-After 这类脱敏诊断值,名字小写、限条数与值长(适配层掐)。
    // 不把所有头搬进来——头表里可能有敏感物,更不许整表进日志。
    std::vector<std::pair<std::string, std::string>> diagnostic_headers;
};

// 一笔完整请求;错误文案是脱敏人话(网络/TLS/超时/取消)。
using QqHttpFunc =
    std::function<std::expected<QqHttpResponse, std::string>(const QqHttpRequest&)>;

// 生产实现:net::PerformFullHttpRequest 适配(连接 10s/硬墙 30s/响应体帽 4 MiB)。
QqHttpFunc MakeDefaultHttpFunc();

// 媒体路生产实现(Q4:附件下载/分片上传):与 MakeDefaultHttpFunc 同底座,
// 差别只在限额——连接 10s、硬墙按参数(下载大件比信令慢),响应体帽
// max_response_body_bytes(传输层在响应回调入口掐流,大件不进内存)。
// 支持 GET/POST/PUT(分片 PUT 预签名 URL 用;§十五:受控 PUT 属 Q4 扩底盘)。
QqHttpFunc MakeMediaHttpFunc(std::int64_t hard_timeout_ms,
                             std::int64_t max_response_body_bytes);

}  // namespace lubancode::channel::qq
