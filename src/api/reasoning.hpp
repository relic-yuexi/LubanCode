#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lubancode::api {

// 落线方言(模型协议兼容实录矩阵单 P1):中立能力在具体 wire 上的精确
// 形状。catalog 明写,serializer 只按方言落线;不按模型名猜,不拿 provider
// 品牌硬编码。方言是"形状表"——要不要落线仍由模型档案(supports_toggle/
// supports_effort/budget 区间)与各家 wire 的明文优先级决定。
//
// 没有方言(empty)时 serializer 走 legacy 兼容路径(generic toggle ->
// thinking.type 那套),该路径视为 unverified,等 fixture 补齐后收紧。
struct ReasoningWireDialect {
    // 开关怎么写:none(不发) | enable_thinking_bool(顶层布尔) |
    // thinking_type({"thinking":{"type":...}})| include_thoughts
    // (Gemini thinkingConfig.includeThoughts,budget=0 表关)
    std::string toggle = "none";
    // 开/关的值:enable_thinking_bool 用 "true"/"false";thinking_type 用
    // "enabled"/"adaptive"/"disabled"。
    std::string toggle_on = "true";
    std::string toggle_off = "false";
    // 档位怎么写:空 = none | "reasoning.effort" | "reasoning_effort"
    // (chat 家顶层参数,名字可由 effort_param 覆写) | "output_config.effort"
    // (Claude 新式) | "thinkingLevel" (Gemini)
    std::string effort_path;
    std::string effort_param;  // effort_path=reasoning_effort 时的顶层参数名
    // 预算怎么写:空 = none | "thinking_budget"(chat 顶层) |
    // "thinking.budget_tokens"(anthropic) | "thinkingBudget"(Gemini)
    std::string budget_path;
    // 思考增量字段:reasoning_content | reasoning | reasoning_details |
    // anthropic_thinking_block | gemini_thought_part(账面记录,供诊断与
    // 真机矩阵对账;事件解析按 provider 声明的字段名走)
    std::string delta;
    // 思考回传:never | tool_episode | always
    std::string replay = "never";
    std::string replay_field;  // chat 家回传字段名,默认 reasoning_content
    // anthropic 家签名必须随思考块回传(signature_delta)。
    bool signature_required = false;
    // 这套方言有没有实证(手册明文/真机实测/官方文档);聚合转发端默认
    // false——形状对得上不等于服务端真认。
    bool verified = false;

    bool empty() const {
        return toggle == "none" && effort_path.empty() && budget_path.empty() && delta.empty() &&
               replay == "never" && replay_field.empty() && !signature_required;
    }
};

// 模型推理能力的中立描述。它只说模型会什么，不夹带任何 wire JSON。
// 各协议在请求落地时，按这份描述写 reasoning/thinking/output_config。
struct ReasoningConfig {
    std::vector<std::string> supported_efforts;
    bool supports_effort = false;
    bool supports_toggle = false;
    std::optional<int> budget_min;
    std::optional<int> budget_max;
    // "effort" / "budget"；空串表示由 wire 和 controls 自行判断。
    // (gemini/anthropic 家的档位走向提示;方言对象才是精确形状。)
    std::string wire_dialect;
    // 落线方言(catalog provider 级声明,模型级可逐字段覆写)。空 =
    // legacy 兼容路径(视为 unverified)。
    ReasoningWireDialect dialect;
    // 目录明说这模型不吃推理参数(ccmoon 巡检单 P2:只出图的模型,
    // image-generation 且无 reasoning 能力):true 时四家 wire 一律停发
    // 推理档位/开关——用户的档位偏好(current_think)不动,切回推理模型
    // 照旧生效。与 empty()(未声明,legacy 照发)分家:不猜没声明的模型。
    bool declined = false;

    bool empty() const {
        return supported_efforts.empty() && !supports_effort && !supports_toggle && !budget_min.has_value() &&
               !budget_max.has_value() && wire_dialect.empty() && dialect.empty() && !declined;
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
