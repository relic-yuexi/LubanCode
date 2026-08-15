#pragma once

#include <nlohmann/json.hpp>

#include "api/types.hpp"

namespace lubancode::api::chat {

// reasoning 回传策略(随 provider capability 走,不按模型名散落特判):
//   never        一律不回传(现行默认;纯对话省 token 也合规矩)。
//   tool_episode 按 user-to-user 交互段办事(DeepSeek Chat 的协议):一段
//                交互只要实际走了工具调用,段内 assistant 的思考正文按
//                原字节原次序回传成 reasoning_content,后续 user/tool_result
//                轮次继续保留,不摘要、不加标签、不混进 content;纯对话段
//                的思考照旧略过。少了这段回传,DeepSeek 带 tools 的后续
//                请求可能直接吃 400。
// anthropic 走自带 signature 的 thinking 块,responses 走服务端状态/
// reasoning item,都不套这份 Chat 特判。
enum class ReasoningReplayPolicy { Never, ToolEpisode };

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
    ReasoningReplayPolicy reasoning_replay = ReasoningReplayPolicy::Never;
};

// 把中立请求翻成 OpenAI Chat Completions 兼容请求。extra_body 最后浅合并，
// 供各家兼容端补 thinking、tool_stream 等私有字段。
nlohmann::json BuildRequestJson(const Request& request,
                                const nlohmann::json& extra_body = nlohmann::json::object(),
                                const ChatRequestOptions& options = {});

}  // namespace lubancode::api::chat
