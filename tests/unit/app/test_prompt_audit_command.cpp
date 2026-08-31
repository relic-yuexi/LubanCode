// /prompt audit 命令面的纯函数册(Token 账本单 A3):二级参数拆词、
// 终端渲染(事实账/发现/coverage/口径行)与 JSON 报告面 schema、
// 隐私红线(正文与 canary 不进 JSON)。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/commands/prompt_audit_commands.hpp"

using namespace lubancode;
using namespace lubancode::app;
using Mode = ParsedPromptAuditCommand::Mode;

namespace {

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
    finding.scope = "config";
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

TEST_CASE("ParsePromptAuditCommand:五种形态/json/since/坏词") {
    CHECK(ParsePromptAuditCommand("").mode == Mode::Invalid);

    const auto static_cmd = ParsePromptAuditCommand("static --json");
    CHECK(static_cmd.mode == Mode::Static);
    CHECK(static_cmd.json);

    const auto runtime = ParsePromptAuditCommand("runtime 20260830-000001-FX001");
    CHECK(runtime.mode == Mode::Runtime);
    CHECK(runtime.session_id == "20260830-000001-FX001");

    const auto runtime_bare = ParsePromptAuditCommand("runtime");
    CHECK(runtime_bare.mode == Mode::Runtime);
    CHECK(runtime_bare.session_id.empty());

    const auto runtime_json = ParsePromptAuditCommand("runtime --json");
    CHECK(runtime_json.mode == Mode::Runtime);
    CHECK(runtime_json.json);
    CHECK(runtime_json.session_id.empty());

    const auto outcome = ParsePromptAuditCommand("outcome --since 7d");
    CHECK(outcome.mode == Mode::Outcome);
    CHECK(outcome.since_days == 7);

    const auto outcome_default = ParsePromptAuditCommand("outcome");
    CHECK(outcome_default.since_days == 30);

    const auto all = ParsePromptAuditCommand("all --json");
    CHECK(all.mode == Mode::All);
    CHECK(all.json);

    const auto explain = ParsePromptAuditCommand("explain P-AUD-R02");
    CHECK(explain.mode == Mode::Explain);
    CHECK(explain.finding_id == "P-AUD-R02");

    const auto review = ParsePromptAuditCommand("all --model-review");
    CHECK(review.later_model_review);
    CHECK(!review.invalid);

    CHECK(ParsePromptAuditCommand("explain").invalid);
    CHECK(ParsePromptAuditCommand("blah").invalid);
    CHECK(ParsePromptAuditCommand("blah").bad_word == "blah");
    CHECK(ParsePromptAuditCommand("static --since").invalid);
    CHECK(ParsePromptAuditCommand("static --since xyz").bad_word == "xyz");
}

TEST_CASE("FormatPromptAuditReport:标题/事实账/发现/口径行") {
    PromptAuditReportModel model;
    model.mode = "static";
    model.has_static = true;
    model.facts.system_tokens = 4200;
    model.facts.soul_tokens = 0;
    model.facts.model_instructions_tokens = 220;
    model.facts.tool_definition_tokens = 9200;
    model.facts.tool_count = 42;
    model.facts.total_context_tokens = 13620;
    model.facts.budget_tokens = 0;
    model.facts.segments.push_back(
        insights::PromptAuditFacts::SegmentFact{"core/10-identity.md", "core", "embedded",
                                                1320, 10, false});
    model.facts.segments.push_back(
        insights::PromptAuditFacts::SegmentFact{"runtime/environment", "runtime",
                                                "host_generated", 180, 90, true});
    model.findings.push_back(SampleFinding());

    const std::string text = Join(FormatPromptAuditReport(model));
    CHECK(text.find("Prompt audit · static") != std::string::npos);
    CHECK(text.find("system 4200") != std::string::npos);
    CHECK(text.find("工具定义 9200(42 枚)") != std::string::npos);
    CHECK(text.find("预算未知,占比不判") != std::string::npos);
    CHECK(text.find("core/10-identity.md 1320") != std::string::npos);
    CHECK(text.find("runtime/environment") != std::string::npos);
    CHECK(text.find("(动态)") != std::string::npos);
    CHECK(text.find("发现 1 条") != std::string::npos);
    CHECK(text.find("P-AUD-S02 · warning · 证据置信 high · prompt.duplicate_content") !=
          std::string::npos);
    CHECK(text.find("建议: 重复段留一份") != std::string::npos);
    CHECK(text.find("口径        只摆事实;prompt 正文与绝对路径不进报告") != std::string::npos);

    // 零发现不硬凑。
    PromptAuditReportModel empty_model;
    empty_model.mode = "static";
    const std::string empty_text = Join(FormatPromptAuditReport(empty_model));
    CHECK(empty_text.find("发现 0 条") != std::string::npos);
    CHECK(empty_text.find("A6") != std::string::npos);
}

TEST_CASE("FormatPromptAuditReport:runtime 逐请求表与 outcome coverage 单列") {
    PromptAuditReportModel model;
    model.mode = "runtime";
    model.session_id = "20260831-000001-PA0001";
    model.provisional = true;
    insights::RuntimeRequestView view;
    view.run_id = "main-0001";
    view.request_id = "req-0001";
    view.event_id = "main-0001:evt-00000101";
    view.purpose = "main_turn";
    agent::RequestSnapshotMetadata snapshot;
    snapshot.request_shape.tool_count = 42;
    snapshot.request_shape.tool_definition_tokens_estimated = 9200;
    snapshot.request_shape.message_count = 17;
    view.snapshot = snapshot;
    view.usage_reported = true;
    view.total_input_tokens = 49200;
    view.cache_read_tokens = 48000;
    model.requests.push_back(view);
    insights::RuntimeRequestView bare;
    bare.request_id = "req-0002";
    bare.event_id = "e2";
    bare.purpose = "unknown";
    model.requests.push_back(bare);

    model.has_outcome = true;
    model.sessions_found = 5;
    model.status_counts["analyzed"] = 2;
    model.status_counts["active"] = 1;
    model.status_counts["incomplete"] = 1;
    model.status_counts["corrupt"] = 1;
    model.scan.push_back(insights::WorkspaceScanEntry{
        "20260831-000009-BAD009", insights::SessionGateStatus::Corrupt,
        "gate.corrupt: hash 链断,整间排除"});
    model.scan.push_back(
        insights::WorkspaceScanEntry{"20260831-000010-OK010",
                                     insights::SessionGateStatus::Analyzed, ""});

    const std::string text = Join(FormatPromptAuditReport(model));
    CHECK(text.find("(未封口 provisional)") != std::string::npos);
    CHECK(text.find("请求 req-0001  purpose=main_turn") != std::string::npos);
    CHECK(text.find("tools 42 枚/9200") != std::string::npos);
    CHECK(text.find("messages 17") != std::string::npos);
    CHECK(text.find("cache 读 98%") != std::string::npos);
    CHECK(text.find("无 snapshot") != std::string::npos);
    CHECK(text.find("场次        found 5") != std::string::npos);
    CHECK(text.find("corrupt 1") != std::string::npos);
    CHECK(text.find("排除 20260831-000009-BAD009") != std::string::npos);
    // analyzed 且无理由的场不打排除行。
    CHECK(text.find("20260831-000010-OK010") == std::string::npos);
}

TEST_CASE("BuildPromptAuditJson:schema、privacy 声明与 canary 红线") {
    PromptAuditReportModel model;
    model.mode = "all";
    model.session_id = "s";
    model.has_static = true;
    model.facts.system_tokens = 100;
    model.findings.push_back(SampleFinding());
    insights::FeatureSignal signal;
    signal.signal_id = "FS-01";
    signal.feature = "/skill 或 /workflow";
    signal.summary = "同类验证跑了 3 次";
    signal.precondition = "步骤稳定已证";
    model.signals.push_back(signal);

    const nlohmann::json json = BuildPromptAuditJson(model);
    CHECK(json.at("schema") == "lubancode.prompt.audit");
    CHECK(json.at("schema_version") == 1);
    CHECK(json.at("rule_version") == "prompt-audit-v1");
    CHECK(json.at("mode") == "all");
    CHECK(json.at("privacy").at("content_policy") == "metadata_only");
    CHECK(json.at("privacy").at("prompt_text_included") == false);
    CHECK(json.at("findings").size() == 1);
    CHECK(json.at("findings")[0].at("finding_id") == "P-AUD-S02");
    CHECK(json.at("findings")[0].at("severity") == "warning");
    CHECK(json.at("findings")[0].at("confidence") == "high");
    CHECK(json.at("feature_signals")[0].at("signal_id") == "FS-01");
    CHECK(json.at("static_facts").at("system_tokens_estimated") == 100);

    // 隐私红线:任何字段不携带 prompt 正文或 secret canary。
    const std::string dumped = json.dump();
    CHECK(dumped.find("sk-CANARY") == std::string::npos);
}
