// 模型目录(models.json)解析与应用逻辑:全部走纯函数
// (ParseModelCatalogJson / FindBySlug / ThinkLevelHintLines /
// ThinkLevelDeclared / ComputeCatalogApplication),不真读磁盘。
// 核心规矩:目录是锦上添花——坏 JSON/坏条目警告跳过不崩,缺失 = 空目录,
// 一切回退现状,零破坏。

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// ---------------------------------------------------------------------------
// 活列表选择落痕(RememberModelChoiceInCatalog):唯一动真磁盘的用例,
// 只碰临时目录。
// ---------------------------------------------------------------------------

TEST_CASE("ParseModelCatalogJson: 条目可带 provider_id(活列表落痕写的)") {
    const auto catalog = config::ParseModelCatalogJson(
        R"({"models": [
            {"slug": "gpt-x", "provider_id": "local", "display_name": "GPT X"},
            {"slug": "bare-one"}
        ]})",
        "models.json");
    CHECK(catalog.warnings.empty());
    REQUIRE(catalog.models.size() == 2);
    CHECK(catalog.models[0].provider_id == "local");
    CHECK(catalog.models[1].provider_id.empty());  // 留空 = 全局覆盖,照旧
}

TEST_CASE("RememberModelChoiceInCatalog: 新建、幂等、保字段、坏 JSON 拒写") {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) /
                     ("lubancode_models_trace_" + std::to_string(::rand()));
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / "models.json").string();
    const std::string other = (dir / "other.json").string();

    // 文件不存在:从头建,写入 slug/provider_id/display_name。
    CHECK(config::RememberModelChoiceInCatalog(path, "local", "gpt-5.6-sol", "GPT 5.6 Sol").has_value());
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const auto catalog = config::ParseModelCatalogJson(buffer.str(), path);
        REQUIRE(catalog.models.size() == 1);
        CHECK(catalog.models[0].slug == "gpt-5.6-sol");
        CHECK(catalog.models[0].provider_id == "local");
        CHECK(catalog.models[0].display_name == "GPT 5.6 Sol");
    }
    // 幂等:同 slug 同 provider 再写,条目数不变,原字段一个不少。
    CHECK(config::RememberModelChoiceInCatalog(path, "local", "gpt-5.6-sol", "换个名也不许改").has_value());
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const auto catalog = config::ParseModelCatalogJson(buffer.str(), path);
        REQUIRE(catalog.models.size() == 1);
        CHECK(catalog.models[0].display_name == "GPT 5.6 Sol");  // 幂等:别重写
    }
    // 保字段:同 slug 不同 provider 追加新条目,已有条目上手工配的字段
    // (default_think 这类)不许冲掉。
    CHECK(config::RememberModelChoiceInCatalog(path, "local", "glm-5", "GLM 5").has_value());
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"({"models": [{"slug": "glm-5", "provider_id": "local", "default_think": "high"}]})";
    }
    CHECK(config::RememberModelChoiceInCatalog(path, "local", "glm-5", "GLM 5").has_value());
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const auto catalog = config::ParseModelCatalogJson(buffer.str(), path);
        REQUIRE(catalog.models.size() == 1);
        CHECK(catalog.models[0].default_think == "high");  // 已有字段原样
    }
    // 同 slug 别家:追加,不覆盖本家的。
    CHECK(config::RememberModelChoiceInCatalog(path, "ccmoon", "glm-5", "GLM 5").has_value());
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        const auto catalog = config::ParseModelCatalogJson(buffer.str(), path);
        REQUIRE(catalog.models.size() == 2);
    }
    // 坏 JSON:报错不写,原文件字节不动。
    {
        std::ofstream out(other, std::ios::binary | std::ios::trunc);
        out << "{ not json";
    }
    CHECK_FALSE(config::RememberModelChoiceInCatalog(other, "local", "x", "X").has_value());
    {
        std::ifstream in(other, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        CHECK(buffer.str() == "{ not json");
    }

    std::filesystem::remove_all(dir, ec);
}



// ---------------------------------------------------------------------------
// ccmoon 真机巡检单 P1:端点能力分类(ClassifyModelEndpoint)与
// capabilities 解析。判词边界:只说"当前 wire 大概率不通",不判模型死刑,
// 也不判中转的 Realtime 路由通不通。
// ---------------------------------------------------------------------------

TEST_CASE("ClassifyModelEndpoint:Realtime 三路认得出,音频与普通模型不误伤") {
    // 名字兜底:中转活列表里的名字(ccmoon 的 gpt-4o-realtime-preview
    // 不在内置目录)靠 "realtime" 通名认。
    CHECK(config::ClassifyModelEndpoint(nullptr, "gpt-4o-realtime-preview") ==
          config::ModelEndpointKind::Realtime);
    CHECK(config::ClassifyModelEndpoint(nullptr, "gpt-realtime-2") == config::ModelEndpointKind::Realtime);

    // 目录能力键:realtime=true(内置目录给 gpt-realtime-* 挂的凭据)。
    config::ModelCatalogEntry realtime_entry;
    realtime_entry.slug = "some-rt";
    realtime_entry.capabilities["realtime"] = true;
    CHECK(config::ClassifyModelEndpoint(&realtime_entry, "some-rt") == config::ModelEndpointKind::Realtime);

    // 音频模型不吃"realtime"判词(巡检单:Audio 没实跑,不判死刑)。
    CHECK(config::ClassifyModelEndpoint(nullptr, "gpt-4o-audio-preview") == config::ModelEndpointKind::Standard);
    config::ModelCatalogEntry audio_entry;
    audio_entry.slug = "gpt-audio";
    audio_entry.capabilities["audio-recognition"] = true;
    audio_entry.capabilities["audio-generation"] = true;
    CHECK(config::ClassifyModelEndpoint(&audio_entry, "gpt-audio") == config::ModelEndpointKind::Standard);

    // 普通文本/工具模型零误伤。
    CHECK(config::ClassifyModelEndpoint(nullptr, "gpt-5.6-luna") == config::ModelEndpointKind::Standard);
    CHECK(config::ClassifyModelEndpoint(nullptr, "minimax-m3") == config::ModelEndpointKind::Standard);

    // 只出图的模型(catalog image-generation 且不吃 reasoning)。
    config::ModelCatalogEntry image_entry;
    image_entry.slug = "gpt-image-1-5";
    image_entry.capabilities["image-generation"] = true;
    image_entry.capabilities["image"] = true;
    CHECK(config::ClassifyModelEndpoint(&image_entry, "gpt-image-1-5") == config::ModelEndpointKind::ImageGen);
}

TEST_CASE("models.json 条目可写 capabilities(布尔键值,坏值按坏条目拒)") {
    const auto parsed = config::ParseModelCatalogJson(
        R"({"models":[)"
        R"({"slug":"rt-x","capabilities":{"realtime":true}},)"
        R"({"slug":"plain-y"})"
        R"(]})",
        "test-models.json");
    REQUIRE(parsed.models.size() == 2);
    CHECK(parsed.models[0].capabilities.at("realtime") == true);
    CHECK(parsed.models[1].capabilities.empty());
    CHECK(config::ClassifyModelEndpoint(&parsed.models[0], "rt-x") == config::ModelEndpointKind::Realtime);

    const auto bad = config::ParseModelCatalogJson(
        R"({"models":[{"slug":"bad","capabilities":{"realtime":"yes"}}]})", "test-models.json");
    CHECK(bad.models.empty());
    CHECK(bad.warnings.size() == 1);
}

TEST_CASE("目录声明不吃推理(declined):出图模型停发档位,回来恢复") {
    // 巡检单 P2:catalog 的 image-generation 且无 reasoning 能力 → 推理
    // 档案 declined,四家 wire 停发推理参数;普通模型档案不动(legacy
    // 照发,不猜)。reasoning.effort 是否仍发由各家 request 单测对账。
    config::ModelCatalogEntry image_gen;
    image_gen.capabilities["image-generation"] = true;
    CHECK(config::ClassifyModelEndpoint(&image_gen, "img-x") == config::ModelEndpointKind::ImageGen);
}

TEST_CASE("ClassifyThinkOffDeclaration:always_think/off_unsupported 两键认得出,没写不猜") {
    // MiniCPM5 巡检单 P1:目录声明"思考关不掉"的两枚键,/think none 与
    // /doctor effort 都拿它亮"此端点未证实可关"。没写 = Unknown,不猜。
    CHECK(config::ClassifyThinkOffDeclaration(nullptr) == config::ThinkOffDeclaration::Unknown);

    config::ModelCatalogEntry plain;
    plain.slug = "plain";
    CHECK(config::ClassifyThinkOffDeclaration(&plain) == config::ThinkOffDeclaration::Unknown);

    config::ModelCatalogEntry always;
    always.capabilities["always_think"] = true;
    CHECK(config::ClassifyThinkOffDeclaration(&always) == config::ThinkOffDeclaration::DeclaredUnsupported);

    config::ModelCatalogEntry off_unsupported;
    off_unsupported.capabilities["off_unsupported"] = true;
    CHECK(config::ClassifyThinkOffDeclaration(&off_unsupported) ==
          config::ThinkOffDeclaration::DeclaredUnsupported);

    // 写了但为假 = 没声明(目录明说"能关"不在此键的语义里,留 Unknown)。
    config::ModelCatalogEntry falsy;
    falsy.capabilities["always_think"] = false;
    CHECK(config::ClassifyThinkOffDeclaration(&falsy) == config::ThinkOffDeclaration::Unknown);

    // models.json 手写与内置目录同形状,一条 JSON 就能落声明(真机巡检的
    // 本地端就走这条路:MiniCPM5-1B 不在内置目录,用户条目自写)。
    const auto parsed = config::ParseModelCatalogJson(
        R"({"models":[{"slug":"MiniCPM5-1B","capabilities":{"off_unsupported":true,"image":false}}]})",
        "test-models.json");
    REQUIRE(parsed.models.size() == 1);
    CHECK(config::ClassifyThinkOffDeclaration(&parsed.models[0]) ==
          config::ThinkOffDeclaration::DeclaredUnsupported);
    CHECK(config::ClassifyImageInputSupport(&parsed.models[0]) == config::ImageInputSupport::TextOnly);
}

TEST_CASE("ClassifyImageInputSupport:input_modalities 与 capabilities 合判,未知放行") {
    // MiniCPM5 巡检单 P2:已知纯文本模型在 /image 发送前拦住,未知才允许
    // 试探。三条判据各钉一册:modalities 列表、capabilities.image 真假、
    // 都没写 = Unknown。
    CHECK(config::ClassifyImageInputSupport(nullptr) == config::ImageInputSupport::Unknown);

    config::ModelCatalogEntry bare;
    CHECK(config::ClassifyImageInputSupport(&bare) == config::ImageInputSupport::Unknown);

    config::ModelCatalogEntry declared_text;
    declared_text.input_modalities = {"text"};
    CHECK(config::ClassifyImageInputSupport(&declared_text) == config::ImageInputSupport::TextOnly);

    config::ModelCatalogEntry declared_multi;
    declared_multi.input_modalities = {"text", "image"};
    CHECK(config::ClassifyImageInputSupport(&declared_multi) == config::ImageInputSupport::Multimodal);

    // 大小写不敏感;只列了别的模态(音频一类)算图片未声明。
    config::ModelCatalogEntry upper;
    upper.input_modalities = {"Text", "IMAGE"};
    CHECK(config::ClassifyImageInputSupport(&upper) == config::ImageInputSupport::Multimodal);
    config::ModelCatalogEntry audio_only;
    audio_only.input_modalities = {"audio"};
    CHECK(config::ClassifyImageInputSupport(&audio_only) == config::ImageInputSupport::Unknown);

    // capabilities.image:写没写都算明声明,真假分家。
    config::ModelCatalogEntry cap_false;
    cap_false.capabilities["image"] = false;
    CHECK(config::ClassifyImageInputSupport(&cap_false) == config::ImageInputSupport::TextOnly);
    config::ModelCatalogEntry cap_true;
    cap_true.capabilities["image"] = true;
    CHECK(config::ClassifyImageInputSupport(&cap_true) == config::ImageInputSupport::Multimodal);

    // modalities 优先于 capabilities:两边打架听 modalities。
    config::ModelCatalogEntry conflict;
    conflict.input_modalities = {"text"};
    conflict.capabilities["image"] = true;
    CHECK(config::ClassifyImageInputSupport(&conflict) == config::ImageInputSupport::TextOnly);
}


// ---------------------------------------------------------------------------
// 动态工具 P3(Claude NativeReference):models.json 条目的 deferred_tools
// 声明与 ClassifyNativeToolSearch 判读。目录不写 = 不声明,不按厂名猜
//(单子红线 2:兼容端不得误开)。
// ---------------------------------------------------------------------------

TEST_CASE("models.json 条目可写 deferred_tools,坏段按坏条目拒(P3)") {
    const auto parsed = config::ParseModelCatalogJson(
        R"({"models":[)"
        R"({"slug":"claude-x","deferred_tools":{"mode":"native_reference","tool_reference":true,"server_tool_search":"regex"}},)"
        R"({"slug":"compat-y"})"
        R"(]})",
        "test-models.json");
    REQUIRE(parsed.models.size() == 2);
    CHECK(parsed.models[0].deferred_tools.declared);
    CHECK(parsed.models[0].deferred_tools.tool_reference);
    CHECK(parsed.models[0].deferred_tools.server_tool_search == "regex");
    CHECK_FALSE(parsed.models[1].deferred_tools.declared);  // 没写 = 不声明

    // 坏段跳条目并记警告:mode 不认 / server_tool_search 不认。
    const auto bad_mode = config::ParseModelCatalogJson(
        R"({"models":[{"slug":"bad","deferred_tools":{"mode":"proxy"}}]})", "t.json");
    CHECK(bad_mode.models.empty());
    CHECK(bad_mode.warnings.size() == 1);
    const auto bad_variant = config::ParseModelCatalogJson(
        R"({"models":[{"slug":"bad","deferred_tools":{"mode":"native_reference","server_tool_search":"向量"}}]})",
        "t.json");
    CHECK(bad_variant.models.empty());
    CHECK(bad_variant.warnings.size() == 1);
}

TEST_CASE("ClassifyNativeToolSearch:声明与 tool_reference 齐了才算,半截声明不认(P3)") {
    // 没条目 / 没声明:一律不开,不猜。
    CHECK_FALSE(config::ClassifyNativeToolSearch(nullptr).declared);
    config::ModelCatalogEntry plain;
    plain.slug = "plain";
    CHECK_FALSE(config::ClassifyNativeToolSearch(&plain).declared);

    // 声明了但 tool_reference=false:自相矛盾的半截,当没声明。
    config::ModelCatalogEntry half;
    half.deferred_tools.declared = true;
    half.deferred_tools.tool_reference = false;
    CHECK_FALSE(config::ClassifyNativeToolSearch(&half).declared);

    // 全套声明:逐字段递进(变体原样带出,空串 = 只声明引用能力)。
    config::ModelCatalogEntry full;
    full.deferred_tools.declared = true;
    full.deferred_tools.tool_reference = true;
    full.deferred_tools.server_tool_search = "bm25";
    const auto capability = config::ClassifyNativeToolSearch(&full);
    CHECK(capability.declared);
    CHECK(capability.tool_reference);
    CHECK(capability.server_tool_search == "bm25");
    config::ModelCatalogEntry no_search;
    no_search.deferred_tools.declared = true;
    no_search.deferred_tools.tool_reference = true;
    CHECK(config::ClassifyNativeToolSearch(&no_search).server_tool_search.empty());
}
