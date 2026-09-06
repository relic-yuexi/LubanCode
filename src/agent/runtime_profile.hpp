// AgentRuntimeProfile(规格"子代理与 MainAgent 同级",根因一):main、
// general-purpose 子代理、后台子代理、单发模式共用的运行策略——输出预算、
// 上下文预算、压缩窗口、步数、length 续跑次数都从这一份不可变值里拿,
// 不再各构造点各写一枚魔数(旧版 AgentLoop 默认 4096、interactive_session
// 显式 4096、AgentTool::RunTask 又 4096,三处各藏一堵矮墙)。
//
// 依赖方向:这只头文件不牵扯 config/(config 不该被 agent 反向依赖的同款
// 规矩),只认纯数据;解析(app 层拿 Config + 目录算出这份 profile)在
// app/runtime_profile.hpp。

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace lubancode::agent {

// 窗口未知(context_window_tokens == 0)时的 token 轴兜底窗口。依据:取
// 2026 年在售主力模型窗口的下限——GPT 系 128k 起、Claude 系 200k 起、
// Gemini 系 1M,本地开源模型按 rope 缩放常态也配 128k。兜底取"主流下限"
// 而不是中位:兜底偏小只会让保护早触发(多压一次,安全侧);偏大会让
// 保护晚触发(真撞墙,危险侧)——窗口未知时宁可保守。字节轴裁剪拆除后,
// 模型不在目录、窗口查不到的会话不能再裸奔,mid-turn 评估、预检硬闸与
// 保命索全按这只口取窗口。
inline constexpr std::size_t kFallbackContextWindowTokens = 128000;

// 输出上限 unset 时的 projected 估算值(仅用于"下一请求放不放得下"的
// 上下文压力估算,不发请求):unset 交服务端默认,服务端默认多半比这个
// 数大,拿它估是保守侧(早压总比撞墙好)。与
// config::kDefaultRequiredMaxOutputTokens / api::kRequiredMaxOutputTokensFallback
// 同值同注释规矩,三处须一起改。
inline constexpr int kUnsetOutputReserveEstimateTokens = 8192;

// length 续跑(max_tokens 打断在思考段)的默认次数:实测(本机 vLLM
// 0.27.1 + qwen3.8-27b,规格"现场")一次续跑足以让模型收束思考交出
// 工具调用或检查点;更多次就是在替用户烧 token,显式配置才能加。
inline constexpr int kDefaultLengthContinuations = 1;

// 成本刹车的默认软线百分比(真机实测 P2-1/P2-6):任一硬线(步数/时间/
// token)跨过这个比例,就注入一次"请基于现有证据收尾"的系统级催办。
// 80 是产品缺省,入参/配置可显式改;0 = 只留硬闸,不催。
inline constexpr int kDefaultBudgetSoftPercent = 80;

// 输出上限最终从哪一级来(/config、/context、agent 查看态写明来源)。
// 从低到高:模型目录 < provider 声明 < 配置文件(项目级压全局在 config
// 层已合并完,到这里只剩一级)。子代理的显式收窄/放宽也算配置文件级,
// 来源句里另注"subagent 段"。
enum class OutputBudgetSource {
    Unset,           // 三级都没声明:chat/responses 请求不带字段,交服务端默认;
                     // anthropic 必填,由 client 落公开兜底(api::kRequiredMaxOutputTokensFallback)
    ModelCatalog,    // models.json 条目声明
    ProviderDeclared,// provider 目录/配置声明
    ConfigFile,      // agent.max_output_tokens / subagent.max_output_tokens
    // 能力级声明(目录/provider)被子任务的受控默认收窄(派工单 §四):
    // 值不再是原声明,查账文案要说清来路。
    SubagentDefault,
};

// 输出上限解析结果:tokens 为 nullopt = unset(语义见上)。
struct OutputBudget {
    std::optional<int> tokens;
    OutputBudgetSource source = OutputBudgetSource::Unset;

    // projected 估算用的那个数:set 了用声明值,unset 用保守估计——只是
    // 算"下一请求 + 输出预留放不放得下"的那一笔,不是请求字段。
    int reserve_for_estimate() const { return tokens.value_or(kUnsetOutputReserveEstimateTokens); }
};

// 输出上限三级解析(纯函数,单测钉):config 显式 > provider 声明 > 模型
// 目录声明,全缺席 = unset。main 与子代理都走这一只函数,不各算各的。
inline OutputBudget ResolveOutputBudget(std::optional<int> config_value,
                                         std::optional<std::size_t> provider_declared,
                                         std::optional<std::size_t> catalog_declared) {
    if (config_value.has_value() && *config_value > 0) {
        return {*config_value, OutputBudgetSource::ConfigFile};
    }
    if (provider_declared.has_value() && *provider_declared > 0) {
        return {static_cast<int>(*provider_declared), OutputBudgetSource::ProviderDeclared};
    }
    if (catalog_declared.has_value() && *catalog_declared > 0) {
        return {static_cast<int>(*catalog_declared), OutputBudgetSource::ModelCatalog};
    }
    return {std::nullopt, OutputBudgetSource::Unset};
}

// 子任务输出预留的受控上限(派工单 §四):模型目录/provider 声明的是模型
// 能力上限,不是任务默认——原样继承会把小半扇窗口当每次请求的输出预留
//(256k 窗口 × 128k 目录上限的真机事故:输入 13 万 + 预留 12.8 万,预检
// 永远过不去)。子任务的预留封顶 = clamp(window/8, 8k, 32k);窗口未知给
// 32k。显式配置(ConfigFile)不收——那是用户的手笔,尊重原值。
inline int SubagentOutputReserveCap(std::size_t window_tokens) {
    std::size_t cap = window_tokens == 0 ? 32768 : window_tokens / 8;
    if (cap < 8192) {
        cap = 8192;
    }
    if (cap > 32768) {
        cap = 32768;
    }
    return static_cast<int>(cap);
}

// 一份不可变运行策略。AgentLoop 与子代理(AgentTool::RunTask)从这里拿
// 全部预算类参数;调用方各声明自己覆盖什么,其余继承 main 的有效值。
// model 不在这里(骨架拆解批四·病十一其一):请求整形归
// api::RequestProfile(AGENT 皮上那份),运行档案只管预算,不再两处各存
// 一份靠手工 if 同步。
struct AgentRuntimeProfile {
    // 输出上限(nullopt = unset,语义见 OutputBudget)。请求构造时透传给
    // api::Request::max_tokens;anthropic 必填,由 client 兜底。
    std::optional<int> max_output_tokens;
    OutputBudgetSource max_output_tokens_source = OutputBudgetSource::Unset;
    // 一个 turn 内的步数上限(0 = 不限步)。
    int max_steps_per_turn = 0;
    // 上下文窗口 token 数(0 = 未知;有效值经 EffectiveContextWindowTokens
    // 落兜底,见上)。token 轴是唯一的上下文裁剪决策者。
    std::size_t context_window_tokens = 0;
    // max_tokens 打断在思考段时的自动续跑次数(规格根因四;0 = 不续)。
    int length_continuations = kDefaultLengthContinuations;

    // ---- 成本刹车(真机实测 P2-1/P2-6)------------------------------------
    // 三根硬线,AgentLoop::Run 在每个步顶查:任一断线就停循环,把到限为止
    // 的部分结果连同"哪根线断的"交回(分型 budget_exhausted,不静默丢)。
    // 0 = 不设。步数硬线就是上面的 max_steps_per_turn,不另立字段。
    int max_wall_secs = 0;            // 整轮墙钟硬线(秒)
    std::int64_t max_total_tokens = 0;  // 累计 token 硬线(完整输入 + 输出)
    // 软线百分比(1~100;0 = 不催):任一硬线跨过 soft% 注入一次催办。
    // 派发层可按任务收窄,main 默认 0(主回合不催,子代理才吃成本闸)。
    int budget_soft_percent = 0;

    // 便捷:是否设了任何一根成本硬线(步数/时间/token)。
    bool HasCostBudget() const {
        return max_steps_per_turn > 0 || max_wall_secs > 0 || max_total_tokens > 0;
    }

    // 便捷:main 的 profile 派生子代理默认份——输出上限等 inherited 字段
    // 原样照抄(默认同级,不暗自缩小);覆盖由调用方在拷贝上显式改并明写
    // 来源(subagent 段的显式收窄/放宽)。
    AgentRuntimeProfile InheritForSubagent() const { return *this; }
};

// 软线取值(纯函数,单测钉):hard>0 且 percent>0 时 = hard*percent/100
// (至少 1,预算再小软线也得是个正数);任一为 0 返回 0(没有软线这回事)。
inline std::int64_t BudgetSoftLine(std::int64_t hard, int percent) {
    if (hard <= 0 || percent <= 0) {
        return 0;
    }
    const std::int64_t line = hard * percent / 100;
    return line < 1 ? 1 : line;
}

// 有效窗口:profile 声明了用声明值,0(未知)落 kFallbackContextWindowTokens
// 兜底。loop 的 mid-turn 评估、token 预检硬闸与 ShrinkOversizedToolResults
// 保命索全走这一只口,不许各拿各的魔数。
inline std::size_t EffectiveContextWindowTokens(const AgentRuntimeProfile& profile) {
    return profile.context_window_tokens != 0 ? profile.context_window_tokens : kFallbackContextWindowTokens;
}

}  // namespace lubancode::agent
