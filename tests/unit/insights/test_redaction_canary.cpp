// 脱敏与 canary 夹具(Token 账本单 §13.2/§15.6 A0):
//   - 扫得出各色 secret,整段替换,不保留前后几位;
//   - 报告字段 allowlist:report JSON 的每条路径都在白名单内;
//   - canary 从报告 JSON 里一只都捞不着。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "insights/finding.hpp"
#include "insights/redaction.hpp"
#include "insights/report_model.hpp"

using namespace lubancode::insights;

namespace {

// §15.6 的 canary:各种形状的真 secret 模样(全是假串,不涉任何真凭据)。
const char* kCanaries[] = {
    "sk-ANT-FAKE0000000000000000",
    "ghp_FAKE0000000000000000",
    "AKIAFAKE00000000",
    "Authorization: Bearer eyJhbGciOi_FAKE",
    "Cookie: session=FAKE1234567890",
    "-----BEGIN RSA PRIVATE KEY-----",
    "postgres://luban:FAKEPASS@db.internal:5432/app",
    "API_KEY=FAKE_KEY_000000000",
    "AUTH_TOKEN=FAKE_TOKEN_0000000",
};

void WalkPaths(const nlohmann::json& json, std::vector<std::string>* path,
               std::vector<std::vector<std::string>>* paths) {
    if (json.is_object()) {
        for (auto it = json.begin(); it != json.end(); ++it) {
            path->push_back(it.key());
            WalkPaths(it.value(), path, paths);
            path->pop_back();
        }
        return;
    }
    if (json.is_array()) {
        // 数组不进 path:元素按同名字段裁。
        for (const auto& item : json) {
            WalkPaths(item, path, paths);
        }
        return;
    }
    paths->push_back(*path);
}

}  // namespace

TEST_CASE("secret 扫描:各色形状都认得") {
    const std::string text =
        "Authorization: Bearer abc\n"
        "Cookie: x=y\n"
        "key sk-ANT-abc12345678 located\n"
        "-----BEGIN RSA PRIVATE KEY-----\n"
        "postgres://u:p@h:1/d\n"
        "API_KEY=zzz12345\n"
        "plain url https://example.com/x no secret";
    const auto hits = ScanSecrets(text);
    REQUIRE(hits.size() == 6);
    CHECK(hits[0].kind == SecretKind::AuthorizationHeader);
    CHECK(hits[1].kind == SecretKind::CookieHeader);
    CHECK(hits[2].kind == SecretKind::ApiKey);
    CHECK(hits[3].kind == SecretKind::PrivateKey);
    CHECK(hits[4].kind == SecretKind::ConnectionString);
    CHECK(hits[5].kind == SecretKind::EnvAssignment);
    // 裸 URL 不误报。
    const auto clean = ScanSecrets("see https://example.com/docs for details");
    CHECK(clean.empty());
}

TEST_CASE("替换:整段换 [REDACTED:kind],不保留前后几位") {
    // 凭据串整行替换:与" done"分行,验替换不吞并行的正文。
    const std::string redacted =
        RedactSecrets("token=sk-FAKE12345678 and postgres://u:secret@h/d\ndone");
    CHECK(redacted.find("sk-FAKE12345678") == std::string::npos);
    CHECK(redacted.find("secret@h") == std::string::npos);
    CHECK(redacted.find("token=[REDACTED:") == 0);
    CHECK(redacted.find("[REDACTED:connection_string]") != std::string::npos);
    CHECK(redacted.find("done") != std::string::npos);
    // 没命中原样返回。
    CHECK(RedactSecrets("nothing here") == std::string("nothing here"));
}

TEST_CASE("canary:过脱敏的报告 JSON 里一只都捞不着") {
    // 模拟分析器防线:finding 的 summary/recommendation 是唯一可能捎带
    // 正文的两栏,序列化前先过 RedactSecrets。
    Finding finding;
    finding.finding_id = "P-AUD-001";
    finding.category = "tool.invalid_input";
    finding.severity = FindingSeverity::Info;
    finding.confidence = FindingConfidence::Medium;
    finding.scope = "session";
    finding.summary = RedactSecrets(std::string("工具入参里带了密钥 ") + kCanaries[0] + " 与 " +
                                    kCanaries[6]);
    finding.recommendation = RedactSecrets(std::string("轮换 ") + kCanaries[5]);
    finding.origin = FindingOrigin::DeterministicRule;
    finding.rule_version = "prompt-audit-v1:P001";

    InsightsReport report;
    report.generated_at = "2026-08-30T00:00:00Z";
    report.scope.workspace_key = "ws-000000000000";
    report.findings.push_back(finding);
    const std::string dumped = report.ToJson().dump(2);
    for (const char* canary : kCanaries) {
        INFO(canary);
        CHECK(dumped.find(canary) == std::string::npos);
    }
    CHECK(dumped.find("[REDACTED:api_key]") != std::string::npos);
    CHECK(dumped.find("[REDACTED:connection_string]") != std::string::npos);
}

TEST_CASE("报告字段 allowlist:report JSON 每条路径都在白名单") {
    InsightsReport report;
    report.generated_at = "2026-08-30T00:00:00Z";
    report.scope.workspace_key = "ws-000000000000";
    report.usage.requests_total = 3;
    SessionInsightSummary summary;
    summary.source.session_id = "s1";
    summary.prompt_findings.push_back(Finding{});
    summary.prompt_findings[0].finding_id = "P-1";
    summary.prompt_findings[0].category = "c";
    summary.prompt_findings[0].summary = "x";
    summary.prompt_findings[0].recommendation = "y";
    summary.prompt_findings[0].rule_version = "v1";
    report.sessions.push_back(summary);

    const nlohmann::json json = report.ToJson();
    std::vector<std::string> path;
    std::vector<std::vector<std::string>> paths;
    WalkPaths(json, &path, &paths);
    REQUIRE(!paths.empty());
    for (const auto& candidate : paths) {
        std::string joined;
        for (const auto& part : candidate) {
            joined += part + "/";
        }
        INFO(joined.c_str());
        CHECK(IsAllowedReportFieldPath(candidate));
    }
    // 白名单外的路径点名拒绝。
    CHECK(!IsAllowedReportFieldPath({"prompt_text"}));
    CHECK(!IsAllowedReportFieldPath({"sessions", "messages"}));
    CHECK(!IsAllowedReportFieldPath({"findings", "evidence", "raw_content"}));
    CHECK(!IsAllowedReportFieldPath({}));
}
