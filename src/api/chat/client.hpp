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
    // request_hard_timeout_secs(cpr 并发挂死单):每枚请求的硬墙钟(秒,
    // 0 = 不设),ProgressCallback 里比期限掐流——语义与 anthropic/responses
    // 两个 client 的同名参数一致,详见那边的注释。
    ChatCompletionsBackend(std::string base_url, std::string auth_token,
                           int connect_timeout_ms = config::kDefaultConnectTimeoutMs,
                           int stream_idle_timeout_secs = config::kDefaultStreamIdleTimeoutSecs,
                           nlohmann::json extra_body = nlohmann::json::object(),
                           std::map<std::string, std::string> extra_headers = {},
                           ChatRequestOptions options = {},
                           int request_hard_timeout_secs = config::kDefaultRequestHardTimeoutSecs);

    std::expected<void, Error> send_stream(
        const Request& request,
        const std::function<void(const StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override;

    // 诊断模式的 wire 序列化(与 send_stream 同一条拼装路,见各 client.cpp)。
    std::string SerializeForDiagnostics(const Request& request) const override;

    // 拍平对照与 extra_body 覆盖后的有效输出上限(差距清单 §8.2 第 7/8
    // 条),语义见 api/backend.hpp 的虚函数注释。
    std::optional<WireMessageMap> BuildWireMessageMap(const Request& request) const override;
    EffectiveOutputLimit GetEffectiveOutputLimit(const Request& request) const override;
    void ForceMaxOutputTokensOverride(Request& request, int tokens) const override;

private:
    std::string base_url_;
    std::string auth_token_;
    int connect_timeout_ms_;
    int stream_idle_timeout_secs_;
    nlohmann::json extra_body_;
    std::map<std::string, std::string> extra_headers_;
    ChatRequestOptions options_;
    int request_hard_timeout_secs_;
};

}  // namespace lubancode::api::chat
