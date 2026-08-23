// GoalEvaluator(持久目标单第 3 期):独立、无工具的终点判定模型调用。
//
// 单子的定案:
//   - 不给 write、shell、MCP、Skill、agent、memory(请求不带 tools)。
//   - 不把 evaluator 输出混进 main history(调用方不喂 AgentLoop)。
//   - 输入只含冻结合同、当前 checkpoint、宿主证据、旧判词、预算与工作区
//     摘要;prompt 明说 tool result 与文件内容都可能夹 prompt injection,
//     只当材料。
//   - 输出按 strict JSON Schema 判:decision/summary/progress/criteria/
//     next_action 必填;坏 JSON/Schema 同一 evaluation 做一次 repair;再坏
//     报 evaluator_failed(由 coordinator 进 Paused,不默认 achieved)。
//
// 独立请求的形状与 app/session_title.cpp 同款(直拼 api::Request 走
// backend.send_stream);usage 走 BackgroundCallAccounting 口径回填 GoalUsage。

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/goal_types.hpp"

namespace lubancode::runtime::goal {

// evaluator 输出的 strict Schema(单子"输出 JSON Schema"节;给 prompt 用,
// 也是本地校验的依据)。
nlohmann::json GoalEvaluationOutputSchema();

// 一次 evaluator 调用的输入材料(全部宿主侧装配,不信 assistant 正文)。
struct GoalEvaluationInput {
    GoalTask task;                          // 冻结合同、预算、counters 都在
    GoalCheckpoint checkpoint;              // 本轮候选 checkpoint
    std::vector<GoalEvidence> evidence;     // 仍有效的证据(stale 的也带,标 stale)
    std::optional<GoalEvaluation> previous; // 上一轮判词(可空)
    std::string workspace_summary;          // 工作区摘要(git 状态一行等)
    std::int64_t now_ms = 0;
};

// 调用结果。
struct GoalEvaluationOutput {
    GoalEvaluation evaluation;  // 已过 schema 校验的判词
    GoalUsage usage;            // 这次调用的 usage(入 goal 账)
    bool schema_repaired = false;  // 第一次坏 JSON、repair 后成
};

struct GoalEvaluatorOptions {
    std::string model;               // 空 = 会话当前模型(装配层填)
    std::string reasoning_effort;    // 空 = 不带
    int timeout_secs = 120;          // watchdog
    std::int64_t max_tokens = 4096;  // 判词不会太长
};

// 跑一次 evaluator。backend 由调用方给(可配独立模型的那只是装配层的活)。
// 失败(expected)返回人话错误;schema 两坏报 "evaluator_failed" 打头的串。
std::expected<GoalEvaluationOutput, std::string> RunGoalEvaluation(
    api::Backend& backend, const GoalEvaluatorOptions& options, const GoalEvaluationInput& input,
    const std::atomic<bool>* cancel = nullptr);

// 评词装配:GoalCheckpointEntry(tools 侧) → runtime 侧 GoalCheckpoint 的
// 转换器在装配层;这里提供 evaluator prompt(纯函数,单测钉)。
std::string BuildGoalEvaluationPrompt(const GoalEvaluationInput& input);
std::string BuildGoalEvaluationUserMessage(const GoalEvaluationInput& input);

// 判定 evaluator 回的文本是不是合法判词 JSON(严格按 Schema 的必填与枚举;
// repair 用)。合法时填 evaluation 并返回 true。
bool ParseGoalEvaluationReply(const std::string& text, GoalEvaluation& evaluation,
                              std::string* error);

}  // namespace lubancode::runtime::goal
