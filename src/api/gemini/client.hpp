// GeminiBackend:对接 Google Gemini 原生 Generate Content API(v1beta)。
// 拼 JSON、POST /v1beta/models/{model}:streamGenerateContent?alt=sse、边收
// 边解析 SSE、逐个语义事件回调给上层。cpr 管线(超时/硬墙钟/取消/溢出)
// 与 chat/responses 两个 client 同一副骨架,注释从简,细节看 anthropic/
// client.cpp 同款注释。

#pragma once

#include <atomic>
#include <expected>
#include <functional>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "config/config.hpp"

namespace lubancode::api::gemini {

class GeminiBackend : public Backend {
public:
    // base_url 形如 https://generativelanguage.googleapis.com(不带结尾
    // /v1beta/...);auth_token 走 x-goog-api-key 头(Gemini 的 native 鉴权,
    // 不是 Authorization Bearer),为空(鉴权三态 none/缺 env)时彻底不带。
    // connect_timeout_ms / stream_idle_timeout_secs / request_hard_timeout_secs
    // 的语义与默认值同 anthropic/responses/chat 三个 client,见
    // anthropic/client.hpp 的注释。
    // extra_body:浅合并进请求体顶层(generationConfig 一键深一层,见
    // request.hpp);extra_headers:覆盖/追加到基础头,同名覆盖(含
    // x-goog-api-key)。
    GeminiBackend(std::string base_url, std::string auth_token,
                  int connect_timeout_ms = config::kDefaultConnectTimeoutMs,
                  int stream_idle_timeout_secs = config::kDefaultStreamIdleTimeoutSecs,
                  nlohmann::json extra_body = nlohmann::json::object(),
                  std::map<std::string, std::string> extra_headers = {},
                  int request_hard_timeout_secs = config::kDefaultRequestHardTimeoutSecs);

    std::expected<void, Error> send_stream(
        const Request& request,
        const std::function<void(const StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override;

private:
    std::string base_url_;
    std::string auth_token_;
    int connect_timeout_ms_;
    int stream_idle_timeout_secs_;
    nlohmann::json extra_body_;
    std::map<std::string, std::string> extra_headers_;
    int request_hard_timeout_secs_;
};

}  // namespace lubancode::api::gemini
