// /doctor 诊断单的纯函数测试(本地兼容端 Effort 与前缀缓存诊断,2026-08):
//   - BuildEffortProbeRequest / DescribeRequestEffort:四种档位(含"不填")
//     都能在请求侧看见实际发送值;不填时字段确实缺席;
//   - ParsePrefixCacheMetrics:Prometheus 文本四项指标 + enable label,
//     没出现的名字留 nullopt,不拿 0 冒充;
//   - ClassifyCacheObservation:四态(not_reported/disabled/enabled_no_hit/
//     hit)同一个 0 不糊;
//   - BuildFixedPrefixProbePair / CommonPrefixBytes:两轮固定前缀只换最后
//     一句,序列化后公共前缀字节对得上设计值;
//   - IsLoopbackUrl / SanitizeProbeError:探针安全闸与错误清洗。

#include <doctest/doctest.h>

#include <iostream>
#include <memory>
#include <sstream>

#include "api/chat/request.hpp"
#include "app/commands/doctor_commands.hpp"
#include "app/commands/settings_commands.hpp"
#include "cli/i18n.hpp"
#include "config/config.hpp"

using namespace lubancode;

// 断言按中文文案钉死(DescribeRequestEffort/SanitizeProbeError 都走 tr),
// 不跟系统语言走。
TEST_CASE("doctor 测试语言基线") {
    cli::SetLanguage("zh-CN");
    CHECK(cli::tr("doctor.level.unset") == "未发送参数");
}

TEST_CASE("BuildEffortProbeRequest: 档位进请求,空串留空,max_tokens 极小") {
    const api::Request probe = app::BuildEffortProbeRequest("qwen-test", "xhigh");
    CHECK(probe.model == "qwen-test");
    CHECK(probe.reasoning_effort == "xhigh");
    CHECK(*probe.max_tokens <= 128);  // 极小探针,不烧预算
    REQUIRE(probe.messages.size() == 1);

    const api::Request unset = app::BuildEffortProbeRequest("qwen-test", "");
    CHECK(unset.reasoning_effort.empty());  // "不填"是正式状态:字段缺席

    // 巡检单 P1:effort 诊断可覆写预算——推理优先模型的思考会先吃满小预算,
    // 正文压根儿没机会产出,得留足。
    const api::Request budgeted = app::BuildEffortProbeRequest("qwen-test", "none", app::kEffortProbeBudgetTokens);
    CHECK(*budgeted.max_tokens == app::kEffortProbeBudgetTokens);
    CHECK(app::kEffortProbeRepeats >= 3);
}

TEST_CASE("SummarizeEffortProbeRounds: 四账分开,预算耗尽只判 inconclusive") {
    SUBCASE("空表:明说未发出") {
        const auto lines = app::SummarizeEffortProbeRounds({});
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].find("未发出") != std::string::npos);
    }
    SUBCASE("none 档三回仍产出思考:判词点破关闭未被证实") {
        std::vector<app::EffortProbeRoundResult> rounds(3);
        for (auto& r : rounds) {
            r.http_ok = true;
            r.thinking_chars = 150;
            r.text_chars = 40;
            r.stop_reason = "end_turn";
        }
        const auto lines = app::SummarizeEffortProbeRounds(rounds);
        REQUIRE(lines.size() >= 5);
        CHECK(lines[0].find("HTTP 接受:3/3") != std::string::npos);
        CHECK(lines[1].find("thinking 产出:3/3") != std::string::npos);
        CHECK(lines[2].find("正文产出:3/3") != std::string::npos);
        CHECK(lines[3].find("end_turn ×3") != std::string::npos);
        CHECK(lines[4].find("关闭未被端点证实") != std::string::npos);
    }
    SUBCASE("预算耗尽(stop=max_tokens 且正文 0)的回不判支持或不支持") {
        std::vector<app::EffortProbeRoundResult> rounds(3);
        for (auto& r : rounds) {
            r.http_ok = true;
            r.thinking_chars = 64;
            r.text_chars = 0;
            r.stop_reason = "max_tokens";
        }
        const auto lines = app::SummarizeEffortProbeRounds(rounds);
        bool saw_exhausted_note = false;
        bool saw_inconclusive = false;
        for (const auto& line : lines) {
            saw_exhausted_note = saw_exhausted_note || line.find("预算耗尽") != std::string::npos;
            saw_inconclusive = saw_inconclusive || line.find("inconclusive") != std::string::npos;
        }
        CHECK(saw_exhausted_note);
        CHECK(saw_inconclusive);
    }
    SUBCASE("部分耗尽:有效回照判,分布把两因都列出来") {
        std::vector<app::EffortProbeRoundResult> rounds(3);
        rounds[0] = {true, 200, 0, "max_tokens"};
        rounds[1] = {true, 0, 60, "end_turn"};
        rounds[2] = {true, 0, 55, "end_turn"};
        const auto lines = app::SummarizeEffortProbeRounds(rounds);
        std::string joined;
        for (const auto& line : lines) {
            joined += line + "\n";
        }
        CHECK(joined.find("max_tokens ×1") != std::string::npos);
        CHECK(joined.find("end_turn ×2") != std::string::npos);
        CHECK(joined.find("有效 2 回") != std::string::npos);
        CHECK(joined.find("inconclusive") == std::string::npos);  // 有有效回,不整单判挂起
    }
    SUBCASE("非 2xx 的回不算 HTTP 接受,终止原因未回单独记账") {
        std::vector<app::EffortProbeRoundResult> rounds(2);
        rounds[0] = {true, 0, 30, "end_turn"};
        rounds[1] = {false, 0, 0, ""};
        const auto lines = app::SummarizeEffortProbeRounds(rounds);
        CHECK(lines[0].find("HTTP 接受:1/2") != std::string::npos);
        CHECK(lines[3].find("(未回) ×1") != std::string::npos);
    }
}

TEST_CASE("DescribeRequestEffort: chat wire 四种档位与不填,实际发送值如实报") {
    SUBCASE("不填:字段确实缺席") {
        const api::Request probe = app::BuildEffortProbeRequest("m", "");
        const std::string text =
            app::DescribeRequestEffort(config::Wire::ChatCompletions, probe, nlohmann::json::object(), "");
        CHECK(text.find("reasoning_effort") != std::string::npos);
        CHECK(text.find("未发送参数") != std::string::npos);
    }
    SUBCASE("填 low:报告参数名与值") {
        const api::Request probe = app::BuildEffortProbeRequest("m", "low");
        const std::string text =
            app::DescribeRequestEffort(config::Wire::ChatCompletions, probe, nlohmann::json::object(), "");
        CHECK(text.find("reasoning_effort = \"low\"") != std::string::npos);
    }
    SUBCASE("provider 声明的参数名生效") {
        const api::Request probe = app::BuildEffortProbeRequest("m", "medium");
        const std::string text =
            app::DescribeRequestEffort(config::Wire::ChatCompletions, probe, nlohmann::json::object(),
                                       "reasoning.effort");
        CHECK(text.find("reasoning.effort = \"medium\"") != std::string::npos);
    }
    SUBCASE("extra_body 压过内置字段,报压后的值") {
        const api::Request probe = app::BuildEffortProbeRequest("m", "low");
        const std::string text = app::DescribeRequestEffort(
            config::Wire::ChatCompletions, probe, nlohmann::json{{"reasoning_effort", "override"}},
            "");
        CHECK(text.find("override") != std::string::npos);
    }
}

TEST_CASE("DescribeRequestEffort: responses/anthropic wire 各报各的参数位置") {
    SUBCASE("responses: reasoning.effort") {
        const api::Request probe = app::BuildEffortProbeRequest("m", "high");
        const std::string text =
            app::DescribeRequestEffort(config::Wire::Responses, probe, nlohmann::json::object(), "whatever");
        CHECK(text.find("reasoning.effort = \"high\"") != std::string::npos);  // responses 不吃 think_param
    }
    SUBCASE("anthropic: 映射后的 budget_tokens") {
        api::Request probe = app::BuildEffortProbeRequest("m", "medium");
        probe.max_tokens = 65536;  // 别让 budget 被 max_tokens 夹小
        const std::string text =
            app::DescribeRequestEffort(config::Wire::Anthropic, probe, nlohmann::json::object(), "");
        CHECK(text.find("thinking.budget_tokens = 4096") != std::string::npos);
        CHECK(text.find("medium") != std::string::npos);
    }
    SUBCASE("anthropic: none 档报 disabled") {
        api::Request probe = app::BuildEffortProbeRequest("m", "none");
        const std::string text =
            app::DescribeRequestEffort(config::Wire::Anthropic, probe, nlohmann::json::object(), "");
        CHECK(text.find("disabled") != std::string::npos);
    }
}

TEST_CASE("ParsePrefixCacheMetrics: vLLM 现场(计数器带 label、多行同名)解析正确") {
    const std::string text =
        "# HELP vllm:prefix_cache_queries_total Number of queries to the prefix cache\n"
        "# TYPE vllm:prefix_cache_queries_total counter\n"
        "vllm:prefix_cache_queries_total{reason=\"new_block\"} 3.0\n"
        "vllm:prefix_cache_queries_total{reason=\"check_hit\"} 0.0\n"
        "# TYPE vllm:prefix_cache_hits_total counter\n"
        "vllm:prefix_cache_hits_total 0.0\n"
        "# TYPE vllm:gpu_prefix_cache_hits_total counter\n"
        "vllm:gpu_prefix_cache_hits_total 0.0\n"
        "# TYPE vllm:prompt_tokens_cached_total counter\n"
        "vllm:prompt_tokens_cached_total 0.0\n";
    const auto metrics = app::ParsePrefixCacheMetrics(text);
    // enable 是 label 形式才判(见下一条);这条现场没有 label,留 nullopt。
    CHECK_FALSE(metrics.enabled.has_value());
    CHECK(metrics.queries_total.has_value());
    CHECK(*metrics.queries_total == 0);  // 同名多行取最后一份(与 vLLM 输出一致:末行是总计)
    CHECK(metrics.hits_total.has_value());
    CHECK(*metrics.hits_total == 0);
    CHECK(metrics.prompt_tokens_cached_total.has_value());
    CHECK(*metrics.prompt_tokens_cached_total == 0);
}

TEST_CASE("ParsePrefixCacheMetrics: label 形式的 enable_prefix_caching 判得出真假") {
    const std::string disabled =
        "vllm:cache_config_info{enable_prefix_caching=\"False\",block_size=\"16\"} 1.0\n"
        "vllm:prefix_cache_queries_total 0\n"
        "vllm:prefix_cache_hits_total 0\n"
        "vllm:prompt_tokens_cached_total 0\n";
    const auto off = app::ParsePrefixCacheMetrics(disabled);
    REQUIRE(off.enabled.has_value());
    CHECK(*off.enabled == false);

    const std::string enabled =
        "vllm:cache_config_info{enable_prefix_caching=\"True\"} 1.0\n";
    const auto on = app::ParsePrefixCacheMetrics(enabled);
    REQUIRE(on.enabled.has_value());
    CHECK(*on.enabled == true);
    // 没出现的计数器留 nullopt,不拿 0 冒充。
    CHECK_FALSE(on.queries_total.has_value());
    CHECK_FALSE(on.hits_total.has_value());
}

TEST_CASE("ClassifyCacheObservation: 四态同一个 0 不糊") {
    using app::CacheObservation;
    // usage 没回:无论服务端什么状态,先记 not_reported。
    CHECK(app::ClassifyCacheObservation(false, 0, std::nullopt) == CacheObservation::NotReported);
    CHECK(app::ClassifyCacheObservation(false, 0, false) == CacheObservation::NotReported);
    // 命中大于 0:hit(哪怕 metrics 说禁用,以实测为准)。
    CHECK(app::ClassifyCacheObservation(true, 512, std::nullopt) == CacheObservation::Hit);
    CHECK(app::ClassifyCacheObservation(true, 512, false) == CacheObservation::Hit);
    // 0 命中 + metrics 说禁用:disabled(8000 现场不再显示含糊的 0%)。
    CHECK(app::ClassifyCacheObservation(true, 0, false) == CacheObservation::Disabled);
    // 0 命中,metrics 说开:enabled_no_hit。
    CHECK(app::ClassifyCacheObservation(true, 0, true) == CacheObservation::EnabledNoHit);
    // 0 命中,没读过 metrics:也归 enabled_no_hit——措辞由显示层带
    // "未验证",这里只钉分类不糊成 disabled。
    CHECK(app::ClassifyCacheObservation(true, 0, std::nullopt) == CacheObservation::EnabledNoHit);
}

TEST_CASE("BuildFixedPrefixProbePair: 同 system 同前缀,只换最后一句") {
    const auto pair = app::BuildFixedPrefixProbePair("m", 512);
    REQUIRE(pair.round1.messages.size() == 2);
    REQUIRE(pair.round2.messages.size() == 2);
    // 第一条消息(固定前缀)逐字节一致。
    const auto& p1 = std::get<api::TextBlock>(pair.round1.messages[0].content[0]);
    const auto& p2 = std::get<api::TextBlock>(pair.round2.messages[0].content[0]);
    CHECK(p1.text == p2.text);
    CHECK(p1.text.size() >= 512);
    // 最后一句确实不同。
    const auto& t1 = std::get<api::TextBlock>(pair.round1.messages[1].content[0]);
    const auto& t2 = std::get<api::TextBlock>(pair.round2.messages[1].content[0]);
    CHECK(t1.text != t2.text);
    // 设计前缀 = system + 固定消息正文字节。
    CHECK(pair.designed_prefix_bytes == pair.round1.system.size() + p1.text.size());
}

TEST_CASE("CommonPrefixBytes: 两轮 chat 请求序列化后的公共前缀对上设计值") {
    const auto pair = app::BuildFixedPrefixProbePair("m", 1024);
    const std::string dump1 = api::chat::BuildRequestJson(pair.round1).dump();
    const std::string dump2 = api::chat::BuildRequestJson(pair.round2).dump();
    const std::size_t common = app::CommonPrefixBytes(dump1, dump2);
    // JSON 键序稳定(nlohmann 默认按字典序),前缀段一致 → 公共前缀至少
    // 覆到设计前缀(quote/转义会再多一些,只钉下界)。
    CHECK(common >= pair.designed_prefix_bytes);
    CHECK(app::CommonPrefixBytes("", "") == 0);
    CHECK(app::CommonPrefixBytes("abcX", "abcY") == 3);
}

TEST_CASE("IsLoopbackUrl: 本机地址的判定") {
    CHECK(app::IsLoopbackUrl("http://127.0.0.1:8000/v1"));
    CHECK(app::IsLoopbackUrl("http://localhost:8000"));
    CHECK(app::IsLoopbackUrl("http://[::1]:9000/v1"));
    CHECK(app::IsLoopbackUrl("https://127.0.0.4/metrics"));
    CHECK_FALSE(app::IsLoopbackUrl("https://api.deepseek.com/v1"));
    CHECK_FALSE(app::IsLoopbackUrl("https://api.example.com"));
    CHECK_FALSE(app::IsLoopbackUrl("not a url"));
    CHECK_FALSE(app::IsLoopbackUrl(""));
    CHECK_FALSE(app::IsLoopbackUrl("http://128.0.0.1"));  // 128 不是 127
}

TEST_CASE("SanitizeProbeError: 换行压平、控制字符剥掉、超长截断") {
    CHECK(app::SanitizeProbeError("line1\nline2\r\nline3") == "line1 line2 line3");
    const std::string with_ctrl = std::string("ab") + char(1) + "cd";
    CHECK(app::SanitizeProbeError(with_ctrl) == "abcd");
    const std::string truncated = app::SanitizeProbeError(std::string(5000, 'x'));
    CHECK(truncated.size() < 5000);
    CHECK(truncated.find("截断") != std::string::npos);
}

namespace {
// 捕获 std::cout 的最小脚手架(写法同 test_status_refresh)。
struct CoutCapture {
    std::ostringstream buffer;
    std::streambuf* old_buf = nullptr;
    CoutCapture() : old_buf(std::cout.rdbuf(buffer.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(old_buf); }
    std::string text() const { return buffer.str(); }
};
}  // namespace

TEST_CASE("/think 裸敲: 不填是正式状态,声明三层找,没声明明说未经能力验证") {
    cli::SetLanguage("zh-CN");
    auto current_think = std::make_shared<std::string>();

    SUBCASE("不填:显示'未发送参数',不是空着也不是默认档") {
        CoutCapture capture;
        app::HandleThinkCommand("", current_think, nullptr, {}, "");
        const std::string text = capture.text();
        CHECK(text.find("未发送参数") != std::string::npos);
        CHECK(text.find("未经能力验证") != std::string::npos);
    }
    SUBCASE("provider 声明:列出档位与参数名") {
        CoutCapture capture;
        app::HandleThinkCommand("", current_think, nullptr, {"low", "medium", "xhigh"}, "reasoning_effort");
        const std::string text = capture.text();
        CHECK(text.find("provider 声明的档位") != std::string::npos);
        CHECK(text.find("reasoning_effort") != std::string::npos);
        CHECK(text.find("xhigh") != std::string::npos);
        CHECK(text.find("未经能力验证") == std::string::npos);
    }
    SUBCASE("切档:声明表内的档标注在表内,表外的标注仍会发送") {
        *current_think = "low";
        CoutCapture in_table;
        app::HandleThinkCommand("low", current_think, nullptr, {"low", "xhigh"}, "");
        CHECK(in_table.text().find("在 provider 声明表内") != std::string::npos);
        CoutCapture out_table;
        app::HandleThinkCommand("turbo", current_think, nullptr, {"low", "xhigh"}, "");
        CHECK(out_table.text().find("不在 provider 声明表内") != std::string::npos);
        CHECK(*current_think == "turbo");
    }
    SUBCASE("切档且没任何声明:明说未经能力验证仍发送") {
        CoutCapture capture;
        app::HandleThinkCommand("xhigh", current_think, nullptr, {}, "");
        CHECK(capture.text().find("未经能力验证") != std::string::npos);
        CHECK(*current_think == "xhigh");
    }
}

TEST_CASE("/think 模型目录声明优先于 provider 声明") {
    cli::SetLanguage("zh-CN");
    config::ModelCatalogEntry entry;
    entry.slug = "qwen-cat";
    entry.supported_think_levels = {{"high", "认真想"}};
    auto current_think = std::make_shared<std::string>("high");
    CoutCapture capture;
    app::HandleThinkCommand("", current_think, &entry, {"low"}, "");
    const std::string text = capture.text();
    CHECK(text.find("模型目录声明的档位") != std::string::npos);
    CHECK(text.find("认真想") != std::string::npos);
    CHECK(text.find("provider 声明的档位") == std::string::npos);
}

TEST_CASE("/think none: 目录声明关不掉时,切换前后都明说未证实可关") {
    // MiniCPM5 真机巡检单 P1:vLLM 现场三组 disabled 全回非空 thinking——
    // none 看似可选实则关不掉。目录声明 always_think/off_unsupported 的
    // 模型,切档与裸敲都要亮"此端点未证实可关",别让状态栏白挂一枚 none。
    cli::SetLanguage("zh-CN");
    config::ModelCatalogEntry entry;
    entry.slug = "MiniCPM5-1B";
    entry.capabilities["off_unsupported"] = true;
    auto current_think = std::make_shared<std::string>("high");

    SUBCASE("切换到 none:切上去这一下就说清") {
        CoutCapture capture;
        app::HandleThinkCommand("none", current_think, &entry, {}, "");
        CHECK(capture.text().find("此端点未证实可关") != std::string::npos);
        CHECK(*current_think == "none");  // 档照切、请求照发,只加明说
    }
    SUBCASE("裸敲时已挂在 none:也明说") {
        *current_think = "none";
        CoutCapture capture;
        app::HandleThinkCommand("", current_think, &entry, {}, "");
        CHECK(capture.text().find("此端点未证实可关") != std::string::npos);
    }
    SUBCASE("没声明的模型不啰嗦") {
        config::ModelCatalogEntry plain;
        plain.slug = "plain";
        CoutCapture capture;
        app::HandleThinkCommand("none", current_think, &plain, {}, "");
        CHECK(capture.text().find("此端点未证实可关") == std::string::npos);
    }
    SUBCASE("非 none 档不提这句") {
        CoutCapture capture;
        app::HandleThinkCommand("high", current_think, &entry, {}, "");
        CHECK(capture.text().find("此端点未证实可关") == std::string::npos);
    }
}
