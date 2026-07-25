#pragma once

#include <nlohmann/json.hpp>

#include "api/types.hpp"

namespace lubancode::api::chat {

// 把中立请求翻成 OpenAI Chat Completions 兼容请求。extra_body 最后浅合并，
// 供各家兼容端补 thinking、tool_stream 等私有字段。
nlohmann::json BuildRequestJson(const Request& request,
                                const nlohmann::json& extra_body = nlohmann::json::object());

}  // namespace lubancode::api::chat
