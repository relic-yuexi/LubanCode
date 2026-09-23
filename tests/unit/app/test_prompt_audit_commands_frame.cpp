// TUI 排版批 5b(tui优化todo.todo:/prompt audit 报告)的输出形状册。
//   - 报告纯函数 FormatPromptAuditReport(model, theme, width):static 事实
//     账/场次/口径进键值对框,标题嵌框顶;runtime 逐请求进表格(request/
//     purpose/tools/messages/cache 列);发现进主表(id/severity/
//     confidence/category,severity 上语义色)+ 附注列表;outcome 排除行进
//     列表;信号进列表(先决挂行尾 hint);
//   - 入口错误路径(invalid 参数/explain 查无 finding)进 error 档键值对框;
//     --json 机器面字节级不变,不走 frame(合同第 2 条,另有册钉 JSON 面);
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条。

#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/commands/prompt_audit_commands.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"

using namespace lubancode;
using namespace lubancode::app;
using Mode = ParsedPromptAuditCommand::Mode;

namespace {

class OutputCapture {
public:
    OutputCapture() { cli::TermPort().Redirect(&stream_, nullptr); }
    ~OutputCapture() { cli::TermPort().Reset(); }
    std::string text() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

std::string StripAnsi(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) {
                ++i;
            }
            if (i < text.size()) {
                ++i;  // 吃掉终结字母
            }
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │

std::string Join(const std::vector<std::string>& lines) {
    std::string joined;
    for (const auto& line : lines) {
        joined += line + "\n";
    }
    return joined;
}

insights::Finding SampleFinding() {
    insights::Finding finding;
    finding.finding_id = "P-AUD-S02";
    finding.category = "prompt.duplicate_content";
    finding.severity = insights::FindingSeverity::Warning;
    finding.confidence = insights::FindingConfidence::High;
    finding.summary = "有 1 组段正文 hash 完全相同";
    finding.recommendation = "重复段留一份";
    finding.rule_version = "prompt-audit-v1:S02";
    insights::EvidenceItem evidence;
    evidence.metric = "duplicate_groups";
    evidence.value = nlohmann::json::array({"a.md 与 1 个别的段同文"});
    finding.evidence.push_back(evidence);
    return finding;
}

}  // namespace

TEST_CASE("static 报告: 事实账/口径进键值对框,标题嵌框顶") {
    PromptAuditReportModel model;
    model.mode = "static";
    model.has_static = true;
    model.facts.system_tokens = 4200;
    model.facts.soul_tokens = 0;
    model.facts.model_instructions_tokens = 220;
    model.facts.tool_definition_tokens = 9200;
    model.facts.tool_count = 42;
    model.facts.segments.push_back(insights::PromptAuditFacts::SegmentFact{
        "core/10-identity.md", "core", "embedded", 1320, 10, false});

    const cli::Theme theme = cli::BuiltinTheme("dark");
    const std::string text = Join(FormatPromptAuditReport(model, theme, 0));
    REQUIRE(Contains(text, kBoxLightTopLeft));
    const std::string plain = StripAnsi(text);
    CHECK(Contains(plain, "Prompt audit · static"));
    CHECK(Contains(plain, "构成"));
    CHECK(Contains(plain, "system 4200"));
    CHECK(Contains(plain, "段级 Top"));
    CHECK(Contains(plain, "口径"));
    // key 列走 row_label 色。
    CHECK(Contains(text, theme.row_label + "构成"));
}

TEST_CASE("发现表: severity 上语义色,附注列表挂 finding_id 对回主表") {
    PromptAuditReportModel model;
    model.mode = "static";
    model.findings.push_back(SampleFinding());

    const cli::Theme theme = cli::BuiltinTheme("dark");
    const std::string text = Join(FormatPromptAuditReport(model, theme, 0));
    const std::string plain = StripAnsi(text);
    // 主表标题与列头。
    CHECK(Contains(plain, "发现 1 条"));
    CHECK(Contains(plain, "id"));
    CHECK(Contains(plain, "severity"));
    CHECK(Contains(plain, "confidence"));
    CHECK(Contains(plain, "category"));
    CHECK(Contains(plain, "P-AUD-S02"));
    // warning 走 skip 档(info 走 pass、high 走 error,同一张表)。
    CHECK(Contains(text, theme.table_skip + "warning"));
    // 附注列表:摘要/建议/证据原文一字不少,行头挂 finding_id。
    CHECK(Contains(plain, "有 1 组段正文 hash 完全相同"));
    CHECK(Contains(plain, "建议: 重复段留一份"));
    CHECK(Contains(plain, "duplicate_groups"));
}

TEST_CASE("runtime 报告: 逐请求进表格,排除行进列表") {
    PromptAuditReportModel model;
    model.mode = "runtime";
    model.session_id = "20260831-000001-PA0001";
    model.provisional = true;
    insights::RuntimeRequestView view;
    view.request_id = "req-0001";
    view.purpose = "main_turn";
    view.usage_reported = true;
    view.total_input_tokens = 49200;
    view.cache_read_tokens = 48000;
    model.requests.push_back(view);
    model.has_outcome = true;
    model.sessions_found = 5;
    model.status_counts["analyzed"] = 2;
    model.scan.push_back(insights::WorkspaceScanEntry{
        "20260831-000009-BAD009", insights::SessionGateStatus::Corrupt, "gate.corrupt: hash 链断"});

    const cli::Theme theme = cli::BuiltinTheme("dark");
    const std::string text = Join(FormatPromptAuditReport(model, theme, 0));
    const std::string plain = StripAnsi(text);
    // 标题带 session 与 provisional 注。
    CHECK(Contains(plain, "Prompt audit · runtime · 20260831-000001-PA0001(未封口 provisional)"));
    // 请求表格列头与内容。
    CHECK(Contains(plain, "request"));
    CHECK(Contains(plain, "purpose"));
    CHECK(Contains(plain, "req-0001"));
    CHECK(Contains(plain, "main_turn"));
    CHECK(Contains(plain, "读 98%"));
    // 场次键值与排除列表。
    CHECK(Contains(plain, "场次"));
    CHECK(Contains(plain, "found 5"));
    CHECK(Contains(plain, "排除 20260831-000009-BAD009"));
    CHECK(Contains(plain, "gate.corrupt: hash 链断"));
}

TEST_CASE("入口: invalid 参数与 explain 查无 finding 进 error 框") {
    const cli::Theme theme = cli::BuiltinTheme("dark");
    PromptAuditContext context{theme};
    {
        OutputCapture capture;
        HandlePromptAuditCommand("blah", context);
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "认不得的参数"));
        CHECK(Contains(StripAnsi(out), "用法"));
        CHECK(Contains(out, theme.error + "认不得的参数"));
    }
    {
        OutputCapture capture;
        HandlePromptAuditCommand("explain P-AUD-NOPE", context);
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "没有这条 finding"));
        CHECK(Contains(out, theme.error));
    }
}

TEST_CASE("--json 机器面不走 frame: 输出以 '{' 起头、零框字形") {
    const cli::Theme theme = cli::BuiltinTheme("dark");
    PromptAuditContext context{theme};
    OutputCapture capture;
    HandlePromptAuditCommand("static --json", context);
    const std::string out = capture.text();
    CHECK(!Contains(out, kBoxLightTopLeft));
    // JSON 面仍是裸 dump(RedactSecrets 后),首行以 { 起。
    const std::size_t first_non_ws = out.find_first_not_of(" \t\r\n");
    REQUIRE(first_non_ws != std::string::npos);
    CHECK(out[first_non_ws] == '{');
}

TEST_CASE("plain 主题: 报告零转义字节、无框字形(T3/--no-color 路径)") {
    PromptAuditReportModel model;
    model.mode = "static";
    model.has_static = true;
    model.facts.system_tokens = 100;
    model.findings.push_back(SampleFinding());
    const std::string text = Join(FormatPromptAuditReport(model, cli::BuiltinTheme("plain"), 0));
    CHECK(text.find("\x1b") == std::string::npos);
    CHECK(!Contains(text, kBoxLightTopLeft));
    CHECK(!Contains(text, kBoxLightVert));
    // 信息一字不少:构成/发现/口径都在,只是没了色与框。
    CHECK(Contains(text, "system 100"));
    CHECK(Contains(text, "P-AUD-S02"));
    CHECK(Contains(text, "只摆事实"));
}
