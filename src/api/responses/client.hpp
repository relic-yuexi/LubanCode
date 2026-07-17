// ResponsesBackend:对接 OpenAI Responses API(MiniMax 提供的兼容端点)。
// 拼 JSON、POST /responses、边收边解析 SSE、逐个语义事件回调给上层。

#pragma once

#include <expected>
#include <functional>
#include <string>

#include "api/backend.hpp"
#include "api/types.hpp"

namespace lubancode::api::responses {

class ResponsesBackend : public Backend {
public:
    // base_url 形如 https://api.minimaxi.com/v1 (不带结尾 /responses);
    // auth_token 走 Authorization: Bearer 头。
    ResponsesBackend(std::string base_url, std::string auth_token);

    std::expected<void, Error> send_stream(
        const Request& request,
        const std::function<void(const StreamEvent&)>& on_event) override;

private:
    std::string base_url_;
    std::string auth_token_;
};

}  // namespace lubancode::api::responses
