// ResponsesBackend:对接 OpenAI Responses API(MiniMax 提供的兼容端点)。
// 拼 JSON、POST /responses、边收边解析 SSE、逐个语义事件回调给上层。

#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <string>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "config/config.hpp"

namespace lubancode::api::responses {

class ResponsesBackend : public Backend {
public:
    // base_url 形如 https://api.minimaxi.com/v1 (不带结尾 /responses);
    // auth_token 走 Authorization: Bearer 头。
    // M11(网络超时):connect_timeout_ms(连接超时,毫秒)、
    // stream_idle_timeout_secs(SSE 读空闲超时,秒,不是总时长上限)两个都有
    // 默认值,来自 config::kDefault*,main.cpp 用 Config 里实际生效的值调用。
    // native_web_search:该端(ProviderConfig::native_web_search 镜像到
    // Config::native_web_search)是否声明协议原生联网搜索,默认 false。
    ResponsesBackend(std::string base_url, std::string auth_token,
                      int connect_timeout_ms = config::kDefaultConnectTimeoutMs,
                      int stream_idle_timeout_secs = config::kDefaultStreamIdleTimeoutSecs,
                      bool native_web_search = false);

    std::expected<void, Error> send_stream(
        const Request& request,
        const std::function<void(const StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override;

private:
    std::string base_url_;
    std::string auth_token_;
    int connect_timeout_ms_;
    int stream_idle_timeout_secs_;
    bool native_web_search_;
};

}  // namespace lubancode::api::responses
