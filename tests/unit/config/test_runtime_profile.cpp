// AgentRuntimeProfile 与输出预算三级解析(规格"子代理与 MainAgent 同级"
// 根因一):main 与 general-purpose 子代理吃同一份有效值的几条铁律——
//   1. ResolveOutputBudget 的三级优先级(config > provider > 模型目录);
//   2. BuildMainRuntimeProfile / BuildSubagentRuntimeProfile 构造出的
//      effective max_output_tokens 相同(默认同级,不暗自缩小);
//   3. subagent 段显式覆盖才不同,且来源标明;
//   4. 未写 subagent 段时永不出现 4096 这类编译期魔数。
// 派工单 §四追加一条:能力级声明(目录/provider)超出子任务受控上限的
// 部分收窄(256k 窗 × 128k 目录上限的真机事故),显式配置不受收窄。
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
    CHECK(sub_unset.length_continuations == main_unset.length_continuations);
    CHECK(sub_unset.max_steps_per_turn == main_unset.max_steps_per_turn);

    // 声明之后:同轮同值——改 provider/模型目录后两边一起变(同一只装配
    // 函数算出来,不存在子代理另算一套)。声明值取受控上限内的 16384
    //(默认窗 256k 的上限是 32000):能力声明超上限的收窄是派工单 §四的
    // 另一条规矩,由下面的专项 TEST CASE 钉。
    config.provider_max_output_tokens = std::size_t{16384};
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
    // 覆盖只动输出上限:步数/窗口/续跑次数照旧继承 main。
    CHECK(sub_override.context_window_tokens == main_set.context_window_tokens);
    CHECK(sub_override.length_continuations == main_set.length_continuations);
}

TEST_CASE("InheritForSubagent:整份继承,不暗自缩小(规格\"产品不变量\")") {
    agent::AgentRuntimeProfile main_profile;
    main_profile.max_output_tokens = 16384;
    main_profile.max_output_tokens_source = agent::OutputBudgetSource::ProviderDeclared;
    main_profile.max_steps_per_turn = 7;
    main_profile.context_window_tokens = 131072;
    main_profile.length_continuations = 2;

    const agent::AgentRuntimeProfile sub = main_profile.InheritForSubagent();
    CHECK(sub.max_output_tokens == main_profile.max_output_tokens);
    CHECK(sub.max_output_tokens_source == main_profile.max_output_tokens_source);
    CHECK(sub.max_steps_per_turn == main_profile.max_steps_per_turn);
    CHECK(sub.context_window_tokens == main_profile.context_window_tokens);
    CHECK(sub.length_continuations == main_profile.length_continuations);
}

// 窗口未知(0)的兜底:字节轴裁剪拆除后,token 轴不许在"模型不在目录/窗口
// 查不到"时裸奔——mid-turn 评估、预检硬闸与保命索全按有效窗口取数。
TEST_CASE("EffectiveContextWindowTokens:声明了用声明值,0(未知)落 128k 兜底") {
    agent::AgentRuntimeProfile profile;
    profile.context_window_tokens = 0;
    CHECK(agent::EffectiveContextWindowTokens(profile) == agent::kFallbackContextWindowTokens);
    CHECK(agent::kFallbackContextWindowTokens == 128000);  // 主流模型窗口下限,宁早压不撞墙

    profile.context_window_tokens = 262144;
    CHECK(agent::EffectiveContextWindowTokens(profile) == 262144);
}

TEST_CASE("子任务输出预留受控上限(派工单 §四):能力声明超窗收窄,显式配置不收") {
    // cap 表:窗口未知与大窗封 32k;中窗按 window/8;小窗托底 8k。
    CHECK(agent::SubagentOutputReserveCap(0) == 32768);
    CHECK(agent::SubagentOutputReserveCap(262144) == 32768);
    CHECK(agent::SubagentOutputReserveCap(65536) == 8192);
    CHECK(agent::SubagentOutputReserveCap(16384) == 8192);  // window/8=2048,托底 8k

    // 事故形状:256k 窗 × 128k 目录上限——main 原样(那是模型能力),子任务
    // 收到 32k、来源记 SubagentDefault;显式 subagent 段配置尊重原值。
    config::Config config;
    config.wire = config::Wire::ChatCompletions;
    config.model = "glm-4.6";
    config.context_window_tokens = 262144;

    config::ModelCatalog catalog;
    config::ModelCatalogEntry entry;
    entry.slug = "glm-4.6";
    entry.max_output_tokens = std::size_t{128000};
    catalog.models.push_back(entry);

    const agent::AgentRuntimeProfile main_profile = app::BuildMainRuntimeProfile(config, &catalog, config.model);
    REQUIRE(main_profile.max_output_tokens.has_value());
    CHECK(*main_profile.max_output_tokens == 128000);
    CHECK(main_profile.max_output_tokens_source == agent::OutputBudgetSource::ModelCatalog);

    const agent::AgentRuntimeProfile sub = app::BuildSubagentRuntimeProfile(main_profile, config);
    REQUIRE(sub.max_output_tokens.has_value());
    CHECK(*sub.max_output_tokens == 32768);
    CHECK(sub.max_output_tokens_source == agent::OutputBudgetSource::SubagentDefault);
    // 收窄只动输出上限:窗口/续跑次数照旧继承。
    CHECK(sub.context_window_tokens == main_profile.context_window_tokens);
    CHECK(sub.length_continuations == main_profile.length_continuations);

    // 显式配置(用户手笔)哪怕超上限也不收。
    config.subagent.max_output_tokens = 128000;
    const agent::AgentRuntimeProfile sub_explicit = app::BuildSubagentRuntimeProfile(main_profile, config);
    REQUIRE(sub_explicit.max_output_tokens.has_value());
    CHECK(*sub_explicit.max_output_tokens == 128000);
    CHECK(sub_explicit.max_output_tokens_source == agent::OutputBudgetSource::ConfigFile);

    // 能力声明在上限之内(如 16k)不动,来源保持原样。注意改 catalog 里
    // 那份(栈上的 entry 是 push_back 时的副本)。
    catalog.models[0].max_output_tokens = std::size_t{16384};
    const agent::AgentRuntimeProfile main_small = app::BuildMainRuntimeProfile(config, &catalog, config.model);
    config.subagent.max_output_tokens = std::nullopt;
    const agent::AgentRuntimeProfile sub_small = app::BuildSubagentRuntimeProfile(main_small, config);
    REQUIRE(sub_small.max_output_tokens.has_value());
    CHECK(*sub_small.max_output_tokens == 16384);
    CHECK(sub_small.max_output_tokens_source == agent::OutputBudgetSource::ModelCatalog);
}
