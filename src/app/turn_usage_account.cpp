// RecordTurnUsageSteps 的实现(骨架拆解反弹·问题 2):折算与记账自
// TerminalSessionController::RunSessionTurn 逐字搬来,行为一字未改。
#include "app/turn_usage_account.hpp"

#include "api/types.hpp"  // api::Usage

namespace lubancode::app {

void RecordTurnUsageSteps(lubancode::agent::ModelUsageLedger& ledger,
                          const std::vector<lubancode::runtime::StepUsageRecord>& steps,
                          const std::string& fallback_model) {
    for (const auto& step : steps) {
        api::Usage step_usage;
        step_usage.input_tokens = step.input_tokens;
        step_usage.output_tokens = step.output_tokens;
        step_usage.cache_read_tokens = step.cache_read_tokens;
        step_usage.cache_creation_tokens = step.cache_creation_tokens;
        step_usage.output_reasoning_tokens = step.reasoning_tokens;
        ledger.Record(lubancode::agent::ModelRole::Normal,
                      step.model.empty() ? fallback_model : step.model, step_usage,
                      /*duration_ms=*/0, step.reported);
    }
}

}  // namespace lubancode::app
