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
//   always       工作视图里每条带 ThinkingBlock 的原始 assistant 消息都
//                回传(Kimi K3/K2.7 Code 的 Preserved Thinking 契约):
//                纯对话、工具调用、最终总结一视同仁,多枚思考块按块序
//                原字节拼接,一条消息只写一份字段;没思考不造空串,也不
//                凭正文猜。消息既然留在请求里,配套思考就不能剥掉。
// anthropic 走自带 signature 的 thinking 块,responses 走服务端状态/
// reasoning item,都不套这份 Chat 特判。
enum class ReasoningReplayPolicy { Never, ToolEpisode, Always };

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
    // reasoning_param:推理档位在请求体顶层的参数名。OpenAI 官方是
    // reasoning_effort(默认);有的本地兼容端叫别的名字,provider 可在
    // 配置里声明(ProviderConfig::think_param),经 Config 镜像到这里。
    // 空 = reasoning_effort。extra_body 仍在最后浅合并,用户显式写的
    // 同名字段整个压过这里。
    std::string reasoning_param = "reasoning_effort";
    // reasoning_delta_field:流式思考增量的字段名声明(解析侧)。空 =
    // 自动兼容:reasoning_content(DeepSeek 系)与 reasoning(vLLM
    // 0.27+/Qwen 系)两个只读别名都认,同一 chunk 两者都有时按固定
    // 优先级去重(EventParser 注释)。provider 声明了就只认那一个字段。
    // 只影响解析;不进请求体。
    std::string reasoning_delta_field;
    // reasoning_replay_field:reasoning 回传(tool_episode 策略)时写进
    // assistant 消息的字段名。默认 reasoning_content(DeepSeek 协议);
    // vLLM/Qwen 这类只认 reasoning 的端由 provider 声明改写。空 =
    // reasoning_content。不想当然把所有服务都写成同一个名字。
    std::string reasoning_replay_field;
};

// 把中立请求翻成 OpenAI Chat Completions 兼容请求。extra_body 最后浅合并，
// 供各家兼容端补 thinking、tool_stream 等私有字段。
nlohmann::json BuildRequestJson(const Request& request,
                                const nlohmann::json& extra_body = nlohmann::json::object(),
                                const ChatRequestOptions& options = {});

}  // namespace lubancode::api::chat
