// AnthropicBackend:对接 Anthropic Messages API(MiniMax 提供的兼容端点)。
// 拼 JSON、POST /v1/messages、边收边解析 SSE、逐个语义事件回调给上层。

#pragma once

#include <expected>
#include <functional>
#include <string>

#include "api/backend.hpp"
#include "api/types.hpp"

namespace lubancode::api::anthropic {

class AnthropicBackend : public Backend {
public:
    // base_url 形如 https://api.minimaxi.com/anthropic (不带结尾 /v1/messages);
    // auth_token 走 Authorization: Bearer 头(MiniMax 用这个,不是 x-api-key)。
    AnthropicBackend(std::string base_url, std::string auth_token);

    std::expected<void, Error> send_stream(
        const Request& request,
        const std::function<void(const StreamEvent&)>& on_event) override;

private:
    std::string base_url_;
    std::string auth_token_;
};

}  // namespace lubancode::api::anthropic
