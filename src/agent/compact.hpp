// M6.6:上下文压缩。history 长了(手动 /compact,或者 ContextTracker 测出
// 占用过 80% 自动触发)时,把整段历史发给模型,换一份浓缩过的"存档"回来,
// 顶替掉中间那大段老对话——跟 agent/context.hpp 的 TrimHistory(按字符数硬切,
// 不管语义、纯粹丢内容)是两条完全不同的路子:这里靠模型自己读懂对话再
// 总结,保留任务目标/关键决策/文件路径/代码要点/未完成事项,是主防线;
// TrimHistory 仍然留着当兜底的硬安全网,两者互不影响、互不依赖。

#pragma once

#include <expected>
#include <string>
#include <vector>

#include "api/backend.hpp"
#include "api/types.hpp"

namespace lubancode::agent {

// 拼出压缩后的完整新历史:archive 消息打头,后面接"最近一轮完整对话"——
// 从历史里最后一条"真正的用户文本输入"(角色是 user 且至少带一个
// TextBlock,工具结果消息角色虽然也是 user,但只有 ToolResultBlock,不算)
// 开始,一直到历史末尾的所有消息原样保留。这一段天然是完整的一轮
// (用户输入 + 之后所有 assistant 回复/工具调用/工具结果,直到下一条用户
// 输入之前),tool_use 和它对应的 tool_result 必然同在这一段里,不会被
// archive 和"最近一轮"这刀切开。
// history 里压根找不到这样一条用户文本输入(比如整段历史只有工具消息,
// 或者 history 本身是空的)时,新历史里只有 archive 这一条。
std::vector<api::Message> BuildCompactedHistory(const std::vector<api::Message>& history,
                                                  const api::Message& archive);

// 向模型请求一次历史压缩:把 history 整份连同一条系统指令发过去,指令要求
// 模型把这段对话压缩成一份存档,保留任务目标、关键决策、涉及的文件路径、
// 代码要点、尚未完成的事项,闲聊和过程细节可以舍弃;focus 非空时,在指令
// 末尾追加"重点保留:{focus}"这一句,让用户能指定这次压缩额外关照哪块。
// history 最后一条若是 assistant 消息,会在发送前补一条空信号性质的 user
// 消息收尾,避免被 API 当成"续写最后一条 assistant 消息"(prefill
// continuation)处理而无视 system 指令——这是实测踩出来的坑,不是可选项。
// 成功时返回一条 role=user 的消息,content 形如
// "[对话存档,此前内容已压缩] ...(模型给出的正文)"——之所以是 user 角色,
// 是因为它要顶在新历史最前面,充当"这轮对话开场时已经知道的背景",跟真正
// 的用户输入摆在同一个角色语义下,下游(TrimHistory 的 IsUserTurnStart 判断、
// 各 wire 的请求拼装)才认得出这是"一轮的开头"。
// backend 调用失败(网络断了、HTTP 非 2xx、流里报错)原样把 api::Error 透传
// 出去,不改动、不丢失传入的 history(本来就是 const 引用,调用方失败时
// 应该继续用旧历史,不能半途换掉)。
std::expected<api::Message, api::Error> Compact(api::Backend& backend, const std::string& model,
                                                   const std::vector<api::Message>& history,
                                                   const std::string& focus);

}  // namespace lubancode::agent
