// 纯函数:把中立的 Request 翻译成 OpenAI Responses API 的请求体 JSON。
// 不碰网络,方便单测直接调用、断言拼出来的 JSON 长什么样。

#pragma once

#include <nlohmann/json.hpp>

#include "api/types.hpp"

namespace lubancode::api::responses {

// 拼出 Responses API 的请求体(stream: true、store: false 恒定):
//   system  -> instructions
//   messages -> input 数组(text 块变 message item;assistant 的 tool_use
//               块变 function_call item;user 的 tool_result 块变
//               function_call_output item)
//   tools   -> [{"type":"function","name":...,"description":...,"parameters":input_schema}]
//   max_tokens -> max_output_tokens
nlohmann::json BuildRequestJson(const Request& request);

}  // namespace lubancode::api::responses
