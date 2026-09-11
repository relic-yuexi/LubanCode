// GoalEvaluator(持久目标单第 3 期;轨迹 v3 §4.67 G2 收编):独立、无工具
// 的终点判定模型调用,验收改走 v3 内部请求服务。
//
// 单子的定案:
//   - 不给 write、shell、MCP、Skill、agent、memory(请求不带 tools)。
//   - 不把 evaluator 输出混进 main history(内部回合 purpose=goal_
//     evaluation,默认折叠,不进 main contextChain)。
//   - 输入只含冻结合同、当前 checkpoint、宿主证据、旧判词、预算与工作区
//     摘要;prompt 明说 tool result 与文件内容都可能夹 prompt injection,
//     只当材料。
//   - 输出按 strict JSON Schema 判:decision/summary/progress/criteria/
//     next_action 必填;坏 JSON/Schema 同一 evaluation 做一次 repair;再坏
//     报 evaluator_failed(由收口编排进 Paused,不默认 achieved)。
//
// G2 的四笔加固(§4.67.5/§4.67.9 缺口清单):
//   1. 内部请求服务:ledger scope 在场时,评估请求经 v3 账留痕——验收
//      system/user/assistant 真消息进账(purpose=goal_evaluation,parentTurnId
//      回指工作轮)、model.request.prepared 带 inputMessageRefs、每次请求
//      各记一笔(usage 逐次累加,不只取末次)。模型调用只能经宿主调度并
//      留账,不再裸跑。
//   2. 严格判词校验:criteria 恰好覆盖冻结合同(缺/重复拒)、证据引用只认
//      本次材料的证据 id(跨 goal/编造拒)、continue 必给可执行下一步;
//      校验不过视同解析失败——一次 repair 后仍错报 evaluator_failed。
//   3. 证据有效期与完成门槛:achieved 的采用资格由 AuditAchievedDecision
//      程序判(required 全 pass 且各配 fresh 证据、required_artifacts 齐备、
//      无 remaining、相关任务收口);缺证据不 achieved,改判 continue。
//   4. 组合取消:外部取消令牌与内部超时同时生效(旧口径"外部链在场时
//      超时不抢断"已废——watchdog 循环两头盯,任一先到都断)。
//
// 独立请求走 agent::SampleModel 原语(骨架拆解批一·病四:与起名/压缩/
// 抽取同一条采样路);usage 累计沿 BackgroundCallAccounting 口径(五项
// 累加、usage_reported 只置不撤)。

#pragma once

#include <atomic>
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

namespace lubancode::trajectory::v3 {
class V3Writer;  // 内部请求服务的账面(writer.hpp 完整合同)
}

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
    // §4.67.5 验收输入固定清单含"相关任务状态"。G2 只显示(空 = 无相关
    // 任务);G3 接后台等待后,非空 waitTaskRefs 同时挡 achieved 门槛
    //(后台没收口不封账)。
    std::vector<std::string> wait_task_refs;
    std::int64_t now_ms = 0;
};

// v3 内部请求服务的账面 scope(§4.67.5"统一内部请求路径")。writer 为空
// = v1 旧路(不进账,v2 场 coordinator 兼容);非空 = 评估的每次模型请求
// 经 writer 落 v3 账(requested/messages/prepared/assistant/completed 或
// rejected),材料版本在此冻结。
struct GoalEvaluationLedgerScope {
    trajectory::v3::V3Writer* writer = nullptr;
    std::string goal_id;
    std::string iteration_id;
    std::string evaluation_id;              // eval-<n>,调用方发(绑定一次状态提交)
    std::string parent_turn_id;             // 回指工作轮(空 = 拿不到,如实落 null)
};

// 调用结果。
struct GoalEvaluationOutput {
    GoalEvaluation evaluation;  // 已过 schema 与材料校验的判词
    GoalUsage usage;            // 这次调用(含 repair 轮)的累计 usage(入 goal 账)
    bool schema_repaired = false;  // 第一次坏 JSON、repair 后成
    // 账面回执(ledger scope 在场时填;v1 旧路恒空)。
    std::string evaluation_turn_id;   // 验收内部回合 turnId(goaleval-turn-<n>)
    std::vector<std::string> request_ids;      // 逐次请求(初判 + repair 各一枚)
    std::vector<std::string> message_ids;      // 本回合落的 system/user/assistant
    std::string evaluation_message_id;         // 判词 assistant 的 messageId
    std::string evidence_set_hash;             // 本次材料的证据集 hash(hex64)
};

struct GoalEvaluatorOptions {
    std::string model;               // 空 = 会话当前模型(装配层填)
    std::string reasoning_effort;    // 空 = 不带
    std::string provider;            // 进账用(装配层从评估端点填;v1 旧路可空)
    std::string wire;                // 同上
    int timeout_secs = 120;          // watchdog(与外部取消组合生效)
    std::int64_t max_tokens = 4096;  // 判词不会太长
    GoalEvaluationLedgerScope ledger;  // v3 内部请求服务接线(空 = 不进账)
};

// 跑一次 evaluator。backend 由调用方给(可配独立模型的那只是装配层的活)。
// 失败(expected)返回人话错误;schema/材料校验两坏报 "evaluator_failed"
// 打头的串(调用方进 Paused,不默认 achieved)。
std::expected<GoalEvaluationOutput, std::string> RunGoalEvaluation(
    api::Backend& backend, const GoalEvaluatorOptions& options, const GoalEvaluationInput& input,
    const std::atomic<bool>* cancel = nullptr);

// ---- 严格判词校验与完成门槛(纯函数,单测钉) ------------------------------

// 判词对材料的严格校验(§4.67.5"严格拒绝"):
//   - criteria 恰好覆盖冻结合同的 criterionId 集(缺一枚拒、多一枚拒、
//     判词内重复拒——重复/缺失 criterionId 都是无效判词);
//   - 证据引用只认本次材料 evidence 清单里的 id(跨 goal/编造拒);
//   - continue 必给非空 next_action(可执行下一步);blocked 必带
//     blocker_key、needs_user 必带 question(Parse 已拦,这里对材料再核)。
// 返回空串 = 过;非空 = 人话原因(喂 repair;repair 后仍错 → evaluator_
// failed,不采用)。
std::string ValidateEvaluationAgainstMaterial(const GoalEvaluationInput& material,
                                              const GoalEvaluation& evaluation);

// achieved 的程序门槛审计(§4.67.5 完成门槛;宿主采用前核,不信判词):
//   - decision 非 achieved:eligible 恒 false(failures 空,不是缺口);
//   - 每条 required criterion 都 pass 且各配至少一枚 fresh(非 stale、非
//     truncated)证据;
//   - required_artifacts 每项都有对应证据(facts 或 kind 对得上;缺者报);
//   - checkpoint.remaining 空(有未完成项不封账——"执行模型自称完成、
//     todo 全勾"不顶用);
//   - wait_task_refs 空(G3 起后台没收口不 achieved;G2 输入侧恒空)。
struct GoalAchievementAudit {
    bool eligible = false;
    std::vector<std::string> failures;  // 人话缺口清单(按序,审计与改判用)
};
GoalAchievementAudit AuditAchievedDecision(const GoalEvaluationInput& material,
                                           const GoalEvaluation& evaluation);

// 评词装配:GoalCheckpointEntry(tools 侧) → runtime 侧 GoalCheckpoint 的
// 转换器在装配层;这里提供 evaluator prompt(纯函数,单测钉)。
std::string BuildGoalEvaluationPrompt(const GoalEvaluationInput& input);
std::string BuildGoalEvaluationUserMessage(const GoalEvaluationInput& input);

// 判定 evaluator 回的文本是不是合法判词 JSON(严格按 Schema 的必填与枚举;
// repair 用)。合法时填 evaluation 并返回 true。
bool ParseGoalEvaluationReply(const std::string& text, GoalEvaluation& evaluation,
                              std::string* error);

// 证据集 hash:材料 evidence 清单的规范字节(canonical json,按 id 序)的
// sha256——requested 事件的 evidenceSetHash 与采用核对账用同一枚。
std::string GoalEvidenceSetHash(const std::vector<GoalEvidence>& evidence);

}  // namespace lubancode::runtime::goal
