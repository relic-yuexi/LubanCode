// 验收收口编排(轨迹 v3 §4.67 G2):一轮 v3 goal 工作轮收口时,
// "采证入账 -> checkpoint 入账 -> 排评估 -> 采判词 -> 程序门槛 -> 采用
// 与续排意图同一快照提交"的全链在这一只纯编排函数里走完。
//
// 分层职责(§4.67.3 唯一写口铁律):
//   - GoalService 仍是状态唯一写口(本编排只调 BeginEvaluation/
//     CompleteIterationWithEvaluation,不自改快照);
//   - V3Writer 只收事实行(goal.evidence.recorded / goal.checkpoint.
//     recorded;requested/completed/rejected 与验收 messages 由
//     RunGoalEvaluation 的内部请求服务落);
//   - 装配层(wiring)只折材料与模型路由,不碰状态机——本函数可单测,
//     wiring 里的胶水不留逻辑。
//
// 判词采用规则(§4.67.5):
//   - achieved 先过程序门槛(AuditAchievedDecision):缺 required 证据/
//     产物、checkpoint 有 remaining、相关任务未收口——不 achieved,改判
//     continue(overridden_achieved 标),下一步补证据;
//   - continue 与下一轮意图同一笔快照提交(§4.67.6),不出现"continue
//     已生效却忘排下一轮";
//   - evaluator 失败(两坏/超时/请求败)按 evaluator_failed 暂停收口,
//     不默认 achieved,不盲排下一轮。

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "runtime/goal_evaluator.hpp"
#include "runtime/goal_service.hpp"
#include "runtime/goal_types.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::runtime::goal {

// 装配层折好的评估模型参数(模型路由的产物)。
struct GoalEvaluationFlowOptions {
    std::string model;            // 评估小模型(空 = 会话当前模型)
    std::string provider;         // 进账用
    std::string wire;             // 进账用
    std::string reasoning_effort; // 空 = 不带
    int timeout_secs = 120;
    std::int64_t max_tokens = 4096;
};

// 一轮收口的材料(全宿主侧装配;不信 assistant 正文)。
struct GoalCloseoutMaterial {
    std::string parent_turn_id;               // 工作轮 turnId(空 = 拿不到,如实落 null)
    GoalCheckpoint checkpoint;                // 已收口(工具调过或宿主合成,synthesized 标)
    std::vector<GoalEvidence> fresh_evidence;  // 本轮新采(入快照 evidenceRefs)
    std::vector<GoalEvidence> material_evidence;  // 判材料全集(含旧证据,带 fresh/stale)
    // 本轮有写盘级工具落成时,在账旧验证证据要翻 fresh=false 的 id 清单
    //(§4.67.5 证据有效期:改动之后旧验证不再可信)。id 取自快照
    // evidenceRefs,由装配层按证据种类折算(EvidenceStalesOnWrite)。
    std::vector<std::string> evidence_stale_ids;
    std::optional<GoalEvaluation> previous;   // 上一轮判词(G3 起从账投影;G2 可空)
    std::string workspace_summary;            // 一行(git 状态等)
    std::string workspace_baseline;           // 工作区基线指纹(证据引用带;空 = 未记)
    // §4.67.5"相关任务状态":G3 接后台等待后由装配层带;非空同时挡
    // achieved 门槛(后台没收口不封账)。
    std::vector<std::string> wait_task_refs;
    std::int64_t now_ms = 0;
};

// 收口结果(wiring 折 notify/hook 用;状态真值永远在 GoalService)。
struct GoalCloseoutResult {
    bool ok = false;             // false = 流程性失败(账/状态拒),error 带码
    std::string error_code;      // goal.* 稳定码(流程失败时)
    std::string error_message;
    std::string evaluation_id;
    std::string decision;        // continue/achieved/blocked/needs_user/
                                 // evaluator_failed(notify 文案分路用)
    std::string summary;         // 判词摘要(通知用)
    bool overridden_achieved = false;  // evaluator 判 achieved 被程序门槛改判
    std::string override_reason;
    std::string next_work_item_id;      // continue 时下一轮工作项 id
    GoalUsage usage;             // 本次评估的逐次累计(调用方汇总显示)
};

// 全链收口。service.current() 须在执行轮(phase=running)、非终态非停态;
// 内部依次落证据/checkpoint 事实行、BeginEvaluation、RunGoalEvaluation
// (内部请求服务经 writer 留账)、判词门槛、CompleteIterationWithEvaluation。
GoalCloseoutResult CloseGoalIterationWithEvaluation(
    GoalService& service, trajectory::v3::V3Writer& writer, api::Backend& backend,
    const GoalEvaluationFlowOptions& options, const GoalCloseoutMaterial& material,
    const std::atomic<bool>* cancel = nullptr);

}  // namespace lubancode::runtime::goal
