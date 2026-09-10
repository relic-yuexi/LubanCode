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
//              request.tools 非空时,每个本地函数工具映射成一项;
//              native_web_search=true 时额外追加 {"type":"web_search"} 一项
//              (哪怕 request.tools 是空的也照样追加,声明的是服务端原生
//              联网搜索,不是本地函数工具)
//   max_tokens -> max_output_tokens
// native_web_search 默认 false,不传就是现状行为零变化。
// extra_body:Config::extra_body(顶层单 provider 配置,或者切 provider 时
// 从 ProviderConfig::extra_body 镜像过来)——浅合并进请求体顶层,merge 点
// 在所有内置逻辑拼完之后、返回之前,键冲突时 extra_body 的值整个覆盖掉
// 前面算出来的值,不做深合并。默认空 object,等于不合并任何东西。
nlohmann::json BuildRequestJson(const Request& request, bool native_web_search = false,
                                 const nlohmann::json& extra_body = nlohmann::json::object(),
                                 WireMessageMap* wire_map = nullptr);

// 拍平对照(轨迹 v3 差距清单 §8.2 第 7 条):与 BuildRequestJson 同一条
// 拼装路产出(第四参传指针共用,不另写影子逻辑)。responses 逐块成
// item——一条内部消息可裂成多个 item(正文 message + 每枚工具调用/
// 结果各一个),思考块(含加密思考)跳过后整条没剩东西的是空对照。
// 供 v3 账 model.request.prepared 的 inputMessageRefs 对账/验尸。
WireMessageMap BuildMessageWireMap(const Request& request);

}  // namespace lubancode::api::responses
