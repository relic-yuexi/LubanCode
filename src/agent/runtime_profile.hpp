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
#include <optional>
#include <string>
#include <utility>

namespace lubancode::agent {

// max_context_chars 的 agent 层默认值:与 config::kDefaultMaxContextChars
// 数值一致、各自单独定义(依赖只许单向,见 config.hpp 同款注释)。
inline constexpr std::size_t kProfileDefaultMaxContextChars = 600000;

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
    // history 字符安全网(裁剪阈值)。
    std::size_t max_context_chars = kProfileDefaultMaxContextChars;
    // 上下文窗口 token 数(0 = 未知,不做 mid-turn 评估)。
    std::size_t context_window_tokens = 0;
    // max_tokens 打断在思考段时的自动续跑次数(规格根因四;0 = 不续)。
    int length_continuations = kDefaultLengthContinuations;

    // 便捷:main 的 profile 派生子代理默认份——输出上限等 inherited 字段
    // 原样照抄(默认同级,不暗自缩小);覆盖由调用方在拷贝上显式改并明写
    // 来源(subagent 段的显式收窄/放宽)。
    AgentRuntimeProfile InheritForSubagent() const { return *this; }
};

}  // namespace lubancode::agent
