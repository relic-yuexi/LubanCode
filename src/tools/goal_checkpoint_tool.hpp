// 内置 "goal_checkpoint" 工具(持久目标单第 2 期):goal execution turn 的
// 检查点落账口。
//
// 单子的定案:
//   - 只给 goal execution turn 动态露面;普通 turn、subagent、MCP 看不见
//     (装配层只在 goal iteration 的 registry 里注册)。
//   - 执行模型最多申请验收(ready_for_evaluation),不能封账——Achieved
//     归 evaluator + 硬门槛,不归这只工具。
//   - 只能引用本 goal、本 iteration 已产生的 evidence id;不认识的 id
//     直接 domain error。
//   - status=blocked 必须带 blocker_key;needs_user 必须带 question。
//   - 工具本身无项目副作用,但要先写 iteration event;写盘失败便不收
//     (Result 带 stable error_code,宿主收口时不会拿它当 achieved)。
//
// 状态存会话级:goal 装配层造一份 GoalCheckpointState(shared_ptr),一轮可
// 多次调用、最后一枚为候选;每枚都经 callbacks 落 trace。

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "tools/tool.hpp"

namespace lubancode::tools {

// checkpoint 的四档表态(与 runtime::goal::CheckpointStatus 一一对应;这里
// 独立枚举,不引 runtime 头——tools 不反向依赖 runtime 的老规矩)。
enum class GoalCheckpointStatus {
    Progress,
    ReadyForEvaluation,
    Blocked,
    NeedsUser,
};

// 工具侧的检查点(结构化落账的原材料;转成 runtime::goal::GoalCheckpoint
// 由装配层做)。
struct GoalCheckpointEntry {
    GoalCheckpointStatus status = GoalCheckpointStatus::Progress;
    std::string summary;
    std::vector<std::string> completed;
    std::vector<std::string> remaining;
    std::string next_action;
    std::vector<std::string> evidence_ids;
    std::optional<std::string> blocker_key;
    std::optional<std::string> question;
};

// 会话级状态:本轮各次调用都记,最后一枚是候选。
struct GoalCheckpointState {
    std::vector<GoalCheckpointEntry> entries;  // 本 iteration 的全部调用
    std::string goal_id;      // 本 iteration 的 goal(空 = 不在 goal turn)
    std::string iteration_id;
    std::vector<std::string> valid_evidence_ids;  // 宿主采证后喂进来的白名单

    // 这一轮有没有至少一枚合法 checkpoint(没调过 = 宿主合成 missing)。
    bool HasCheckpoint() const { return !entries.empty(); }
    // 候选 = 最后一枚。
    std::optional<GoalCheckpointEntry> Candidate() const {
        if (entries.empty()) return std::nullopt;
        return entries.back();
    }
};

class GoalCheckpointTool : public Tool {
public:
    explicit GoalCheckpointTool(std::shared_ptr<GoalCheckpointState> state) : state_(std::move(state)) {}

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }
    // 无项目副作用,只写 iteration event;EffectClass 声明只读本地。
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
    tools::Idempotency idempotency() const override { return tools::Idempotency::Idempotent; }
    Result execute(const nlohmann::json& input) override;

private:
    std::shared_ptr<GoalCheckpointState> state_;
};

}  // namespace lubancode::tools
