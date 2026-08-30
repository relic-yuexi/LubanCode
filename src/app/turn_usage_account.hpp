// 整轮逐步 usage 的分角色记账(骨架拆解反弹·问题 2 自 TerminalSessionController
// 的 RunSessionTurn 拆出):把 TurnUsageStats::steps 每枚折成 api::Usage 记进
// 模型路由的台账(normal 档——普通用户回合的全笔消耗;compact/标题/抽取的
// 后台采样在各自路径另记,不混进来)。model 为空的步用发起请求时的当前模型
// 名兜底(与原逻辑一字不差)。
#pragma once

#include <string>
#include <vector>

#include "agent/model_router.hpp"       // ModelUsageLedger
#include "runtime/turn_runtime.hpp"     // StepUsageRecord

namespace lubancode::app {

// steps:本轮攒下的逐笔用量;fallback_model:step.model 为空时兜底的名字
//(controller 传 *current_model)。duration_ms 记 0(未计时,照记不猜)。
void RecordTurnUsageSteps(lubancode::agent::ModelUsageLedger& ledger,
                          const std::vector<lubancode::runtime::StepUsageRecord>& steps,
                          const std::string& fallback_model);

}  // namespace lubancode::app
