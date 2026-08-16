// 回合收尾的记忆抽取(0.30.x 候审箱第一期):外层回合结束后,把本轮
// 增量(用户消息、最终回答、结构化工具摘要)交给主模型做一次总结,顺手
// 产出去重候选与下一轮检索扩展词。抽取借当前主模型、严格 JSON、失败降级
// 不影响主会话——这套纯逻辑与请求拼装都住在这,交互会话只管接线。

#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

#include "api/backend.hpp"
#include "api/types.hpp"

namespace lubancode::app {

// 抽取结果(回合总结 + 候选 + 检索扩展词)。
struct ProposedCandidate {
    std::string kind;         // fact | preference | feedback
    std::string title;
    std::string summary;
    std::string content;
    std::vector<std::string> keywords;
    std::vector<std::string> paths;
    std::string confidence;   // user-stated | verified | inferred
};

struct MemoryExtraction {
    std::string task_type;    // code | research | config | docs | other
    std::string summary;
    std::vector<std::string> retrieval_terms;
    std::vector<ProposedCandidate> candidates;
};

// 任务类型判定(用户基调 1:先推测目的再选总结提示词)。纯词法启发,不
// 打请求;user_text 是本轮用户消息,tool_names 是本轮调用过的工具名。
std::string ClassifyTaskType(const std::string& user_text, const std::vector<std::string>& tool_names);

// 把一个回合的消息增量压成给模型看的转写:用户/助手正文收全(各截
// 4 KiB),工具调用只留名字与紧凑入参,工具结果只留开头一小段;大段日志、
// 网页/MCP 原文不整包送抽取。max_bytes 是整段转写的字节上限。
std::string BuildTurnTranscript(const std::vector<api::Message>& messages, std::size_t max_bytes);

// 抽取提示词:基础契约(features/memory-summary-base.md)+ 分型侧重
// (features/memory-summary-<type>.md),用户目录可覆盖。task_type 认不出
// 时用 other。
std::string BuildExtractionSystemPrompt(const std::string& prompts_dir, const std::string& task_type);

// 解析模型输出(容错:剥代码围栏、取首个 { 到末个 })。候选最多 3 条,
// 字段缺错的整条丢弃,不整份报错。
std::expected<MemoryExtraction, std::string> ParseExtractionJson(const std::string& text);

// 发一次抽取请求(同步,带看门狗取消)。失败只返回错误,调用方降级。
std::expected<MemoryExtraction, std::string> RunMemoryExtraction(api::Backend& backend,
                                                                 const std::string& model,
                                                                 const std::string& system_prompt,
                                                                 const std::string& transcript,
                                                                 int timeout_secs);

}  // namespace lubancode::app
