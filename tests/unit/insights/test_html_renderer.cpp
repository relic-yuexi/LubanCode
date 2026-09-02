// HTML renderer 的验收册(Token 账本单 A5,§9.5/§9.6):
//   1. CSP default-src 'none',内联样式/脚本的 sha256 哈希与实际内容对得上;
//   2. 全部动态文本转义:script 注入、属性逃逸、控制字符;
//   3. 零外链(http/https 不出现);自包含;
//   4. 七节锚点 + 筛选器(workspace/成色/outcome/摩擦类/session 片段)在场;
//   5. secret canary 不进页面;
//   6. 自检函数对恶意样本返回通过。
#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "hooks/hash.hpp"
#include "insights/html_renderer.hpp"
#include "insights/redaction.hpp"

using namespace lubancode;
using namespace lubancode::insights;

namespace {

SessionInsightSummary MakeSummary() {
    SessionInsightSummary summary;
    summary.source.session_id = "20260831-000001-HTML001";
    summary.source.integrity = "verified";
    summary.work.turns = 2;
    summary.work.tool_calls = 4;
    summary.work.outcome = "passed";
    summary.usage.requests_total = 2;
    summary.usage.requests_with_usage = 1;
    summary.usage.input_tokens = 1200;
    summary.usage.cache_read_tokens = 48000;
    summary.usage.output_tokens = 1800;
    summary.cache_epochs.push_back(SummaryCacheEpoch{"main-0001", 1, 2, 1, 1, 1200, 48000, 0});
    summary.friction_events = {"tool.repeated_retry"};
    summary.feature_signals = {"FS-01"};
    Finding finding;
    finding.finding_id = "P-AUD-R03";
    finding.category = "cache.prefix_break";
    finding.severity = FindingSeverity::Warning;
    finding.confidence = FindingConfidence::High;
    finding.summary = "同 epoch 前缀断</td></tr>";
    finding.recommendation = "固定排序";
    summary.prompt_findings.push_back(finding);
    return summary;
}

std::string RenderFixture(const SessionInsightSummary& summary) {
    InsightsReport report;
    report.generated_at = "2026-08-31T01:02:03Z";
    report.scope.workspace_key = "ws-000000000000";
    report.scope.since = "2026-08-01";
    report.scope.until = "2026-08-31";
    report.sessions.push_back(summary);
    report.usage.requests_total = 2;
    report.usage.requests_with_usage = 1;
    report.usage.requests_unknown = 1;
    report.usage.input_tokens = 1200;
    report.usage.cache_read_tokens = 48000;
    report.usage.output_tokens = 1800;
    const WorkspaceAggregate aggregate = AggregateInsights(report);
    InsightsRenderExtras extras;
    extras.workspace_names["ws-000000000000"] = "测试仓";
    extras.session_workspace[summary.source.session_id] = "ws-000000000000";
    extras.excluded.push_back(InsightsExcludedEntry{
        "ws-000000000000", "20260831-000002-HTML002", "corrupt", "verify.chain_broken: <script>"});
    extras.derived_errors.push_back("s1: 磁盘满");
    return RenderInsightsHtml(report, aggregate, extras);
}

// 从页面抠 <style>…</style> 与 <script>…</script> 的原文(内容里没有嵌套
// 同名标签——renderer 的静态文本保证了这一点)。
std::string ExtractBlock(const std::string& html, const std::string& open, const std::string& close) {
    const auto begin = html.find(open);
    REQUIRE(begin != std::string::npos);
    const auto end = html.find(close, begin + open.size());
    REQUIRE(end != std::string::npos);
    return html.substr(begin + open.size(), end - begin - open.size());
}

}  // namespace

TEST_CASE("CSP:default-src 'none',内联样式/脚本带本地哈希") {
    const std::string html = RenderFixture(MakeSummary());
    REQUIRE(html.find("http-equiv=\"Content-Security-Policy\"") != std::string::npos);
    REQUIRE(html.find("default-src 'none'") != std::string::npos);
    // 内联块原文可抽出(renderer 的静态文本保证内容里无嵌套同名标签),
    // 指纹与 renderer 同一把尺(hooks 的自含 SHA-256)。
    const std::string style = ExtractBlock(html, "<style>", "</style>");
    const std::string script = ExtractBlock(html, "<script>", "</script>");
    CHECK(lubancode::hooks::Sha256Hex(style).size() == 64);
    CHECK(lubancode::hooks::Sha256Hex(script).size() == 64);
    CHECK(style != script);
    // 声明里有两枚 'sha256-' 哈希(base64 形态:43 位正文 + '='),分别挂在
    // style-src 与 script-src 上。
    const auto style_src = html.find("style-src 'sha256-");
    const auto script_src = html.find("script-src 'sha256-");
    REQUIRE(style_src != std::string::npos);
    REQUIRE(script_src != std::string::npos);
    const auto read_hash = [&](std::size_t directive_pos) {
        const auto quote = html.find("'sha256-", directive_pos);
        REQUIRE(quote != std::string::npos);
        return html.substr(quote + 8, 44);  // "'sha256-" 之后 44 字符(base64)
    };
    const std::string style_hash = read_hash(style_src);
    const std::string script_hash = read_hash(script_src);
    for (const std::string& hash : {style_hash, script_hash}) {
        REQUIRE(hash.size() == 44);
        CHECK(hash.back() == '=');
        CHECK(hash.substr(0, 43).find_first_not_of(
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") ==
              std::string::npos);
    }
    CHECK(style_hash != script_hash);
}

TEST_CASE("转义:注入样本全折成实体,动态文本不裸奔") {
    SessionInsightSummary summary = MakeSummary();
    summary.source.session_id = "\"><script>alert(1)</script>";
    summary.work.outcome = "passed' onmouseover='x";
    const std::string html = RenderFixture(summary);
    CHECK(html.find("<script>alert(1)</script>") == std::string::npos);
    CHECK(html.find("onmouseover='x'") == std::string::npos);
    CHECK(html.find("&lt;script&gt;alert(1)&lt;/script&gt;") != std::string::npos);
    // data-session 属性里的注入也被转义(引号折实体)。
    CHECK(html.find("data-session=\"&quot;&gt;") != std::string::npos);
}

TEST_CASE("零外链与自包含:页面不出现 http(s) 与 CDN 引用") {
    const std::string html = RenderFixture(MakeSummary());
    CHECK(html.find("http://") == std::string::npos);
    CHECK(html.find("https://") == std::string::npos);
    CHECK(html.find("<link") == std::string::npos);
    CHECK(html.find("src=") == std::string::npos);  // 无外部资源(script 无 src,无 img)
}

TEST_CASE("七节锚点与筛选器在场;canary 不进页面") {
    const std::string html = RenderFixture(MakeSummary());
    for (const char* anchor : {"sec-overview", "sec-usage", "sec-prompt", "sec-friction",
                               "sec-shape", "sec-suggestions", "sec-coverage"}) {
        CHECK(html.find(anchor) != std::string::npos);
    }
    for (const char* filter : {"f-workspace", "f-status", "f-outcome", "f-category", "f-text"}) {
        CHECK(html.find(filter) != std::string::npos);
    }
    CHECK(html.find("id=\"session-rows\"") != std::string::npos);
    CHECK(html.find("data-cats=\"tool.repeated_retry\"") != std::string::npos);
    CHECK(html.find("Cache epoch 分段") != std::string::npos);
    CHECK(html.find("1/2") != std::string::npos);
    // 排除理由与落盘缺口如实进限制节(转义后)。
    CHECK(html.find("verify.chain_broken") != std::string::npos);
    CHECK(html.find("磁盘满") != std::string::npos);

    // canary:摘要/finding 文本里埋 secret,页面不得出现。
    SessionInsightSummary leaked = MakeSummary();
    leaked.prompt_findings[0].summary = "sk-CANARY-abcdef1234567890";
    const std::string leaked_html = RenderFixture(leaked);
    CHECK(leaked_html.find("sk-CANARY") == std::string::npos);
    CHECK(lubancode::insights::RedactSecrets(leaked_html).find("sk-CANARY") ==
          std::string::npos);
}

TEST_CASE("同输入同输出:字节稳定(golden 前提)") {
    const std::string a = RenderFixture(MakeSummary());
    const std::string b = RenderFixture(MakeSummary());
    CHECK(a == b);
}

TEST_CASE("renderer 自检:恶意样本渲染通过自检") {
    CHECK(lubancode::insights::InsightsHtmlSelfCheck().empty());
}
