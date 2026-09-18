#pragma once

#include <nlohmann/json.hpp>

#include "api/types.hpp"

namespace lubancode::api::chat {

// 旧目录/调用方兼容枚举。回传开关统一由 Request.reasoning_history 与
// reasoning_effort 决定,默认完整保留;不再按工具交互裁剪。
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
    ReasoningReplayPolicy reasoning_replay = ReasoningReplayPolicy::Always;
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
// 供各家兼容端补 thinking、tool_stream 等私有字段。wire_map 非空时随拼装
// 同路产出"内部消息序 -> wire messages 序"的对照(差距清单 §8.2 第 7
// 条),不影响出口 JSON 一个字节。
nlohmann::json BuildRequestJson(const Request& request,
                                const nlohmann::json& extra_body = nlohmann::json::object(),
                                const ChatRequestOptions& options = {},
                                WireMessageMap* wire_map = nullptr);

// 拍平对照(差距清单 §8.2 第 7 条):与 BuildRequestJson 同一条拼装路。
// chat 的形状:Request::system 与多条 System 消息拼成 wire[0] 一条
// system 消息(多个内部 messageRef 对同一 wire 消息);User/Tool 消息
// 可一裂二(正文落 user、每枚 ToolResultBlock 各落一条 tool);只装
// 工具结果的消息不产 user 消息;assistant 恒一条。供 v3 账 prepared
// 事件对账/验尸。
WireMessageMap BuildMessageWireMap(const Request& request);

}  // namespace lubancode::api::chat
