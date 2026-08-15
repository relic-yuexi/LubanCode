#pragma once

#include <nlohmann/json.hpp>

#include "api/types.hpp"

namespace lubancode::api::chat {

// Chat 请求的 transport 选项(随 provider capability 走,不进中立
// api::Request——那是三家 wire 共用的形状,这一层是 Chat 私有的):
//
//   stream_usage      流式请求带 stream_options.include_usage=true,
//                     让服务端在 [DONE] 前多回一只完整 usage chunk
//                     (DeepSeek 等家靠这个拿到逐请求的缓存 hit/miss)。
//                     有些兼容端不认 stream_options,不可全局生塞——
//                     由 provider 目录按家声明,默认不发。
//                     extra_body 仍在最后浅合并,用户显式写了
//                     stream_options 就整个压过这里。
struct ChatRequestOptions {
    bool stream_usage = false;
};

// 把中立请求翻成 OpenAI Chat Completions 兼容请求。extra_body 最后浅合并，
// 供各家兼容端补 thinking、tool_stream 等私有字段。
nlohmann::json BuildRequestJson(const Request& request,
                                const nlohmann::json& extra_body = nlohmann::json::object(),
                                const ChatRequestOptions& options = {});

}  // namespace lubancode::api::chat
