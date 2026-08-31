// ResolvedPromptBuilder(Token 账本单 A1:事实接线)。
//
// A0 只冻了 PromptManifest 的数据合同(agent/prompt_manifest.hpp)与合成
// 夹具;这一件把它接上真实拼装现场——PromptAssembler(AssembleSystemPrompt
// 的五层回路)+ AgentLoop 每次请求现叠的三层后叠(延迟工具索引 -> 模型
// 目录指令 -> 魂,见 agent/prompts.hpp 的 With* 与 agent/loop.cpp 的调用
// 次序)。manifest 从这一次真实拼装现场直接产出,不靠 analyzer 事后拆
// 字符串猜模块边界(§6.4"manifest 不靠 analyzer 事后拆字符串")。
//
// 两段式,理由是性能与事实边界都要顾:
//   BuildResolvedPromptBase  —— AssembleSystemPrompt 五层回路要读用户/项目/
//                               包目录的模块文件,构造期(或 /clear 重建时)
//                               跑一次,结果连同 PromptSourceLedger 存住
//                              (与 AgentProfile.system_prompt 缓存同一份
//                               生命周期,不为求 manifest 每步重新读盘)。
//   ResolveFinalPrompt        —— 每次请求现叠三层后叠(它们随 tool_search
//                               命中、/model 切换、/soul 切换逐请求变化,
//                               这三段本就是纯字符串拼接,没有磁盘 IO),
//                               在 base 之上产完整 AssembledPrompt。
//
// 调用方约定次序与 loop.cpp 原有三行(WithDeferredToolsIndex ->
// WithModelInstructions -> WithSoul)一字不差——manifest 的 segments 顺序
// 与实际发给模型的文本顺序必须对得上,不然 audit 报告指错位置。
#pragma once

#include <string>

#include "agent/prompt_assembler.hpp"
#include "agent/prompt_manifest.hpp"

namespace lubancode::agent {

// AssembleSystemPrompt 一次拼装的底账:原始文本(未叠三层后叠)+ 来源
// 账本,同一次调用产出,彼此不会错位。
struct ResolvedPromptBase {
    std::string text;
    PromptSourceLedger ledger;
    std::string assembly_version = "prompt-assembler-v1";
};

// 跑一次 AssembleSystemPrompt,顺带把 ledger 收进返回值。构造期/
// /clear 重建时调一次;三层后叠不在这里(见 ResolveFinalPrompt)。
ResolvedPromptBase BuildResolvedPromptBase(const PromptOptions& options);

// 一次真实请求的完整拼装结果:base 之上叠 deferred tool index -> model
// instructions -> soul(次序与 agent/loop.cpp 一致),产最终文本与完整
// PromptManifest(segments 含 base 的来源段 + 三层后叠段;soul/
// model_instructions 另写 manifest 顶层字段,§6.4 schema)。
// soul_name 空 = "default"(manifest.soul.name 的口径)。
struct AssembledPrompt {
    std::string text;
    PromptManifest manifest;
};

AssembledPrompt ResolveFinalPrompt(const ResolvedPromptBase& base, const std::string& deferred_index_segment,
                                   const std::string& model_instructions, const std::string& soul_content,
                                   const std::string& soul_name);

}  // namespace lubancode::agent
