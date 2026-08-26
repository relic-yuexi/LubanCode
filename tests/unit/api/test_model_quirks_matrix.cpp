// 模型怪癖矩阵(单):用户点名"各种奇怪的模型什么的",这册把各家模型的
// 怪癖钉死在测试层。三个来源:
//   1) 内嵌 catalog 快照的真条目逐家扫掠——每只有推理档案的模型,按自家
//      provider 的 wire 过一遍翻译,断言公开不变式(档位名原样落、none 才
//      是关、minimal 声明了就是真档、budget 落在声明区间);
//   2) 点名怪模型的字节级形状:GLM-5.2 双开、deepseek 纯 toggle、
//      gemini 2.5/3.1 的 budget/档位、o 系/gpt-5 effort 透传、kimi-k2.7-code
//      纯 budget、无推理模型不发任何 reasoning 字段;
//   3) 四家共用的边角:usage 摊账口径、SSE 态(半截帧/坏 UTF-8/空增量/
//      交错/单帧超限)、extra_body 覆盖序、system 形状不互渗、工具名与
//      schema 边角、并行工具批。
// 已有的逐 wire 单测(test_*_request/test_*_events)钉各自正路;这册只钉
// "怪",不重复正路。

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <map>
#include <string>
#include <vector>

#include "api/anthropic/client.hpp"
#include "api/anthropic/events.hpp"
#include "api/assembler.hpp"
#include "api/chat/events.hpp"
#include "api/chat/request.hpp"
#include "api/gemini/events.hpp"
#include "api/gemini/request.hpp"
#include "api/responses/events.hpp"
#include "api/responses/request.hpp"
#include "api/sse_framing.hpp"
#include "api/types.hpp"
#include "config/provider_catalog.hpp"
#include "embedded_provider_catalog.hpp"
#include "platform/text_encoding.hpp"

using namespace lubancode;

namespace {

// 内嵌目录快照只解析一次(1.2MB,逐 CASE 重析纯浪费)。
const config::ProviderCatalog& EmbeddedCatalog() {
    static const auto parsed = config::ParseProviderCatalogJson(
        config::embedded::kProviderCatalogJson, "<embedded>");
    REQUIRE(parsed.has_value());
    static const config::ProviderCatalog catalog = std::move(*parsed);
    return catalog;
}

api::SseFrame Frame(std::string data) {
    return api::SseFrame{"message", std::move(data)};
}

// 目录档案里有没有声明某枚档位(大小写不敏感,与 api 层判定同一口径)。
bool DeclaresEffort(const api::ReasoningConfig& config, const std::string& lowered) {
    for (const auto& declared : config.supported_efforts) {
        std::string low = declared;
        for (char& c : low) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (low == lowered) {
            return true;
        }
    }
    return false;
}

// budget 档位的独立账房:按公开规矩(min+(max-min)*rank/4,超 max_tokens 夹
// 到 max-256,legacy 表 1k/4k/16k/32k/48k)在测试里重推一遍,不调产品代码
// 的 ReasoningBudgetForEffort——两边各算各的,对不上就是翻译层改了规矩。
int ExpectedBudget(const api::ReasoningConfig& config, const std::string& lowered,
                   int max_tokens) {
    if (!config.budget_min.has_value() && !config.budget_max.has_value()) {
        int legacy = 16384;
        if (lowered == "low" || lowered == "minimal") legacy = 1024;
        else if (lowered == "medium" || lowered == "auto") legacy = 4096;
        else if (lowered == "xhigh" || lowered == "extra") legacy = 32768;
        else if (lowered == "max") legacy = 49152;
        if (max_tokens > 0 && legacy >= max_tokens) {
            legacy = max_tokens > 256 ? max_tokens - 256 : max_tokens / 2;
        }
        return std::max(1, legacy);
    }
    const int minimum = std::max(1, config.budget_min.value_or(1024));
    const int maximum = std::max(minimum, config.budget_max.value_or(49152));
    int rank = 2;
    if (lowered == "low" || lowered == "minimal") rank = 0;
    else if (lowered == "medium") rank = 1;
    else if (lowered == "high") rank = 2;
    else if (lowered == "xhigh" || lowered == "extra") rank = 3;
    else if (lowered == "max") rank = 4;
    const long long span = static_cast<long long>(maximum) - minimum;
    int budget = minimum + static_cast<int>((span * rank) / 4);
    if (max_tokens > 0 && budget >= max_tokens) {
        budget = max_tokens > 256 ? max_tokens - 256 : max_tokens / 2;
    }
    return std::max(1, budget);
}

std::string Lowered(std::string effort) {
    for (char& c : effort) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return effort;
}

// 扫掠用请求:模型 id + 目录档案 + 档位 + 目录声明的输出上限。
api::Request SweepRequest(const config::ProviderCatalogModel& model, const std::string& effort) {
    api::Request request;
    request.model = model.id;
    request.reasoning = model.reasoning;
    request.reasoning_effort = effort;
    if (model.max_output_tokens.has_value() && *model.max_output_tokens <= INT_MAX) {
        request.max_tokens = static_cast<int>(*model.max_output_tokens);
    }
    return request;
}

}  // namespace

// ===========================================================================
// A. 推理档位三态
// ===========================================================================

TEST_CASE("矩阵 A0: ReasoningEffortIsOff 目录判定版——minimal 声明了就是真档") {
    api::ReasoningConfig declares;
    declares.supported_efforts = {"none", "minimal", "low", "high"};

    CHECK(api::ReasoningEffortIsOff("none", declares));
    CHECK(api::ReasoningEffortIsOff("NONE", api::ReasoningConfig{}));   // none 永远是关
    CHECK(api::ReasoningEffortIsOff("none", api::ReasoningConfig{}));
    // 目录没声明 minimal:沿用旧口径,当关(无档案的兼容路径行为不变)。
    CHECK(api::ReasoningEffortIsOff("minimal", api::ReasoningConfig{}));
    // 目录声明了:minimal 是一档真实的最浅思考,不是关。
    CHECK_FALSE(api::ReasoningEffortIsOff("minimal", declares));
    CHECK_FALSE(api::ReasoningEffortIsOff("MINIMAL", declares));  // 大小写不敏感
    CHECK_FALSE(api::ReasoningEffortIsOff("auto", api::ReasoningConfig{}));
    CHECK_FALSE(api::ReasoningEffortIsOff("high", declares));
    // 单参旧版(兼容路径)口径不动:none|minimal 皆关。
    CHECK(api::ReasoningEffortIsOff("minimal"));
    CHECK(api::ReasoningEffortIsOff("none"));
    CHECK_FALSE(api::ReasoningEffortIsOff("high"));
}

TEST_CASE("矩阵 A1: catalog 扫掠——每只有推理档案的模型按自家 wire 过翻译,不变式成立") {
    const auto& catalog = EmbeddedCatalog();
    std::size_t swept_models = 0;
    std::size_t swept_cases = 0;
    for (const auto& provider : catalog.providers) {
        for (const auto& model : provider.models) {
            if (model.reasoning.empty()) {
                continue;
            }
            ++swept_models;
            // 档位取声明值 ∪ {none}(去重,大小写不敏感)。
            std::vector<std::string> efforts;
            for (const auto& declared : model.reasoning.supported_efforts) {
                const std::string lowered = Lowered(declared);
                if (std::find_if(efforts.begin(), efforts.end(),
                                 [&](const std::string& e) { return Lowered(e) == lowered; }) ==
                    efforts.end()) {
                    efforts.push_back(declared);
                }
            }
            if (std::none_of(efforts.begin(), efforts.end(),
                             [](const std::string& e) { return Lowered(e) == "none"; })) {
                efforts.push_back("none");
            }
            REQUIRE_FALSE(efforts.empty());

            for (const auto& effort : efforts) {
                ++swept_cases;
                const api::Request request = SweepRequest(model, effort);
                const std::string lowered = Lowered(effort);
                const bool off = api::ReasoningEffortIsOff(effort, model.reasoning);
                const bool effort_dialect =
                    model.reasoning.wire_dialect == "effort" || model.reasoning.supports_effort;
                const int effective_max = request.max_tokens.value_or(
                    api::kRequiredMaxOutputTokensFallback);

                if (provider.wire == config::Wire::ChatCompletions) {
                    const auto body = api::chat::BuildRequestJson(request);
                    if (model.reasoning.supports_effort) {
                        CHECK(body.value("reasoning_effort", std::string()) == effort);
                    } else {
                        CHECK_FALSE(body.contains("reasoning_effort"));
                    }
                    if (model.reasoning.supports_toggle) {
                        REQUIRE(body.contains("thinking"));
                        CHECK(body["thinking"].at("type") == (off ? "disabled" : "enabled"));
                    } else {
                        CHECK_FALSE(body.contains("thinking"));
                    }
                } else if (provider.wire == config::Wire::Responses) {
                    const auto body = api::responses::BuildRequestJson(request);
                    if (model.reasoning.supports_effort) {
                        REQUIRE(body.contains("reasoning"));
                        CHECK(body["reasoning"].at("effort") == effort);
                    } else {
                        CHECK_FALSE(body.contains("reasoning"));
                    }
                    if (model.reasoning.supports_toggle) {
                        REQUIRE(body.contains("thinking"));
                        CHECK(body["thinking"].at("type") == (off ? "disabled" : "enabled"));
                    } else {
                        CHECK_FALSE(body.contains("thinking"));
                    }
                } else if (provider.wire == config::Wire::Anthropic) {
                    const auto body = api::anthropic::BuildRequestJson(request);
                    REQUIRE(body.contains("thinking"));
                    if (off) {
                        CHECK(body["thinking"].at("type") == "disabled");
                        CHECK_FALSE(body["thinking"].contains("budget_tokens"));
                        CHECK_FALSE(body.contains("output_config"));
                    } else if (model.reasoning.wire_dialect == "effort") {
                        CHECK(body["thinking"].at("type") == "adaptive");
                        CHECK_FALSE(body["thinking"].contains("budget_tokens"));
                        REQUIRE(body.contains("output_config"));
                        CHECK(body["output_config"].at("effort") ==
                              (lowered == "extra" ? "xhigh" : lowered));
                    } else {
                        CHECK(body["thinking"].at("type") == "enabled");
                        REQUIRE(body["thinking"].contains("budget_tokens"));
                        const int budget = body["thinking"].at("budget_tokens").get<int>();
                        CHECK(budget >= 1);
                        CHECK(budget < effective_max);  // Anthropic 硬约束
                        CHECK(budget == ExpectedBudget(model.reasoning, lowered, effective_max));
                    }
                } else {  // GoogleGenerateContent
                    const auto body = api::gemini::BuildRequestJson(request);
                    REQUIRE(body["generationConfig"].contains("thinkingConfig"));
                    const auto& thinking = body["generationConfig"].at("thinkingConfig");
                    CHECK(thinking.at("includeThoughts") == !off);
                    if (off) {
                        if (model.reasoning.supports_toggle) {
                            CHECK(thinking.at("thinkingBudget") == 0);
                        } else {
                            // 没声明 toggle 的模型(如 gemini-2.5-pro,官方不许关
                            // 思考)不发 budget/level,交服务端默认。
                            CHECK_FALSE(thinking.contains("thinkingBudget"));
                            CHECK_FALSE(thinking.contains("thinkingLevel"));
                        }
                    } else if (effort_dialect) {
                        CHECK(thinking.at("thinkingLevel") == lowered);
                        CHECK_FALSE(thinking.contains("thinkingBudget"));  // 两键并发的 400
                    } else if (model.reasoning.wire_dialect == "budget" ||
                               model.reasoning.budget_max.has_value()) {
                        if (lowered == "auto") {
                            CHECK(thinking.at("thinkingBudget") == -1);  // 动态思考
                        } else {
                            REQUIRE(thinking.contains("thinkingBudget"));
                            const int budget = thinking.at("thinkingBudget").get<int>();
                            CHECK(budget == ExpectedBudget(model.reasoning, lowered,
                                                           request.max_tokens.value_or(0)));
                            CHECK_FALSE(thinking.contains("thinkingLevel"));
                        }
                    } else {
                        // toggle-only/光杆档案(gemma-4 这类):不发 budget/level,
                        // includeThoughts 之外交服务端默认深度。
                        CHECK_FALSE(thinking.contains("thinkingBudget"));
                        CHECK_FALSE(thinking.contains("thinkingLevel"));
                    }
                }
            }
        }
    }
    // 防呆:扫掠必须真的扫到了 catalog 里的怪模型,不是空转。
    CHECK(swept_models > 500);
    CHECK(swept_cases > 1500);
}

TEST_CASE("矩阵 A2: GLM-5.2(chat wire)——effort 档 + thinking 块双开,minimal 是真档") {
    const auto* model = EmbeddedCatalog().FindProvider("zai")->FindModel("glm-5.2");
    REQUIRE(model != nullptr);
    REQUIRE(model->reasoning.supports_effort);
    REQUIRE(model->reasoning.supports_toggle);

    api::Request request = SweepRequest(*model, "max");
    const auto body = api::chat::BuildRequestJson(request);
    CHECK(body["reasoning_effort"] == "max");  // z.ai 官方:reasoning_effort 管深度
    CHECK(body["thinking"].at("type") == "enabled");  // thinking.type 管开关,两键并开

    // none:开关关掉,档位名照发(z.ai 双参数互不干涉)。
    api::Request off_request = SweepRequest(*model, "none");
    const auto off_body = api::chat::BuildRequestJson(off_request);
    CHECK(off_body["reasoning_effort"] == "none");
    CHECK(off_body["thinking"].at("type") == "disabled");

    // minimal:目录声明了(与 none 并列),开关必须留在开——修前这只档位
    // 被旧 ReasoningEffortIsOff 当成关,用户选的最低思考档悄悄变没档。
    api::Request minimal_request = SweepRequest(*model, "minimal");
    const auto minimal_body = api::chat::BuildRequestJson(minimal_request);
    CHECK(minimal_body["reasoning_effort"] == "minimal");
    CHECK(minimal_body["thinking"].at("type") == "enabled");
}

TEST_CASE("矩阵 A3: GLM-5.2(anthropic wire)——无 dialect,落 budget 档,minimal 落最低档") {
    const auto* model = EmbeddedCatalog().FindProvider("zai-anthropic")->FindModel("glm-5.2");
    REQUIRE(model != nullptr);

    const auto body = api::anthropic::BuildRequestJson(SweepRequest(*model, "max"));
    CHECK(body["thinking"].at("type") == "enabled");
    const int max_budget = body["thinking"].at("budget_tokens").get<int>();

    // minimal 声明成了档:enabled + 最低档预算(1024),不是 disabled。
    const auto minimal_body = api::anthropic::BuildRequestJson(SweepRequest(*model, "minimal"));
    CHECK(minimal_body["thinking"].at("type") == "enabled");
    const int minimal_budget = minimal_body["thinking"].at("budget_tokens").get<int>();
    CHECK(minimal_budget == 1024);
    CHECK(minimal_budget < max_budget);
}

TEST_CASE("矩阵 A4: deepseek-chat——纯 toggle 模型,不乱发 reasoning_effort,未声明的 minimal 才当关") {
    const auto* model = EmbeddedCatalog().FindProvider("deepseek")->FindModel("deepseek-chat");
    REQUIRE(model != nullptr);
    REQUIRE(model->reasoning.supports_toggle);
    CHECK_FALSE(model->reasoning.supports_effort);

    const auto body = api::chat::BuildRequestJson(SweepRequest(*model, "high"));
    CHECK(body["thinking"].at("type") == "enabled");
    CHECK_FALSE(body.contains("reasoning_effort"));  // 没声明 effort 档就不发这个参数

    const auto off_body = api::chat::BuildRequestJson(SweepRequest(*model, "none"));
    CHECK(off_body["thinking"].at("type") == "disabled");

    // 目录没声明 minimal:沿用旧口径当关(与 A0 的兼容路径同一判定)。
    const auto minimal_body = api::chat::BuildRequestJson(SweepRequest(*model, "minimal"));
    CHECK(minimal_body["thinking"].at("type") == "disabled");
}

TEST_CASE("矩阵 A5: gemini-2.5-pro——纯 budget 模型:档位摊成区间内预算,auto=-1,none 不发字段") {
    const auto* model = EmbeddedCatalog().FindProvider("gemini")->FindModel("gemini-2.5-pro");
    REQUIRE(model != nullptr);
    REQUIRE(model->reasoning.budget_min.has_value());
    REQUIRE(model->reasoning.budget_max.has_value());
    CHECK_FALSE(model->reasoning.supports_toggle);

    const auto body = api::gemini::BuildRequestJson(SweepRequest(*model, "high"));
    const auto& thinking = body["generationConfig"].at("thinkingConfig");
    // 128 + (32768-128)*2/4 = 16448:落在声明的 [128,32768] 区间里。
    CHECK(thinking.at("thinkingBudget") == 16448);
    CHECK(thinking.at("includeThoughts") == true);

    const auto auto_body = api::gemini::BuildRequestJson(SweepRequest(*model, "auto"));
    CHECK(auto_body["generationConfig"].at("thinkingConfig").at("thinkingBudget") == -1);

    // none:没声明 toggle 的模型不发 budget/level(官方口径 2.5-pro 关不掉思考),
    // 只把思考展示关掉。
    const auto off_body = api::gemini::BuildRequestJson(SweepRequest(*model, "none"));
    const auto& off_thinking = off_body["generationConfig"].at("thinkingConfig");
    CHECK(off_thinking.at("includeThoughts") == false);
    CHECK_FALSE(off_thinking.contains("thinkingBudget"));
    CHECK_FALSE(off_thinking.contains("thinkingLevel"));
}

TEST_CASE("矩阵 A6: gemini-2.5-flash——toggle+budget:none 落 thinkingBudget=0(官方允许的关法)") {
    const auto* model = EmbeddedCatalog().FindProvider("gemini")->FindModel("gemini-2.5-flash");
    REQUIRE(model != nullptr);
    REQUIRE(model->reasoning.supports_toggle);

    const auto body = api::gemini::BuildRequestJson(SweepRequest(*model, "none"));
    const auto& thinking = body["generationConfig"].at("thinkingConfig");
    CHECK(thinking.at("thinkingBudget") == 0);
    CHECK(thinking.at("includeThoughts") == false);
}

TEST_CASE("矩阵 A7: gemini-3.1-flash-lite——effort 档,minimal 翻成 thinkingLevel=minimal") {
    const auto* model =
        EmbeddedCatalog().FindProvider("gemini")->FindModel("gemini-3.1-flash-lite");
    REQUIRE(model != nullptr);
    REQUIRE(model->reasoning.wire_dialect == "effort");
    REQUIRE(DeclaresEffort(model->reasoning, "minimal"));

    // 官方 ThinkingLevel 枚举里 minimal 是"几乎不思考"的真实档位;修前
    // 被当"关",thinkingLevel 压根不发,用户选的最低档变回默认档。
    const auto body = api::gemini::BuildRequestJson(SweepRequest(*model, "minimal"));
    const auto& thinking = body["generationConfig"].at("thinkingConfig");
    CHECK(thinking.at("thinkingLevel") == "minimal");
    CHECK(thinking.at("includeThoughts") == true);
    CHECK_FALSE(thinking.contains("thinkingBudget"));  // level 与 budget 并发会吃 400
}

TEST_CASE("矩阵 A8: o 系与 gpt-5(responses wire)——effort 原样透传,gpt-5 的 minimal 不当关") {
    const auto* openai = EmbeddedCatalog().FindProvider("openai");
    REQUIRE(openai != nullptr);

    const auto* o3 = openai->FindModel("o3");
    REQUIRE(o3 != nullptr);
    const auto o3_body = api::responses::BuildRequestJson(SweepRequest(*o3, "high"));
    CHECK(o3_body["reasoning"].at("effort") == "high");
    CHECK_FALSE(o3_body.contains("thinking"));

    const auto* gpt5 = openai->FindModel("gpt-5");
    REQUIRE(gpt5 != nullptr);
    REQUIRE(DeclaresEffort(gpt5->reasoning, "minimal"));
    const auto gpt5_body = api::responses::BuildRequestJson(SweepRequest(*gpt5, "minimal"));
    CHECK(gpt5_body["reasoning"].at("effort") == "minimal");
    CHECK_FALSE(gpt5_body.contains("thinking"));  // 无 toggle,不写 thinking 键
}

TEST_CASE("矩阵 A9: kimi-k2.7-code——纯 budget:anthropic 落预算,chat 发不出任何 reasoning 键(记案)") {
    const auto* moonshot = EmbeddedCatalog().FindProvider("moonshot");
    REQUIRE(moonshot != nullptr);
    const auto* model = moonshot->FindModel("kimi-k2.7-code");
    REQUIRE(model != nullptr);
    REQUIRE(model->reasoning.budget_max.has_value());
    CHECK_FALSE(model->reasoning.supports_effort);
    CHECK_FALSE(model->reasoning.supports_toggle);

    // anthropic wire:budget 档照翻,0 + 30720*2/4 = 15360。
    const auto anthropic_body =
        api::anthropic::BuildRequestJson(SweepRequest(*model, "high"));
    CHECK(anthropic_body["thinking"].at("type") == "enabled");
    CHECK(anthropic_body["thinking"].at("budget_tokens") == 15360);

    // chat wire:中立层没有 Chat 方言的 budget 参数名,这只模型在 chat 家
    // 发不出任何 reasoning 字段(现状记案:要靠目录加方言声明才能修,
    // 见模型怪癖矩阵工单的报告;这里钉住"至少不发错形状")。
    const auto chat_body = api::chat::BuildRequestJson(SweepRequest(*model, "high"));
    CHECK_FALSE(chat_body.contains("reasoning_effort"));
    CHECK_FALSE(chat_body.contains("thinking"));
    CHECK_FALSE(chat_body.contains("thinking_budget"));
}

TEST_CASE("矩阵 A10: 无推理模型 × 四家 wire——一个 reasoning 字段都不发") {
    const auto& catalog = EmbeddedCatalog();
    std::map<config::Wire, api::Request> samples;
    for (const auto& provider : catalog.providers) {
        if (samples.count(provider.wire) != 0) {
            continue;
        }
        for (const auto& model : provider.models) {
            if (model.reasoning.empty()) {
                api::Request request;
                request.model = model.id;  // 档位空串 = 不发
                samples.emplace(provider.wire, request);
                break;
            }
        }
    }
    REQUIRE(samples.size() == 4);  // 四条 wire 都有无推理模型的真条目

    const auto& chat_body = api::chat::BuildRequestJson(samples[config::Wire::ChatCompletions]);
    CHECK_FALSE(chat_body.contains("reasoning_effort"));
    CHECK_FALSE(chat_body.contains("thinking"));
    CHECK_FALSE(chat_body.contains("reasoning"));

    const auto& responses_body =
        api::responses::BuildRequestJson(samples[config::Wire::Responses]);
    CHECK_FALSE(responses_body.contains("reasoning"));
    CHECK_FALSE(responses_body.contains("thinking"));
    CHECK_FALSE(responses_body.contains("reasoning_effort"));

    const auto& anthropic_body =
        api::anthropic::BuildRequestJson(samples[config::Wire::Anthropic]);
    CHECK_FALSE(anthropic_body.contains("thinking"));
    CHECK_FALSE(anthropic_body.contains("output_config"));

    const auto& gemini_body =
        api::gemini::BuildRequestJson(samples[config::Wire::GoogleGenerateContent]);
    CHECK_FALSE(gemini_body.contains("generationConfig"));  // 无上限无档位,整块不发
}

TEST_CASE("矩阵 A11: ReasoningBudgetForEffort——minimal 摊最低档,不再落默认档") {
    api::ReasoningConfig budgeted;
    budgeted.budget_min = 128;
    budgeted.budget_max = 32768;
    CHECK(api::ReasoningBudgetForEffort(budgeted, "minimal", 65536) == 128);
    CHECK(api::ReasoningBudgetForEffort(budgeted, "low", 65536) == 128);
    CHECK(api::ReasoningBudgetForEffort(budgeted, "high", 65536) == 16448);
    CHECK(api::ReasoningBudgetForEffort(api::ReasoningConfig{}, "minimal", 0) == 1024);
}

// ===========================================================================
// B. 工具格式边角
// ===========================================================================

api::Request ToolEdgeRequest() {
    api::Request request;
    request.model = "weird-model";
    api::ToolDefinition dotted;
    dotted.name = "mcp__server__tool.v2";
    dotted.description = "名里带点";
    dotted.input_schema = nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};
    api::ToolDefinition chinese;
    chinese.name = "读文件_中文";
    chinese.description = "名里带中文";
    chinese.input_schema = nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};
    api::ToolDefinition slashed;
    slashed.name = "accounts/fireworks/models/tool";
    slashed.description = "名里带斜杠(fireworks 实名)";
    slashed.input_schema = nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};
    request.tools = {dotted, chinese, slashed};
    return request;
}

TEST_CASE("矩阵 B1: 空 tools——四家都不发 tools 字段(空数组会让有些端 500)") {
    api::Request request;
    request.model = "m";
    CHECK_FALSE(api::chat::BuildRequestJson(request).contains("tools"));
    CHECK_FALSE(api::responses::BuildRequestJson(request).contains("tools"));
    CHECK_FALSE(api::anthropic::BuildRequestJson(request).contains("tools"));
    CHECK_FALSE(api::gemini::BuildRequestJson(request).contains("tools"));
}

TEST_CASE("矩阵 B2: 工具名带点/中文/斜杠——四家逐字节原样放行") {
    const auto request = ToolEdgeRequest();
    const std::vector<std::string> names = {"mcp__server__tool.v2", "读文件_中文",
                                            "accounts/fireworks/models/tool"};

    const auto chat_body = api::chat::BuildRequestJson(request);
    for (std::size_t i = 0; i < names.size(); ++i) {
        CHECK(chat_body["tools"][i].at("function").at("name") == names[i]);
    }
    const auto responses_body = api::responses::BuildRequestJson(request);
    for (std::size_t i = 0; i < names.size(); ++i) {
        CHECK(responses_body["tools"][i].at("name") == names[i]);
    }
    const auto anthropic_body = api::anthropic::BuildRequestJson(request);
    for (std::size_t i = 0; i < names.size(); ++i) {
        CHECK(anthropic_body["tools"][i].at("name") == names[i]);
    }
    const auto gemini_body = api::gemini::BuildRequestJson(request);
    const auto& decls = gemini_body["tools"][0].at("functionDeclarations");
    for (std::size_t i = 0; i < names.size(); ++i) {
        CHECK(decls[i].at("name") == names[i]);
    }
}

TEST_CASE("矩阵 B3: 深嵌套 optional schema(anyOf/嵌套 optional)——四家原样放行不兑壳") {
    api::Request request;
    request.model = "m";
    const auto deep = nlohmann::json::parse(R"({
        "type": "object",
        "properties": {
            "target": {
                "anyOf": [
                    {"type": "string", "description": "路径"},
                    {"type": "object", "properties": {"root": {"type": "string"}}, "required": []}
                ]
            },
            "maybe": {"type": ["string", "null"]}
        },
        "required": ["target"],
        "additionalProperties": false
    })");
    api::ToolDefinition tool{"deep_tool", "深 schema", deep};
    request.tools.push_back(tool);

    CHECK(api::chat::BuildRequestJson(request)["tools"][0].at("function").at("parameters") == deep);
    CHECK(api::responses::BuildRequestJson(request)["tools"][0].at("parameters") == deep);
    CHECK(api::anthropic::BuildRequestJson(request)["tools"][0].at("input_schema") == deep);
    CHECK(api::gemini::BuildRequestJson(request)["tools"][0].at("functionDeclarations")[0].at(
              "parameters") == deep);
}

api::Request ParallelToolRequest() {
    api::Request request;
    request.model = "m";
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"两个都读"});
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(
        api::ToolUseBlock{"call_1", "read_file", nlohmann::json{{"path", "a"}}});
    assistant.content.push_back(
        api::ToolUseBlock{"call_2", "read_file", nlohmann::json{{"path", "b"}}});
    request.messages = {user, assistant};
    return request;
}

TEST_CASE("矩阵 B4: 并行工具批请求侧——四家各自成对") {
    const auto request = ParallelToolRequest();

    const auto chat_body = api::chat::BuildRequestJson(request);
    REQUIRE(chat_body["messages"][1].at("tool_calls").size() == 2);
    CHECK(chat_body["messages"][1]["tool_calls"][0].at("function").at("arguments") == R"({"path":"a"})");
    CHECK(chat_body["messages"][1]["tool_calls"][1].at("function").at("name") == "read_file");

    const auto responses_body = api::responses::BuildRequestJson(request);
    REQUIRE(responses_body["input"].size() == 3);  // user 消息 + 两枚 function_call
    CHECK(responses_body["input"][1].at("type") == "function_call");
    CHECK(responses_body["input"][2].at("call_id") == "call_2");

    const auto anthropic_body = api::anthropic::BuildRequestJson(request);
    const auto& content = anthropic_body["messages"][1].at("content");
    REQUIRE(content.size() == 2);
    CHECK(content[0].at("type") == "tool_use");
    CHECK(content[1].at("id") == "call_2");

    const auto gemini_body = api::gemini::BuildRequestJson(request);
    // user 文本一条 + 两条 functionCall 各自成条。
    REQUIRE(gemini_body["contents"].size() == 3);
    CHECK(gemini_body["contents"][1]["parts"][0].at("functionCall").at("args").at("path") == "a");
    CHECK(gemini_body["contents"][2]["parts"][0].at("functionCall").at("args").at("path") == "b");
}

TEST_CASE("矩阵 B5: 并行工具批事件侧——chat 一只 chunk 两枚 call,gemini 一帧两个 functionCall") {
    api::chat::EventParser chat;
    auto events = chat.Consume(Frame(
        R"({"id":"1","model":"m","choices":[{"delta":{"tool_calls":[)"
        R"({"index":0,"id":"c1","function":{"name":"read_file","arguments":"{\"path\":\""}},)"
        R"({"index":1,"id":"c2","function":{"name":"read_file","arguments":"{}"}}]}}]})"));
    REQUIRE(events.size() == 1);  // 只有 MessageStart,工具攒着不发
    auto flushed = chat.Consume(Frame(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"a\"}"}}]},)"
        R"("finish_reason":"tool_calls"}]})"));
    REQUIRE(flushed.empty());  // finish 只记账,工具在 Finish 落锤
    const auto done = chat.Finish();
    REQUIRE(done.size() == 8);  // ContentBlockDone + (Start/Delta/Done)*2 + MessageDone
    CHECK(std::get<api::ToolUseStart>(done[1]).name == "read_file");
    CHECK(std::get<api::ToolUseStart>(done[1]).id == "c1");
    CHECK(std::get<api::ToolUseInputDelta>(done[2]).partial_json == R"({"path":"a"})");
    CHECK(std::get<api::ToolUseStart>(done[4]).id == "c2");
    CHECK(std::get<api::ToolUseInputDelta>(done[5]).partial_json == "{}");
    CHECK(std::get<api::MessageDone>(done[7]).stop_reason == "tool_use");

    api::gemini::EventParser gemini;
    const auto gemini_done = gemini.Consume(Frame(
        R"({"candidates":[{"content":{"parts":[)"
        R"({"functionCall":{"name":"f1","args":{"x":1}}},)"
        R"({"functionCall":{"name":"f2","args":{}}}]},)"
        R"("finishReason":"STOP"}],"modelVersion":"gemini"})"));
    // MessageStart + (ContentBlockDone + Start/Delta/Done)*2 + MessageDone。
    REQUIRE(gemini_done.size() == 9);
    CHECK(std::get<api::ToolUseStart>(gemini_done[2]).name == "f1");
    CHECK(std::get<api::ToolUseStart>(gemini_done[5]).name == "f2");
    CHECK(std::get<api::MessageDone>(gemini_done[8]).stop_reason == "tool_use");
}

// ===========================================================================
// C. usage 摊账(各家口径对得上、不丢不重、守恒)
// ===========================================================================

namespace {

template <typename T>
const T* FindEvent(const std::vector<api::StreamEvent>& events) {
    for (const auto& event : events) {
        if (const auto* typed = std::get_if<T>(&event); typed != nullptr) {
            return typed;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("矩阵 C1: anthropic——四字段原账,输入守恒 input+read+creation") {
    const auto done = api::anthropic::parse_event(Frame(
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"input_tokens":100,"output_tokens":60,"cache_read_input_tokens":400,"cache_creation_input_tokens":50}})"));
    REQUIRE(done.has_value());
    const auto& usage = std::get<api::MessageDone>(*done).usage;
    CHECK(usage.input_tokens == 100);
    CHECK(usage.cache_read_tokens == 400);
    CHECK(usage.cache_creation_tokens == 50);
    CHECK(usage.output_tokens == 60);
    CHECK(api::TotalInputTokens(usage) == 550);
}

TEST_CASE("矩阵 C2: chat DeepSeek——hit/miss 摊账,49k 命中 + 1k 未命中 = 50k 输入") {
    api::chat::EventParser parser;
    parser.Consume(Frame(R"({"id":"c1","model":"deepseek","choices":[{"delta":{"content":"答"}}]})"));
    parser.Consume(Frame(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"));
    // 独立 usage chunk(stream_options.include_usage 换来的那只)在 [DONE] 前,
    // 终账由 [DONE] 触发的 Finish 落锤。
    parser.Consume(Frame(
        R"({"choices":[],"usage":{"prompt_tokens":50000,"completion_tokens":10,"prompt_cache_hit_tokens":49000,"prompt_cache_miss_tokens":1000}})"));
    // [DONE] 触发 Finish 落锤;先落盘事件再取指针——FindEvent 的结果指着
    // 这份 vector,悬在临时上就是踩野内存。
    const auto done_events = parser.Consume(Frame("[DONE]"));
    const auto* final = FindEvent<api::MessageDone>(done_events);
    REQUIRE(final != nullptr);
    CHECK(final->usage.input_tokens == 1000);        // miss 才是普通输入
    CHECK(final->usage.cache_read_tokens == 49000);  // hit 是读缓存
    CHECK(api::TotalInputTokens(final->usage) == 50000);  // 不许 50k+49k 重复累计
    CHECK(final->stop_reason == "end_turn");
}

TEST_CASE("矩阵 C3: chat OpenAI/Qwen——cached_tokens 摊账 + reasoning 拆账两种来源") {
    // OpenAI 风格:prompt_tokens 已含 cached,input=total-cached。
    api::chat::EventParser openai;
    openai.Consume(Frame(R"({"id":"o1","model":"gpt","choices":[{"delta":{"content":"x"}}]})"));
    openai.Consume(Frame(R"({"choices":[],"usage":{"prompt_tokens":50000,"completion_tokens":100,"prompt_tokens_details":{"cached_tokens":30000},"completion_tokens_details":{"reasoning_tokens":70}}})"));
    const auto openai_done = openai.Consume(Frame("[DONE]"));
    const auto* usage = FindEvent<api::MessageDone>(openai_done);
    REQUIRE(usage != nullptr);
    CHECK(usage->usage.input_tokens == 20000);
    CHECK(usage->usage.cache_read_tokens == 30000);
    CHECK(api::TotalInputTokens(usage->usage) == 50000);
    CHECK(usage->usage.output_tokens == 100);          // reasoning 含在 output 里
    CHECK(usage->usage.output_reasoning_tokens == 70); // 不是另加的一笔

    // Qwen 风格:顶层 reasoning_tokens。
    api::chat::EventParser qwen;
    qwen.Consume(Frame(R"({"id":"q1","model":"qwen","choices":[{"delta":{"content":"y"}}]})"));
    qwen.Consume(Frame(
        R"({"choices":[],"usage":{"prompt_tokens":100,"completion_tokens":50,"reasoning_tokens":30}})"));
    const auto qwen_done = qwen.Consume(Frame("[DONE]"));
    const auto* qwen_usage = FindEvent<api::MessageDone>(qwen_done);
    REQUIRE(qwen_usage != nullptr);
    CHECK(qwen_usage->usage.output_tokens == 50);
    CHECK(qwen_usage->usage.output_reasoning_tokens == 30);
    CHECK(qwen_usage->usage.input_tokens == 100);
    CHECK(qwen_usage->usage.cache_read_tokens == 0);
}

TEST_CASE("矩阵 C4: responses——input_tokens 已含 cached,摊开不加两遍") {
    const auto done = api::responses::parse_event(Frame(
        R"({"type":"response.completed","response":{"status":"completed","usage":{"input_tokens":1000,"output_tokens":500,"input_tokens_details":{"cached_tokens":400},"output_tokens_details":{"reasoning_tokens":300}}}})"));
    REQUIRE(done.has_value());
    const auto& usage = std::get<api::MessageDone>(*done).usage;
    CHECK(usage.input_tokens == 600);
    CHECK(usage.cache_read_tokens == 400);
    CHECK(api::TotalInputTokens(usage) == 1000);
    CHECK(usage.output_tokens == 500);
    CHECK(usage.output_reasoning_tokens == 300);
    CHECK(usage.cache_creation_tokens == 0);  // responses 无缓存写概念
}

TEST_CASE("矩阵 C5: gemini 两代口径——total 对账归一,另计的一代把思考并进 output") {
    // 2.5 时代(candidates 已含思考):total = prompt + candidates,照旧。
    api::gemini::EventParser old_gen;
    const auto* old_done = FindEvent<api::MessageDone>(old_gen.Consume(Frame(
        R"({"candidates":[{"content":{"parts":[{"text":"答"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":50000,"candidatesTokenCount":80,"thoughtsTokenCount":20,"cachedContentTokenCount":49000,"totalTokenCount":50080}})")));
    REQUIRE(old_done != nullptr);
    CHECK(old_done->usage.output_tokens == 80);
    CHECK(old_done->usage.output_reasoning_tokens == 20);
    CHECK(api::TotalInputTokens(old_done->usage) == 50000);

    // 现行手册口径(思考另计):total = prompt + thoughts + candidates,
    // 思考并进 output(中立契约:reasoning 含在 output 里),输入账不变。
    api::gemini::EventParser new_gen;
    const auto* new_done = FindEvent<api::MessageDone>(new_gen.Consume(Frame(
        R"({"candidates":[{"content":{"parts":[{"text":"答"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":1000,"candidatesTokenCount":171,"thoughtsTokenCount":297,"totalTokenCount":1468}})")));
    REQUIRE(new_done != nullptr);
    CHECK(new_done->usage.input_tokens == 1000);
    CHECK(new_done->usage.output_tokens == 171 + 297);
    CHECK(new_done->usage.output_reasoning_tokens == 297);

    // 没报 total 的旧端:按 2.5 旧口径,宁可少算不瞎加。
    api::gemini::EventParser no_total;
    const auto* legacy_done = FindEvent<api::MessageDone>(no_total.Consume(Frame(
        R"({"candidates":[{"content":{"parts":[{"text":"答"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":100,"candidatesTokenCount":90,"thoughtsTokenCount":40}})")));
    REQUIRE(legacy_done != nullptr);
    CHECK(legacy_done->usage.output_tokens == 90);
    CHECK(legacy_done->usage.output_reasoning_tokens == 40);
}

// ===========================================================================
// D. SSE 态
// ===========================================================================

TEST_CASE("矩阵 D1: 半截帧——一只事件劈三段喂(刀口切在多字节字符中间),四家照常出事件") {
    // 刀口选在中文 UTF-8 三字节序列的中间。
    const std::string chunk_text =
        "data: {\"id\":\"x\",\"model\":\"m\",\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}\n\n";
    const std::size_t cut = chunk_text.find("content\":\"") + 11;  // "你"的第一字节后
    api::SseFramer framer;
    const auto frames = [&] {
        std::vector<api::SseFrame> all;
        for (const auto& piece : {chunk_text.substr(0, cut), chunk_text.substr(cut, 5),
                                  chunk_text.substr(cut + 5)}) {
            const auto produced = framer.feed(piece);
            all.insert(all.end(), produced.begin(), produced.end());
        }
        return all;
    }();
    REQUIRE(frames.size() == 1);
    REQUIRE_FALSE(framer.overflowed());
    api::chat::EventParser parser;
    const auto events = parser.Consume(frames[0]);
    REQUIRE(events.size() == 2);
    CHECK(std::get<api::TextDelta>(events[1]).text == "你好");  // 字节拼齐,UTF-8 完好
}

TEST_CASE("矩阵 D2: 坏 UTF-8 字节——帧里有坏字节整帧当没看见,流不断,下一帧照收") {
    std::string data = R"({"id":"x","model":"m","choices":[{"delta":{"content":"前")";
    data += "\xFF\xFE";  // 坏字节:nlohmann 解析期就拒(UTF-8 校验)
    data += R"("后"}}]})";
    api::chat::EventParser parser;
    CHECK(parser.Consume(Frame(data)).empty());  // 坏帧丢弃,不崩、不吐半截

    // 坏帧之后的好帧照常出事件(含 MessageStart)——服务端偶发一条坏帧
    // 不该杀掉整条流。
    const auto events = parser.Consume(Frame(
        R"({"id":"g1","model":"m","choices":[{"delta":{"content":"好帧"},"finish_reason":"stop"}]})"));
    REQUIRE(events.size() == 2);
    CHECK(std::get<api::TextDelta>(events[1]).text == "好帧");
    CHECK(std::get<api::MessageDone>(parser.Finish()[0]).stop_reason == "end_turn");

    // 四家解析器同一付规矩:坏 UTF-8 帧一律静默跳过,不抛、不崩。
    const std::string anthropic_bad =
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\","
        "\"text\":\"";
    CHECK_FALSE(
        api::anthropic::parse_event(Frame(anthropic_bad + "\xC0\x80" + "\"}")).has_value());
    const std::string responses_bad = "{\"type\":\"response.output_text.delta\",\"delta\":\"";
    CHECK_FALSE(
        api::responses::parse_event(Frame(responses_bad + "\xC0" + "\"}")).has_value());
    api::gemini::EventParser gemini;
    const std::string gemini_bad =
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"";
    CHECK(gemini.Consume(Frame(gemini_bad + "\xFF" + "\"}]}]}")).empty());
}

TEST_CASE("矩阵 D3: 空 delta——chat 的空 content 不吐事件,流不因此断") {
    api::chat::EventParser parser;
    auto events = parser.Consume(Frame(
        R"({"id":"x","model":"m","choices":[{"delta":{"content":""}}]})"));
    REQUIRE(events.size() == 1);  // 只有 MessageStart
    events = parser.Consume(Frame(
        R"({"choices":[{"delta":{"content":null,"reasoning_content":""},"finish_reason":"stop"}]})"));
    CHECK(events.empty());
    const auto done = parser.Finish();
    REQUIRE(done.size() == 1);
    CHECK(std::get<api::MessageDone>(done[0]).stop_reason == "end_turn");
}

TEST_CASE("矩阵 D4: reasoning 与正文交错——事件序保序,assembler 攒出思考在前正文合一") {
    api::chat::EventParser parser;
    api::MessageAssembler assembler;
    const auto feed = [&](const char* chunk) {
        for (const auto& event : parser.Consume(Frame(chunk))) {
            assembler.Feed(event);
        }
    };
    feed(R"({"id":"x","model":"m","choices":[]})");
    feed(R"({"choices":[{"delta":{"reasoning_content":"想一"}}]})");
    feed(R"({"choices":[{"delta":{"content":"文一"}}]})");
    feed(R"({"choices":[{"delta":{"reasoning_content":"想二"}}]})");
    feed(R"({"choices":[{"delta":{"content":"文二","reasoning_content":null},"finish_reason":"stop"}]})");
    for (const auto& event : parser.Finish()) {
        assembler.Feed(event);
    }
    const api::Message message = assembler.BuildMessage();
    // chat 没有块边界标记,交错流的块序契约:思考按到达先后在前,正文合一段。
    REQUIRE(message.content.size() == 3);
    CHECK(std::get<api::ThinkingBlock>(message.content[0]).text == "想一");
    CHECK(std::get<api::ThinkingBlock>(message.content[1]).text == "想二");
    CHECK(std::get<api::TextBlock>(message.content[2]).text == "文一文二");
}

TEST_CASE("矩阵 D5: 单帧超限——framer 报废后一帧不吐,解析层见不到半截") {
    api::SseFramer framer;
    std::string giant = "data: ";
    giant += std::string(api::SseFramer::kMaxFrameBytes + 16, 'x');
    CHECK(framer.feed(giant).empty());
    CHECK(framer.overflowed());
    CHECK(framer.feed("\n\n").empty());  // 报废后不再产出
}

// ===========================================================================
// E. extra_body 合并与覆盖序(provider 级先、请求级后、同名整段覆盖)
// ===========================================================================

TEST_CASE("矩阵 E: extra_body 三层序——内置 < provider < 请求级,四家同一套规矩") {
    const nlohmann::json provider_extra{{"A", 1}, {"B", nlohmann::json{{"x", 1}}}};
    const std::vector<std::pair<config::Wire, nlohmann::json>> bodies = [&] {
        api::Request request;
        request.model = "m";
        request.reasoning_effort = "high";
        request.extra_body = {{"B", nlohmann::json{{"y", 2}}}, {"C", 3}};
        return std::vector<std::pair<config::Wire, nlohmann::json>>{
            {config::Wire::ChatCompletions, api::chat::BuildRequestJson(request, provider_extra)},
            {config::Wire::Responses, api::responses::BuildRequestJson(request, false, provider_extra)},
            {config::Wire::Anthropic, api::anthropic::BuildRequestJson(request, false, provider_extra)},
            {config::Wire::GoogleGenerateContent,
             api::gemini::BuildRequestJson(request, provider_extra)},
        };
    }();
    REQUIRE(bodies.size() == 4);
    for (const auto& [wire, body] : bodies) {
        CHECK(body.at("A") == 1);                          // provider 级原样进
        CHECK(body.at("B") == nlohmann::json{{"y", 2}});   // 同名整段覆盖,不深合并
        CHECK(body.at("C") == 3);                          // 请求级新键追加
        if (wire != config::Wire::GoogleGenerateContent) {
            CHECK(body.at("model") == "m");                // 没动内置键(gemini 的
        } else {
            CHECK(body.contains("contents"));              // model 在 URL 不在体里)
        }
    }
}

// ===========================================================================
// F. system / instructions / systemInstruction 各归各家,不互渗
// ===========================================================================

TEST_CASE("矩阵 F: system 形状互渗——每家只出自家的那只键,别家的不见踪影") {
    api::Request request;
    request.model = "m";
    request.system = "唯一系统提示";
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"问"});
    request.messages.push_back(user);

    const auto chat_body = api::chat::BuildRequestJson(request);
    REQUIRE(chat_body["messages"].size() == 2);
    CHECK(chat_body["messages"][0].at("role") == "system");
    CHECK(chat_body["messages"][0].at("content") == "唯一系统提示");
    CHECK_FALSE(chat_body.contains("system"));
    CHECK_FALSE(chat_body.contains("instructions"));
    CHECK_FALSE(chat_body.contains("systemInstruction"));

    const auto anthropic_body = api::anthropic::BuildRequestJson(request);
    CHECK(anthropic_body.at("system") == "唯一系统提示");
    CHECK_FALSE(anthropic_body.contains("instructions"));
    CHECK_FALSE(anthropic_body.contains("systemInstruction"));
    CHECK(anthropic_body["messages"][0].at("role") == "user");

    const auto responses_body = api::responses::BuildRequestJson(request);
    CHECK(responses_body.at("instructions") == "唯一系统提示");
    CHECK_FALSE(responses_body.contains("system"));
    CHECK_FALSE(responses_body.contains("systemInstruction"));

    const auto gemini_body = api::gemini::BuildRequestJson(request);
    CHECK(gemini_body["systemInstruction"]["parts"][0].at("text") == "唯一系统提示");
    CHECK_FALSE(gemini_body.contains("system"));
    CHECK_FALSE(gemini_body.contains("instructions"));
    CHECK(gemini_body["contents"][0].at("role") == "user");  // contents 没有 system 一角

    // system 为空:四家谁都不带自家的键。
    api::Request no_system = request;
    no_system.system.clear();
    CHECK_FALSE(api::chat::BuildRequestJson(no_system)["messages"][0].at("role") == "system");
    CHECK_FALSE(api::anthropic::BuildRequestJson(no_system).contains("system"));
    CHECK_FALSE(api::responses::BuildRequestJson(no_system).contains("instructions"));
    CHECK_FALSE(api::gemini::BuildRequestJson(no_system).contains("systemInstruction"));
}
