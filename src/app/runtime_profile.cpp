#include "app/runtime_profile.hpp"

#include "cli/i18n.hpp"

namespace lubancode::app {

agent::AgentRuntimeProfile BuildMainRuntimeProfile(const config::Config& config,
                                                    const config::ModelCatalog* catalog,
                                                    const std::string& current_model) {
    (void)current_model;  // 只用于目录条目查询;model 本身归 RequestProfile(批四)
    agent::AgentRuntimeProfile profile;
    // 输出上限三级解析:config 显式 > provider 声明 > 模型目录。
    // unset(nullopt)是合法收场:chat/responses 请求不带字段交服务端默认,
    // anthropic 必填由 client 落公开兜底——不再有写死的 4096。
    std::optional<std::size_t> catalog_declared;
    if (catalog != nullptr) {
        if (const auto* entry = catalog->FindBySlug(current_model); entry != nullptr) {
            catalog_declared = entry->max_output_tokens;
        }
    }
    const agent::OutputBudget budget =
        agent::ResolveOutputBudget(config.agent.max_output_tokens, config.provider_max_output_tokens,
                                    catalog_declared);
    profile.max_output_tokens = budget.tokens;
    profile.max_output_tokens_source = budget.source;
    profile.max_steps_per_turn = config.max_steps_per_turn;
    profile.max_context_chars = config.max_context_chars;
    profile.context_window_tokens = config.context_window_tokens;
    profile.length_continuations = config.agent.length_continuations;
    return profile;
}

agent::AgentRuntimeProfile BuildSubagentRuntimeProfile(const agent::AgentRuntimeProfile& main_profile,
                                                        const config::Config& config) {
    agent::AgentRuntimeProfile profile = main_profile.InheritForSubagent();
    // 唯一改写:用户显式写的 subagent.max_output_tokens。nullopt = 继承
    // main 的有效值(含 unset),不落回任何编译期魔数(规格"兼容与配置")。
    if (config.subagent.max_output_tokens.has_value()) {
        profile.max_output_tokens = config.subagent.max_output_tokens;
        profile.max_output_tokens_source = agent::OutputBudgetSource::ConfigFile;
    }
    // 受控收窄(派工单 §四):继承来的能力级声明(模型目录/provider)超出
    // 子任务上限的部分收掉——输出预留不该一刀切吃掉半扇窗口,普通子任务
    // 的工具轮用不了那么多输出,报告阶段真要长文再显式放宽。显式配置
    //(ConfigFile)不收,用户手笔尊重原值。
    if (profile.max_output_tokens.has_value() &&
        profile.max_output_tokens_source != agent::OutputBudgetSource::ConfigFile) {
        const int cap = agent::SubagentOutputReserveCap(profile.context_window_tokens);
        if (*profile.max_output_tokens > cap) {
            profile.max_output_tokens = cap;
            profile.max_output_tokens_source = agent::OutputBudgetSource::SubagentDefault;
        }
    }
    return profile;
}

std::string OutputBudgetSourceText(agent::OutputBudgetSource source, bool subagent_override) {
    switch (source) {
        case agent::OutputBudgetSource::ConfigFile:
            return subagent_override ? cli::tr("config.output_source.config_subagent")
                                     : cli::tr("config.output_source.config");
        case agent::OutputBudgetSource::ProviderDeclared:
            return cli::tr("config.output_source.provider");
        case agent::OutputBudgetSource::ModelCatalog:
            return cli::tr("config.output_source.catalog");
        case agent::OutputBudgetSource::SubagentDefault:
            return cli::tr("config.output_source.subagent_default");
        case agent::OutputBudgetSource::Unset:
            return cli::tr("config.output_source.unset");
    }
    return cli::tr("config.output_source.unset");
}

}  // namespace lubancode::app
