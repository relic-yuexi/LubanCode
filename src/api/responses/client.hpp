// ResponsesBackend:对接 OpenAI Responses API(MiniMax 提供的兼容端点)。
// 拼 JSON、POST /responses、边收边解析 SSE、逐个语义事件回调给上层。

#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "config/config.hpp"

namespace lubancode::api::responses {

// 纯函数:把 extra_headers 覆盖/追加到一份基础 HTTP 头表里。批六(API
// 传输合流)起,实现收拢在 api/types.hpp 的 api::ApplyExtraHeaders(四家
// wire 共用一份),这里保留这个公开名字当薄壳——单测钉着它。当年"两边
// 各自小巧,不为共用几行代码搭公共头"的局部决定,四份 client 过了临界,
// 翻篇了。
std::map<std::string, std::string> ApplyExtraHeaders(std::map<std::string, std::string> base,
                                                        const std::map<std::string, std::string>& extra_headers);

class ResponsesBackend : public Backend {
public:
    // base_url 形如 https://api.minimaxi.com/v1 (不带结尾 /responses);
    // auth_token 走 Authorization: Bearer 头。
    // M11(网络超时):connect_timeout_ms(连接超时,毫秒)、
    // stream_idle_timeout_secs(SSE 读空闲超时,秒,不是总时长上限)两个都有
    // 默认值,来自 config::kDefault*,main.cpp 用 Config 里实际生效的值调用。
    // request_hard_timeout_secs(cpr 并发挂死单):每枚请求的硬墙钟(秒,
    // 0 = 不设),ProgressCallback 里比期限掐流——语义与 anthropic/chat 两个
    // client 的同名参数一致,详见 anthropic/client.hpp 的注释。
    // native_web_search:该端(ProviderConfig::native_web_search 镜像到
    // Config::native_web_search)是否声明协议原生联网搜索,默认 false。
    // extra_body/extra_headers:同上,从 Config 同名字段传进来,默认都是
    // 空(不合并/不加任何东西)。
    ResponsesBackend(std::string base_url, std::string auth_token,
                      int connect_timeout_ms = config::kDefaultConnectTimeoutMs,
                      int stream_idle_timeout_secs = config::kDefaultStreamIdleTimeoutSecs,
                      bool native_web_search = false,
                      nlohmann::json extra_body = nlohmann::json::object(),
                      std::map<std::string, std::string> extra_headers = {},
                      int request_hard_timeout_secs = config::kDefaultRequestHardTimeoutSecs);

    std::expected<void, Error> send_stream(
        const Request& request,
        const std::function<void(const StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override;

    // 诊断模式的 wire 序列化(与 send_stream 同一条拼装路,见 client.cpp)。
    std::string SerializeForDiagnostics(const Request& request) const override;

private:
    std::string base_url_;
    std::string auth_token_;
    int connect_timeout_ms_;
    int stream_idle_timeout_secs_;
    bool native_web_search_;
    nlohmann::json extra_body_;
    std::map<std::string, std::string> extra_headers_;
    int request_hard_timeout_secs_;
};

}  // namespace lubancode::api::responses
