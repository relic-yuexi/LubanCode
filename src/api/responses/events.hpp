// 语义层:把 SseFrame(SSE 分帧器吐出来的原始帧)翻译成中立的 StreamEvent。
// 只认得 OpenAI Responses API(MiniMax 兼容端点)的事件字段,认不得的
// 事件类型一律静默跳过,不抛异常、不崩。

#pragma once

#include <optional>

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

}  // namespace lubancode::api::responses
