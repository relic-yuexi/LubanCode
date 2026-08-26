// AgentRuntimeProfile 与输出预算三级解析(规格"子代理与 MainAgent 同级"
// 根因一):main 与 general-purpose 子代理吃同一份有效值的几条铁律——
//   1. ResolveOutputBudget 的三级优先级(config > provider > 模型目录);
//   2. BuildMainRuntimeProfile / BuildSubagentRuntimeProfile 构造出的
//      effective max_output_tokens 相同(默认同级,不暗自缩小);
//   3. subagent 段显式覆盖才不同,且来源标明;
//   4. 未写 subagent 段时永不出现 4096 这类编译期魔数。
// 真机回归(vLLM 0.27.1 + qwen3.8-27b)的解析层行为也钉在这里:unset 时
// 请求不带字段,墙在服务端。

#include <doctest/doctest.h>

#include "agent/runtime_profile.hpp"
#include "app/runtime_profile.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"

using namespace lubancode;

TEST_CASE("ResolveOutputBudget:三级优先级 config > provider > 模型目录") {
    using agent::OutputBudgetSource;
    // 三级全缺席:unset。
    const auto none = agent::ResolveOutputBudget(std::nullopt, std::nullopt, std::nullopt);
    CHECK(none.tokens == std::nullopt);
    CHECK(none.source == OutputBudgetSource::Unset);
    CHECK(none.reserve_for_estimate() == agent::kUnsetOutputReserveEstimateTokens);

    // 只有模型目录:目录说了算。
    const auto catalog_only = agent::ResolveOutputBudget(std::nullopt, std::nullopt, std::size_t{4096});
    REQUIRE(catalog_only.tokens.has_value());
    CHECK(*catalog_only.tokens == 4096);
    CHECK(catalog_only.source == OutputBudgetSource::ModelCatalog);

    // provider 压过目录。
    const auto provider_wins = agent::ResolveOutputBudget(std::nullopt, std::size_t{16384}, std::size_t{4096});
    REQUIRE(provider_wins.tokens.has_value());
    CHECK(*provider_wins.tokens == 16384);
    CHECK(provider_wins.source == OutputBudgetSource::ProviderDeclared);

    // config 压过 provider。
    const auto config_wins = agent::ResolveOutputBudget(32768, std::size_t{16384}, std::size_t{4096});
    REQUIRE(config_wins.tokens.has_value());
    CHECK(*config_wins.tokens == 32768);
    CHECK(config_wins.source == OutputBudgetSource::ConfigFile);

    // 声明是 0/负数按缺席算(不拿 0 冒充上限)。
    const auto bogus = agent::ResolveOutputBudget(0, std::size_t{0}, std::nullopt);
    CHECK(bogus.tokens == std::nullopt);
    CHECK(bogus.source == OutputBudgetSource::Unset);
}

TEST_CASE("BuildMainRuntimeProfile:从 config+目录折出 main 的有效份") {
    config::Config config;
    config.wire = config::Wire::ChatCompletions;
    config.model = "qwen3.8-27b";
    config.max_steps_per_turn = 0;
    config.max_context_chars = 123456;
    config.context_window_tokens = 262144;

    config::ModelCatalog catalog;
    config::ModelCatalogEntry entry;
    entry.slug = "qwen3.8-27b";
    entry.max_output_tokens = std::size_t{32768};
    catalog.models.push_back(entry);

    const agent::AgentRuntimeProfile main_profile = app::BuildMainRuntimeProfile(config, &catalog, config.model);
    REQUIRE(main_profile.max_output_tokens.has_value());
    CHECK(*main_profile.max_output_tokens == 32768);
    CHECK(main_profile.max_output_tokens_source == agent::OutputBudgetSource::ModelCatalog);
    CHECK(main_profile.max_steps_per_turn == 0);
    CHECK(main_profile.max_context_chars == 123456);
    CHECK(main_profile.context_window_tokens == 262144);
    CHECK(main_profile.length_continuations == config::kDefaultLengthContinuations);

    // config 显式值压过目录声明。
    config.agent.max_output_tokens = 65536;
    const agent::AgentRuntimeProfile overridden = app::BuildMainRuntimeProfile(config, &catalog, config.model);
    REQUIRE(overridden.max_output_tokens.has_value());
    CHECK(*overridden.max_output_tokens == 65536);
    CHECK(overridden.max_output_tokens_source == agent::OutputBudgetSource::ConfigFile);

    // provider 声明压过目录。
    config.agent.max_output_tokens = std::nullopt;
    config.provider_max_output_tokens = std::size_t{8192};
    const agent::AgentRuntimeProfile provider_declared = app::BuildMainRuntimeProfile(config, &catalog, config.model);
    REQUIRE(provider_declared.max_output_tokens.has_value());
    CHECK(*provider_declared.max_output_tokens == 8192);
    CHECK(provider_declared.max_output_tokens_source == agent::OutputBudgetSource::ProviderDeclared);
}

TEST_CASE("main 与 general-purpose 子代理的有效输出上限相同(规格\"预算\"第 1 条)") {
    // 什么都不声明:两边都是 unset,绝无 4096。
    config::Config config;
    config.wire = config::Wire::ChatCompletions;
    config.model = "qwen3.8-27b";
    const agent::AgentRuntimeProfile main_unset = app::BuildMainRuntimeProfile(config, nullptr, config.model);
    const agent::AgentRuntimeProfile sub_unset = app::BuildSubagentRuntimeProfile(main_unset, config);
    CHECK(main_unset.max_output_tokens == std::nullopt);
    CHECK(sub_unset.max_output_tokens == main_unset.max_output_tokens);
    CHECK(sub_unset.max_output_tokens != 4096);
    CHECK(sub_unset.max_context_chars == main_unset.max_context_chars);
    CHECK(sub_unset.length_continuations == main_unset.length_continuations);
    CHECK(sub_unset.max_steps_per_turn == main_unset.max_steps_per_turn);

    // 声明之后:同轮同值——改 provider/模型目录后两边一起变(同一只装配
    // 函数算出来,不存在子代理另算一套)。
    config.provider_max_output_tokens = std::size_t{32768};
    const agent::AgentRuntimeProfile main_set = app::BuildMainRuntimeProfile(config, nullptr, config.model);
    const agent::AgentRuntimeProfile sub_set = app::BuildSubagentRuntimeProfile(main_set, config);
    REQUIRE(main_set.max_output_tokens.has_value());
    CHECK(sub_set.max_output_tokens == main_set.max_output_tokens);
    CHECK(sub_set.max_output_tokens_source == main_set.max_output_tokens_source);

    // subagent 段显式覆盖:才算不同,且来源标 ConfigFile。
    config.subagent.max_output_tokens = 2048;
    const agent::AgentRuntimeProfile sub_override = app::BuildSubagentRuntimeProfile(main_set, config);
    REQUIRE(sub_override.max_output_tokens.has_value());
    CHECK(*sub_override.max_output_tokens == 2048);
    CHECK(sub_override.max_output_tokens_source == agent::OutputBudgetSource::ConfigFile);
    // 覆盖只动输出上限:步数/窗口/字符安全网/续跑次数照旧继承 main。
    CHECK(sub_override.max_context_chars == main_set.max_context_chars);
    CHECK(sub_override.context_window_tokens == main_set.context_window_tokens);
    CHECK(sub_override.length_continuations == main_set.length_continuations);
}

TEST_CASE("InheritForSubagent:整份继承,不暗自缩小(规格\"产品不变量\")") {
    agent::AgentRuntimeProfile main_profile;
    main_profile.max_output_tokens = 16384;
    main_profile.max_output_tokens_source = agent::OutputBudgetSource::ProviderDeclared;
    main_profile.max_steps_per_turn = 7;
    main_profile.max_context_chars = 999999;
    main_profile.context_window_tokens = 131072;
    main_profile.length_continuations = 2;

    const agent::AgentRuntimeProfile sub = main_profile.InheritForSubagent();
    CHECK(sub.max_output_tokens == main_profile.max_output_tokens);
    CHECK(sub.max_output_tokens_source == main_profile.max_output_tokens_source);
    CHECK(sub.max_steps_per_turn == main_profile.max_steps_per_turn);
    CHECK(sub.max_context_chars == main_profile.max_context_chars);
    CHECK(sub.context_window_tokens == main_profile.context_window_tokens);
    CHECK(sub.length_continuations == main_profile.length_continuations);
}
