// 上下文压缩(0.21.x 起,0.31.x 分层化第一期):history 长了(手动 /compact,
// ContextTracker 测出占用过 80%,或 AgentLoop 在模型请求前测出 projected
// overflow 触发 mid-turn 压缩)时,把历史送给模型换一份浓缩"存档"回来,
// 顶替掉老对话——跟 agent/context.hpp 的 TrimHistory(按字节硬切,不管
// 语义、纯粹丢内容)是两条路子:这里靠模型读懂对话再总结,保留任务目标/
// 关键决策/文件路径/代码要点/未完成事项,是主防线;TrimHistory 仍留着当
// 兜底的硬安全网,两者互不影响、互不依赖。
//
// 第一期堵的洞:
//   - 压缩模型自己的窗口单独算预算;窗口装不下时明确拒绝,不静默截史。
//   - 摘要末尾必须带可解析的 JSON manifest(目标/约束/待办/下一步);
//     manifest 缺失、解析不动、或"必须守恒的未完成事项"漏了一项,一律
//     拒收——旧 history 原样不动。
//   - 热区按 token 预算保留(不再固定"只留最后一轮")。

#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "api/backend.hpp"
#include "api/types.hpp"

namespace lubancode::agent {

// 压缩预算:按压缩模型自己的窗口算,不拿主模型 context_window 冒充。
struct CompactBudget {
    // 压缩模型的上下文窗口(token)。nullopt = 窗口未知——不做窗口拦截,
    // 但调用方应向用户说明"未按窗口校验",不许假装核过。
    std::optional<std::size_t> window_tokens;
    // 摘要输出预留(token),与请求里的 max_tokens 对齐。
    std::size_t output_reserve_tokens = 4096;
    // 协议与安全余量(token):system 指令、wire 包装、流式协议开销。
    std::size_t protocol_headroom_tokens = 2048;
};

// 压缩请求的可用输入预算:window - 输出预留 - 协议余量。窗口未知给
// nullopt(没有预算可用,只能不拦)。
std::optional<std::size_t> CompactInputBudget(const CompactBudget& budget);

// 摘要必须附上的机器 manifest。Markdown 栏目给模型读,manifest 给程序验,
// 两者由模型按同一份结构产出(指令里钉死键名),不许各写一份互相打架。
struct CompactManifest {
    std::string goal;                    // 当前任务目标(一句话)
    std::vector<std::string> constraints;  // 用户明示约束/禁止/验收句
    std::vector<std::string> open_items;   // 未完成事项(逐字收编活动待办)
    std::string next_action;              // 下一步要执行的准确动作
};

// manifest 守恒校验结果。ok=false 时 failures 逐条列拒收原因(人话,
// 直接给用户看)。
struct CompactValidation {
    bool ok = false;
    std::vector<std::string> failures;
};

// 守恒校验(纯函数):
//   - goal 非空;
//   - required_open_items(活动 plan/todo 的未完成条目)每一条都必须在
//     open_items 里逐字在场(空白归一后比对)——摘要漏一项 pending 就拒收,
//     旧 history 不动。
CompactValidation ValidateCompactManifest(const CompactManifest& manifest,
                                          const std::vector<std::string>& required_open_items);

// 从摘要正文里解析 manifest:取末尾最后一个 ```json 围栏块,按
// goal/constraints/open_items/next_action 键取值。缺围栏块、坏 JSON、
// 缺必填键,给 nullopt。
std::optional<CompactManifest> ParseCompactManifest(const std::string& summary_text);

// 压缩参数。
struct CompactOptions {
    std::string focus;  // /compact <重点>:额外重点保留一段
    CompactBudget budget{};
    // 必须守恒的未完成事项(活动 todo 的 pending/in_progress 条目原文)。
    // 空表 = 没有可钉的待办,manifest 只做结构校验。
    std::vector<std::string> required_open_items;
};

// 一次压缩的产物:archive 消息(顶进新历史用) + 解析出的 manifest。
struct CompactSummary {
    api::Message archive;
    CompactManifest manifest;
};

// 热区 token 预算:压缩后从最后一轮用户输入往前按整轮收,收满预算为止;
// 最后一轮无论多大都整轮保留(最新用户消息绝不丢,TrimHistory 是它后面
// 的安全网)。不再按固定轮数留——一轮可能只有十字,也可能拖着五万 token
// 的工具结果。
constexpr std::size_t kDefaultHotZoneTokens = 12000;

// 拼出压缩后的完整新历史:archive 消息并入热区第一条 user 消息开头(不
// 单独成一条,免得相邻两条 user 违反 Anthropic 角色交替),其后是按
// hot_zone_tokens 预算保留的最近若干完整轮。history 里找不到真正的用户
// 文本输入时,新历史里只有 archive 这一条。
std::vector<api::Message> BuildCompactedHistory(const std::vector<api::Message>& history,
                                                const api::Message& archive,
                                                std::size_t hot_zone_tokens = kDefaultHotZoneTokens);

// 向模型请求一次历史压缩。指令要求:固定六栏 Markdown 存档 + 末尾一枚
// ```json manifest;required_open_items 非空时,指令明说这些条目必须逐字
// 进 open_items。
// 发送前先按预算核窗口:估算输入(指令 + 全份 history,统一 token 口径)
// 超过 CompactInputBudget 时直接拒绝(不发请求),错误信息写明窗口数与
// 估算体积——分块压缩是下一期的活,这期绝不静默截史。
// 成功条件(全过才收,任一不过返回错误、调用方旧 history 不动):
//   1. 流式请求成功、无 StreamError;
//   2. Markdown 正文剥空白后不少于 40 个 UTF-8 码点(防 prefill 残次品);
//   3. 末尾有可解析的 JSON manifest;
//   4. ValidateCompactManifest 通过(目标非空、待办守恒)。
std::expected<CompactSummary, api::Error> Compact(api::Backend& backend, const std::string& model,
                                                  const std::vector<api::Message>& history,
                                                  const CompactOptions& options);

}  // namespace lubancode::agent
