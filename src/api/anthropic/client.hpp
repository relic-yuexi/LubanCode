// AnthropicBackend:对接 Anthropic Messages API(MiniMax 提供的兼容端点)。
// 拼 JSON、POST /v1/messages、边收边解析 SSE、逐个语义事件回调给上层。

#pragma once

#include <expected>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"

namespace lubancode::api::anthropic {

// 纯函数:把中立 Request 翻译成 Anthropic Messages API 的请求体 JSON
// (stream 恒为 true)。send_stream 内部就是调这个函数拼请求体——单独在头文件
// 里声明出来,只是为了让单测能直接调用、断言拼出来的 JSON 长什么样(比如
// M6.6 的 think 强度要不要出现在 "thinking" 字段里),不碰网络、不改变任何
// 线上行为。
nlohmann::json BuildRequestJson(const Request& request);

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
