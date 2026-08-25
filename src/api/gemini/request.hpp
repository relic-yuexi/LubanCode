// Google Gemini 原生 Generate Content API(v1beta)的请求翻译层:把中立
// api::Request 翻成 Generate Content 请求体。纯函数,不碰网络,好单测。

#pragma once

#include <nlohmann/json.hpp>

#include "api/types.hpp"

namespace lubancode::api::gemini {

// 把中立请求翻成 Gemini Generate Content 请求体:
//   - system -> systemInstruction.parts[{text}](不进 contents,Gemini 的
//     contents 里没有 system 角色);
//   - 消息 role:User -> "user",Assistant -> "model";
//   - 文本 -> parts[{text}],图片 -> parts[{inlineData:{mimeType,data}}];
//   - assistant 的工具调用 -> parts[{functionCall:{name,args}}];
//   - user 的工具结果 -> 单独一条 role:"user" 的 content,
//     parts[{functionResponse:{name,response}}](协议只认函数名,不认调用
//     id,所以先扫一遍历史把 tool_use_id 对回函数名);
//   - 思考块(ThinkingBlock)不回传:Gemini 的 thought 是一次性的,续会话
//     不重放(与 responses wire 同一取舍);
//   - max_tokens -> generationConfig.maxOutputTokens;
//   - reasoning_effort 非空 -> generationConfig.thinkingConfig；模型档案的
//     wireDialect 决定写 thinkingLevel 或 thinkingBudget，none/minimal 关。
//
// extra_body(provider 级先合并,Request::extra_body 后合并)顶层浅合并、
// 同名键整个覆盖——唯独 "generationConfig" 一键例外:两家都是 object 时按
// 一层深合并(user 的子键压过内置子键,内置的 maxOutputTokens/
// thinkingConfig 不被整块冲掉)，所以这一键必须深一层。
nlohmann::json BuildRequestJson(const Request& request,
                                const nlohmann::json& extra_body = nlohmann::json::object());

// 流式端点:POST {base_url}/v1beta/models/{model}:streamGenerateContent?alt=sse。
// model 带 "models/" 前缀(比如从 ListModels 原样抄来的名字)时剥掉,结尾
// 斜杠同理。单独导出给 client 用,也让单测能直接断言 URL 形状。
std::string StreamUrl(const std::string& base_url, const std::string& model);

}  // namespace lubancode::api::gemini
