// 模型目录(models.json)解析与应用逻辑:全部走纯函数
// (ParseModelCatalogJson / FindBySlug / ThinkLevelHintLines /
// ThinkLevelDeclared / ComputeCatalogApplication),不真读磁盘。
// 核心规矩:目录是锦上添花——坏 JSON/坏条目警告跳过不崩,缺失 = 空目录,
// 一切回退现状,零破坏。

#include <doctest/doctest.h>

#include <string>

#include "agent/prompts.hpp"
#include "config/model_catalog.hpp"

using namespace lubancode;

namespace {

// 一条五脏俱全的 MiniMax-M3 条目,多个用例复用。
const char* kFullCatalogJson = R"({
  "models": [
    {
      "slug": "MiniMax-M3",
      "display_name": "MiniMax M3",
      "description": "MiniMax 旗舰模型",
      "default_think": "high",
      "supported_think_levels": [
        {"effort": "none", "description": "关闭思考,直答"},
        {"effort": "high", "description": "开启 Adaptive Thinking"}
      ],
      "base_instructions": "你是鲁班座下的 M3 试验机。",
      "context_window": "1m",
      "supports_parallel_tool_calls": true,
      "input_modalities": ["text", "image"],
      "truncation_policy": "auto"
    }
  ]
})";

}  // namespace

// ---------------------------------------------------------------------------
// 解析:完整条目
// ---------------------------------------------------------------------------

TEST_CASE("ParseModelCatalogJson: 完整条目逐字段解出,含暂不启用的三个字段") {
    const auto catalog = config::ParseModelCatalogJson(kFullCatalogJson, "models.json");
    CHECK(catalog.warnings.empty());
    REQUIRE(catalog.models.size() == 1);

    const auto& entry = catalog.models[0];
    CHECK(entry.slug == "MiniMax-M3");
    CHECK(entry.display_name == "MiniMax M3");
    CHECK(entry.description == "MiniMax 旗舰模型");
    CHECK(entry.default_think == "high");
    REQUIRE(entry.supported_think_levels.size() == 2);
    CHECK(entry.supported_think_levels[0].effort == "none");
    CHECK(entry.supported_think_levels[0].description == "关闭思考,直答");
    CHECK(entry.supported_think_levels[1].effort == "high");
    CHECK(entry.base_instructions == "你是鲁班座下的 M3 试验机。");
    REQUIRE(entry.context_window_tokens.has_value());
    CHECK(*entry.context_window_tokens == 1000000);
    // 三个"先解析存储不启用"的字段也要真的存下来。
    REQUIRE(entry.supports_parallel_tool_calls.has_value());
    CHECK(*entry.supports_parallel_tool_calls == true);
    REQUIRE(entry.input_modalities.size() == 2);
    CHECK(entry.input_modalities[0] == "text");
    CHECK(entry.truncation_policy == "auto");
}

TEST_CASE("ParseModelCatalogJson: 只有 slug 的最小条目,其余字段全部缺省") {
    const auto catalog = config::ParseModelCatalogJson(R"({"models":[{"slug":"foo"}]})", "models.json");
    CHECK(catalog.warnings.empty());
    REQUIRE(catalog.models.size() == 1);
    const auto& entry = catalog.models[0];
    CHECK(entry.slug == "foo");
    CHECK(entry.display_name.empty());
    CHECK(entry.default_think.empty());
    CHECK(entry.supported_think_levels.empty());
    CHECK(entry.base_instructions.empty());
    CHECK_FALSE(entry.context_window_tokens.has_value());
    CHECK_FALSE(entry.supports_parallel_tool_calls.has_value());
    CHECK(entry.input_modalities.empty());
    CHECK(entry.truncation_policy.empty());
}

// ---------------------------------------------------------------------------
// 解析:context_window 三种写法
// ---------------------------------------------------------------------------

TEST_CASE("ParseModelCatalogJson: context_window 认 1m / 512k / 裸数字三种写法") {
    const auto catalog = config::ParseModelCatalogJson(R"({"models":[
        {"slug": "a", "context_window": "1m"},
        {"slug": "b", "context_window": "512k"},
        {"slug": "c", "context_window": 200000}
    ]})", "models.json");
    CHECK(catalog.warnings.empty());
    REQUIRE(catalog.models.size() == 3);
    CHECK(*catalog.models[0].context_window_tokens == 1000000);
    CHECK(*catalog.models[1].context_window_tokens == 512000);
    CHECK(*catalog.models[2].context_window_tokens == 200000);
}

// ---------------------------------------------------------------------------
// 解析:坏 JSON / 坏条目——警告跳过,不崩、好条目照收
// ---------------------------------------------------------------------------

TEST_CASE("ParseModelCatalogJson: 整体不是合法 JSON → 空目录 + 一条警告") {
    const auto catalog = config::ParseModelCatalogJson("{oops", "models.json");
    CHECK(catalog.models.empty());
    REQUIRE(catalog.warnings.size() == 1);
    CHECK(catalog.warnings[0].find("models.json") != std::string::npos);
}

TEST_CASE("ParseModelCatalogJson: 顶层不是 {\"models\":[...]} → 空目录 + 警告") {
    CHECK(config::ParseModelCatalogJson(R"([1,2,3])", "p").models.empty());
    CHECK(config::ParseModelCatalogJson(R"([1,2,3])", "p").warnings.size() == 1);
    CHECK(config::ParseModelCatalogJson(R"({"nope": []})", "p").warnings.size() == 1);
    CHECK(config::ParseModelCatalogJson(R"({"models": "x"})", "p").warnings.size() == 1);
}

TEST_CASE("ParseModelCatalogJson: 坏条目跳过、好条目照收,一条坏条目一条警告") {
    const auto catalog = config::ParseModelCatalogJson(R"({"models":[
        {"slug": "good-1"},
        {"display_name": "缺 slug"},
        {"slug": ""},
        {"slug": "bad-window", "context_window": "abc"},
        {"slug": "bad-levels", "supported_think_levels": [{"description": "缺 effort"}]},
        {"slug": "bad-type", "default_think": 42},
        "不是 object",
        {"slug": "good-2", "default_think": "low"}
    ]})", "models.json");
    REQUIRE(catalog.models.size() == 2);
    CHECK(catalog.models[0].slug == "good-1");
    CHECK(catalog.models[1].slug == "good-2");
    CHECK(catalog.warnings.size() == 6);
    // 警告里带下标,能定位到是哪一条坏了。
    CHECK(catalog.warnings[0].find("models[1]") != std::string::npos);
}

// ---------------------------------------------------------------------------
// FindBySlug
// ---------------------------------------------------------------------------

TEST_CASE("FindBySlug: 精确命中返回条目,不在目录返回 nullptr") {
    const auto catalog = config::ParseModelCatalogJson(kFullCatalogJson, "models.json");
    const auto* entry = catalog.FindBySlug("MiniMax-M3");
    REQUIRE(entry != nullptr);
    CHECK(entry->display_name == "MiniMax M3");
    CHECK(catalog.FindBySlug("gpt-x") == nullptr);
    CHECK(catalog.FindBySlug("") == nullptr);
    // slug 是 API 模型名,大小写敏感——精确匹配,不做归一化。
    CHECK(catalog.FindBySlug("minimax-m3") == nullptr);
}

// ---------------------------------------------------------------------------
// /think(/effort)候选与档位声明
// ---------------------------------------------------------------------------

TEST_CASE("ThinkLevelHintLines: 有声明档位时一档一行带描述,没条目/没声明时为空") {
    const auto catalog = config::ParseModelCatalogJson(kFullCatalogJson, "models.json");
    const auto lines = config::ThinkLevelHintLines(catalog.FindBySlug("MiniMax-M3"));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].find("none") != std::string::npos);
    CHECK(lines[0].find("关闭思考") != std::string::npos);
    CHECK(lines[1].find("high") != std::string::npos);
    CHECK(lines[1].find("Adaptive Thinking") != std::string::npos);

    // 不在目录:nullptr → 空,调用方回退现状提示。
    CHECK(config::ThinkLevelHintLines(nullptr).empty());
    // 在目录但没声明档位:同样为空。
    const auto minimal = config::ParseModelCatalogJson(R"({"models":[{"slug":"foo"}]})", "p");
    CHECK(config::ThinkLevelHintLines(minimal.FindBySlug("foo")).empty());
}

TEST_CASE("ThinkLevelDeclared: 表内档位认(大小写不敏感),表外不认") {
    const auto catalog = config::ParseModelCatalogJson(kFullCatalogJson, "models.json");
    const auto* entry = catalog.FindBySlug("MiniMax-M3");
    REQUIRE(entry != nullptr);
    CHECK(config::ThinkLevelDeclared(*entry, "high"));
    CHECK(config::ThinkLevelDeclared(*entry, "High"));
    CHECK(config::ThinkLevelDeclared(*entry, "NONE"));
    CHECK_FALSE(config::ThinkLevelDeclared(*entry, "medium"));
    CHECK_FALSE(config::ThinkLevelDeclared(*entry, ""));
}

TEST_CASE("ThinkLevelExtraBody: 按档位取模型私有参数，大小写不敏感") {
    const auto catalog = config::ParseModelCatalogJson(
        R"({"models":[{"slug":"m","supported_think_levels":[{"effort":"high","extra_body":{"thinking":{"type":"enabled"}}}]}]})",
        "p");
    const auto body = config::ThinkLevelExtraBody(catalog.FindBySlug("m"), "HIGH");
    CHECK(body["thinking"]["type"] == "enabled");
    CHECK(config::ThinkLevelExtraBody(catalog.FindBySlug("m"), "none").empty());
}

// ---------------------------------------------------------------------------
// ComputeCatalogApplication:启动/切换时应用什么
// ---------------------------------------------------------------------------

TEST_CASE("ComputeCatalogApplication: 命中目录且用户没显式配 → 应用 default_think/context_window/base_instructions") {
    const auto catalog = config::ParseModelCatalogJson(kFullCatalogJson, "models.json");
    const auto apply = config::ComputeCatalogApplication(catalog, "MiniMax-M3",
                                                          /*think_explicitly_configured=*/false,
                                                          /*window_explicitly_configured=*/false);
    CHECK(apply.in_catalog);
    REQUIRE(apply.think.has_value());
    CHECK(*apply.think == "high");
    REQUIRE(apply.context_window_tokens.has_value());
    CHECK(*apply.context_window_tokens == 1000000);
    CHECK(apply.base_instructions == "你是鲁班座下的 M3 试验机。");
}

TEST_CASE("ComputeCatalogApplication: 用户显式配过的字段不动,目录压不过用户") {
    const auto catalog = config::ParseModelCatalogJson(kFullCatalogJson, "models.json");
    const auto apply = config::ComputeCatalogApplication(catalog, "MiniMax-M3",
                                                          /*think_explicitly_configured=*/true,
                                                          /*window_explicitly_configured=*/true);
    CHECK(apply.in_catalog);
    CHECK_FALSE(apply.think.has_value());
    CHECK_FALSE(apply.context_window_tokens.has_value());
    // base_instructions 不冲突任何用户配置,照样给。
    CHECK_FALSE(apply.base_instructions.empty());
}

TEST_CASE("ComputeCatalogApplication: 不在目录 → 什么都不应用,base_instructions 空串(该清掉)") {
    const auto catalog = config::ParseModelCatalogJson(kFullCatalogJson, "models.json");
    const auto apply = config::ComputeCatalogApplication(catalog, "gpt-x", false, false);
    CHECK_FALSE(apply.in_catalog);
    CHECK_FALSE(apply.think.has_value());
    CHECK_FALSE(apply.context_window_tokens.has_value());
    CHECK(apply.base_instructions.empty());
}

TEST_CASE("ComputeCatalogApplication: 条目声明了什么才应用什么——没写 default_think/context_window 就不动") {
    const auto catalog = config::ParseModelCatalogJson(
        R"({"models":[{"slug":"foo","base_instructions":"只有指令"}]})", "p");
    const auto apply = config::ComputeCatalogApplication(catalog, "foo", false, false);
    CHECK(apply.in_catalog);
    CHECK_FALSE(apply.think.has_value());
    CHECK_FALSE(apply.context_window_tokens.has_value());
    CHECK(apply.base_instructions == "只有指令");
}

// ---------------------------------------------------------------------------
// base_instructions 注入后的系统提示结构
// ---------------------------------------------------------------------------

TEST_CASE("WithModelInstructions: 独立段追加在末尾,人格段/环境段原样保留,互不覆盖") {
    const std::string base = agent::BuildSystemPrompt("D:/work", "你是自定义人格。", "");
    const std::string with = agent::WithModelInstructions(base, "你是 M3 试验机。");

    // 原提示(人格段 + 环境段)一个字不少地在前头。
    CHECK(with.compare(0, base.size(), base) == 0);
    CHECK(with.find("你是自定义人格。") != std::string::npos);
    CHECK(with.find("- 工作目录: D:/work") != std::string::npos);  // 0.19.x:环境段改成运行环境清单行
    // 模型专属段接在后面,带来源说明,收尾是 base_instructions 本身。
    const std::size_t seg_pos = with.find("模型专属指令");
    REQUIRE(seg_pos != std::string::npos);
    CHECK(seg_pos > base.size());
    CHECK(with.find("你是 M3 试验机。") > seg_pos);
}

TEST_CASE("WithModelInstructions: base_instructions 为空,原样返回一个字符不多") {
    const std::string base = agent::BuildSystemPrompt("D:/work");
    CHECK(agent::WithModelInstructions(base, "") == base);
}
