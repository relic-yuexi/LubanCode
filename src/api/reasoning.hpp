#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lubancode::api {

// 模型推理能力的中立描述。它只说模型会什么，不夹带任何 wire JSON。
// 各协议在请求落地时，按这份描述写 reasoning/thinking/output_config。
struct ReasoningConfig {
    std::vector<std::string> supported_efforts;
    bool supports_effort = false;
    bool supports_toggle = false;
    std::optional<int> budget_min;
    std::optional<int> budget_max;
    // "effort" / "budget"；空串表示由 wire 和 controls 自行判断。
    std::string wire_dialect;

    bool empty() const {
        return supported_efforts.empty() && !supports_effort && !supports_toggle && !budget_min.has_value() &&
               !budget_max.has_value() && wire_dialect.empty();
    }
};

std::string LowerReasoningEffort(std::string effort);
bool ReasoningEffortIsOff(const std::string& effort);
int ReasoningBudgetForEffort(const ReasoningConfig& config, const std::string& effort,
                             int max_tokens);

}  // namespace lubancode::api
