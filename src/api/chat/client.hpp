#pragma once

#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/chat/request.hpp"
#include "config/config.hpp"

namespace lubancode::api::chat {

class ChatCompletionsBackend : public Backend {
public:
    // options.stream_usage:provider 声明支持流式 usage chunk 时置真,
    // 请求体带 stream_options.include_usage(见 request.hpp)。
    ChatCompletionsBackend(std::string base_url, std::string auth_token,
                           int connect_timeout_ms = config::kDefaultConnectTimeoutMs,
                           int stream_idle_timeout_secs = config::kDefaultStreamIdleTimeoutSecs,
                           nlohmann::json extra_body = nlohmann::json::object(),
                           std::map<std::string, std::string> extra_headers = {},
                           ChatRequestOptions options = {});

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
    ChatRequestOptions options_;
};

}  // namespace lubancode::api::chat
