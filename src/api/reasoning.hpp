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

// 目录判定版(模型怪癖矩阵单):"minimal" 只有在目录没把它声明成档位时
// 才当"关"。zai GLM-5.2、openai gpt-5、gemini-3.1-flash-lite 这类模型把
// "minimal" 与 "none" 并列声明——none 是关,minimal 是一档真实的"最浅
// 思考"。不区分的旧判定会把用户选的最低档悄悄翻成"关"(chat/responses
// 落 thinking.type=disabled,gemini 落 includeThoughts=false 连档位都不发),
// 四家 wire 的 toggle/档位翻译一律走这份。
bool ReasoningEffortIsOff(const std::string& effort, const ReasoningConfig& config);

int ReasoningBudgetForEffort(const ReasoningConfig& config, const std::string& effort,
                             int max_tokens);

}  // namespace lubancode::api
