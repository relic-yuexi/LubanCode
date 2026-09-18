// 飞书侧 HTTP seam(飞书/企微设计单 F1,§5.8"照 QqHttpFunc 一行适配")。
//
// 引导(拿 WSS 地址)、tenant_access_token、回话三路只做整请求/整响应的
// 受控 POST——恰是 src/net/http_transport 的合同面,生产实现一行适配,
// 不动 net 层。测试注入假账,零网络。
//
// 泄露禁令:错误文案不带请求头/请求体(引导体里有 AppSecret、发送头里
// 有 Bearer token——都在那两处)。
#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::channel::feishu {

struct FeishuHttpRequest {
    std::string method;  // "GET"/"POST"
    std::string url;     // 绝对 https URL
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;  // 可空
    // 停止旗(照 QqHttpRequest 的 A08 口径):非空时生产实现把它递给 net
    // 层,任一阶段置位即就地掐流。归发起方所有(适配器恒传 &stop_)。
    const std::atomic<bool>* cancel = nullptr;
};

struct FeishuHttpResponse {
    int status = 0;
    std::string body;
};

// 一笔完整请求;错误文案是脱敏人话(网络/TLS/超时/取消)。
using FeishuHttpFunc =
    std::function<std::expected<FeishuHttpResponse, std::string>(const FeishuHttpRequest&)>;

// 生产实现:net::PerformFullHttpRequest 一笔适配(默认限额;信令路无
// 大件)。飞书 F1 无媒体路,不需要 Media 变体。
FeishuHttpFunc MakeDefaultFeishuHttpFunc();

}  // namespace lubancode::channel::feishu
