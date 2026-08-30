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
// 跨 Turn 保留式思考的中立意图(Kimi 保留式思考单 P1)。与 replay 是两笔
// 账:replay 管客户端怎样序列化已有的 ThinkingBlock;history 管用户要不要
// 显式请求服务端把跨 Turn 的思考纳入本轮推理。它是中立意图——不直接写死
// thinking.keep;Chat/Kimi 方言把 All 落成 thinking.keep="all" 并把 replay
// 升为 Always;不支持的模型在配置入口明报,不静默忽略。
enum class ReasoningHistoryMode {
    ProviderDefault,  // 不显式请求跨 Turn 保留(模型方言自带的缺省形状)
    All,              // 显式请求完整保留(方言声明了请求控制才落线)
};

// 模型对"跨 Turn 保留式思考"的能力档(由方言推导,不按模型名特判):
// 同一枚 Kimi 可能经直连、聚合端、本地 vLLM 出站,协议责任随实际绑定的
// provider 声明的方言走——/think history 的切换与切模型重校验都先问它。
enum class ReasoningHistorySupport {
    None,           // 无方言(自定义旧 provider)或方言未声明历史控制且
                    // replay != always:不支持,选 all 当场明报
    ServerFixed,    // replay=always 且无请求控制:服务端固定开启,关不掉,
                    // wire 上也没有请求字段可发
    RequestControl, // 方言声明 history_control=thinking_keep:可选开(K2.6
                    // 的 thinking.keep;开了 replay 同步升 Always)
};

struct ReasoningWireDialect {
    // 开关怎么写:none(不发) | enable_thinking_bool(顶层布尔) |
    // thinking_type({"thinking":{"type":...}})| include_thoughts
    // (Gemini thinkingConfig.includeThoughts,budget=0 表关) |
    // chat_template_kwargs_enable_thinking(嵌套
    // body["chat_template_kwargs"]["enable_thinking"],vLLM/qwen 模板开关——
    // 本机 vLLM 0.27.1 实测顶层 enable_thinking 被无视,唯一生效路)
    std::string toggle = "none";
    // 开/关的值:enable_thinking_bool 与 chat_template_kwargs_enable_thinking
    // 用 "true"/"false";thinking_type 用 "enabled"/"adaptive"/"disabled"。
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
    // 服务端历史控制形状(P1):空 = 无请求字段 | thinking_keep(该家 wire
    // 认一枚可选的"请求保留"字段)。与 replay 是两笔账——replay 是客户端
    // 序列化已有思考块的责任;history_control 是服务端跨 Turn 保留的请求
    // 开关。catalog 模型级可覆写 provider 级,"none" 显式清掉继承。
    std::string history_control;
    // thinking_keep 形状下请求保留时写的值。空 = "all"(Kimi 官方文档只给
    // 了这一个值;声明成字段是给别家将来留口子,不是替厂商编参数)。
    std::string history_all_value;
    // anthropic 家签名必须随思考块回传(signature_delta)。
    bool signature_required = false;
    // 这套方言有没有实证(手册明文/真机实测/官方文档);聚合转发端默认
    // false——形状对得上不等于服务端真认。
    bool verified = false;

    bool empty() const {
        return toggle == "none" && effort_path.empty() && budget_path.empty() && delta.empty() &&
               replay == "never" && replay_field.empty() && history_control.empty() &&
               history_all_value.empty() && !signature_required;
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

// 能力档裁决(纯函数,单测钉):方言声明了 history_control=thinking_keep
// -> RequestControl;没声明但 replay=always -> ServerFixed(服务端固定
// 开启,没有请求字段);其余(含无方言的 legacy provider)-> None。
ReasoningHistorySupport ReasoningHistorySupportFor(const ReasoningConfig& config);

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
