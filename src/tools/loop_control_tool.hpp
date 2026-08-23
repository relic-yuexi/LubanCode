// 内置 "loop_control" 工具(loop 单第 4 期):scheduled tick 里模型声明
// 完成的窄口。
//
// 单子的定案:
//   - 只在 scheduled tick 的动态 tool set 中露出;普通用户 turn 调不到
//     (装配层只在 loop turn 的 scope 里放行,普通轮看不见)。
//   - 只能操作当前 task id,不能停别人的 loop;伪造 task id 报 scope
//     error。
//   - complete 是正常终态;pause 可用于需要用户处理的情况。
//   - 它不改项目、不需 permission,但写 scheduler/session 账。
//   - 调用完成后本 tick 可继续收最终答话,下一拍不再排。
//
// 状态存会话级:loop 装配层造一份 LoopControlState(shared_ptr),tick 开
// turn 前灌 task_id,turn 收口后清。

#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "tools/tool.hpp"

namespace lubancode::tools {

// 会话级状态:当前 scheduled tick 属于哪只 task(空 = 不在 loop turn,
// 工具明拒)。
struct LoopControlState {
    std::string task_id;  // 当前 tick 的 task(空 = 不在 loop turn)
    bool complete_requested = false;  // 本 tick 是否声明过 complete
    bool pause_requested = false;     // 本 tick 是否声明过 pause
};

class LoopControlTool : public Tool {
public:
    explicit LoopControlTool(std::shared_ptr<LoopControlState> state) : state_(std::move(state)) {}

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }
    // 不改项目(只写 scheduler 账);EffectClass 声明只读本地。
    tools::EffectClass effect_class() const override { return tools::EffectClass::ReadOnlyLocal; }
    tools::Idempotency idempotency() const override { return tools::Idempotency::Idempotent; }
    Result execute(const nlohmann::json& input) override;

private:
    std::shared_ptr<LoopControlState> state_;
};

}  // namespace lubancode::tools
