// api 层对上暴露的抽象接口。agent 层只认这一个接口,不关心背后是
// Anthropic Messages 还是 OpenAI Responses 在干活。

#pragma once

#include <expected>
#include <functional>

#include "api/types.hpp"

namespace lubancode::api {

class Backend {
public:
    virtual ~Backend() = default;

    // 发一轮消息,流式拿结果。每收到一个语义事件就回调一次 on_event。
    // 失败(网络错、HTTP 非 200……)时返回 Error,调用方自己判断、自己处理。
    virtual std::expected<void, Error> send_stream(
        const Request& request,
        const std::function<void(const StreamEvent&)>& on_event) = 0;
};

}  // namespace lubancode::api
