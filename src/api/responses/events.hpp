// 语义层:把 SseFrame(SSE 分帧器吐出来的原始帧)翻译成中立的 StreamEvent。
// 只认得 OpenAI Responses API(MiniMax 兼容端点)的事件字段,认不得的
// 事件类型一律静默跳过,不抛异常、不崩。

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "api/sse_framing.hpp"
#include "api/types.hpp"

namespace lubancode::api::responses {

// 解析一帧 SSE 数据。判定事件种类靠 data 里的 "type" 字段。
//
// 返回 std::nullopt 表示这一帧不需要往上抛任何 StreamEvent,可能是:
//   - JSON 解析失败(数据坏了,跳过而不是崩溃);
//   - 认得种类但语义上不需要单独发事件(比如 response.created、
//     response.in_progress、reasoning 相关事件——眼下不展示);
//   - 完全没见过的事件类型(尚未接线的内置工具、MCP 调用等)。
// web_search_call 例外：翻成 BuiltinToolStart/Done，只展示，不本地执行。
std::optional<StreamEvent> parse_event(const SseFrame& frame);

// 非流式响应体(POST /responses 不带 stream,一次 JSON 对象)的展开路径
//(vLLM 本地模型勘察单 P2;当前客户端恒走流式,这层给非流式体一条现成的
// 中立事件翻译,诊断驱动/未来非流式请求模式直接用,不必再走 SSE 分帧):
//   output[].reasoning  -> ThinkingDelta(content[].reasoning_text 逐段,
//                          vLLM 扩展;summary[].summary_text 是 OpenAI 官方
//                          形状,两者取在场的那种——vLLM 端 summary 恒空)
//   output[].message    -> TextDelta(content[].output_text 逐段)+ ContentBlockDone
//   output[].function_call -> ToolUseStart + ToolUseInputDelta(arguments
//                          非空时一整段)+ ContentBlockDone(id 认 call_id,
//                          vLLM 非流式项带 fc_/chatcmpl-tool- 双 id,取 call_id)
//   末尾一枚 MessageDone:stop_reason 与 usage 口径同 response.completed
//   (incomplete → max_tokens;有 function_call → tool_use;否则 end_turn;
//   usage 摊法见 HandleCompleted 的注释)。
// 坏 JSON/坏形状返回空数组,不抛——调用方按"没有 MessageDone"当不完整
// 响应处理,与流式路的兜底同一语义。
std::vector<StreamEvent> ExpandNonStreamResponse(const std::string& body);

}  // namespace lubancode::api::responses
