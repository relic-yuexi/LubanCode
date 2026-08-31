// PromptAuditor 的规则册(Token 账本单 A3):static 十二规则的确定性命中、
// runtime 层变化账、读侧装配与隐私红线(正文/canary 不进输出)。
#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "insights/prompt_auditor.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/schema.hpp"

#include "insights_fixtures.hpp"

using namespace lubancode;
using namespace lubancode::insights;
using agent::PromptManifest;
using agent::PromptSegment;

namespace {

constexpr const char* kWorkspaceKey = "ws-000000000000";

PromptSegment Segment(const std::string& id, const std::string& rendered, std::int64_t tokens,
                      int order, bool volatile_segment = false,
                      std::vector<std::string> overrides = {}) {
    PromptSegment segment;
    segment.segment_id = id;
    segment.role = id.rfind("core/", 0) == 0 ? "core" : "other";
    segment.source_kind = "embedded";
    segment.source_ref = "embedded:" + id;
    segment.source_hash = hooks::Sha256Hex(rendered);
    segment.rendered_hash = hooks::Sha256Hex(rendered);
    segment.rendered_tokens_estimated = tokens;
    segment.order = order;
    segment.volatile_segment = volatile_segment;
    segment.overrides = std::move(overrides);
    return segment;
}

const Finding* FindByCategory(const std::vector<Finding>& findings, const std::string& category) {
    for (const auto& finding : findings) {
        if (finding.category == category) {
            return &finding;
        }
    }
    return nullptr;
}

// 一段足够长的正文(n-gram 测量要 ≥160 字节)。
std::string LongText(const std::string& seed, std::size_t repeats) {
    std::string text;
    for (std::size_t i = 0; i < repeats; ++i) {
        text += seed + " ";
    }
    return text;
}

}  // namespace

TEST_CASE("static:override 链、同文重复、漂移与段级事实账") {
    StaticAuditInput input;
    input.manifest.assembly_version = "prompt-assembler-v1";
    input.manifest.segments.push_back(Segment("core/10-identity.md", "A", 100, 0,
                                              false, {"embedded default"}));
    input.manifest.segments.push_back(Segment("features/dup-a.md", "SAME", 200, 1));
    input.manifest.segments.push_back(Segment("features/dup-b.md", "SAME", 200, 2));
    input.manifest.resolved_prompt_tokens_estimated = 500;
    input.module_sources.push_back(agent::PromptModuleSource{"core/20-rules.md", true, true});
    input.module_sources.push_back(agent::PromptModuleSource{"core/30-tone.md", true, false});

    PromptAuditFacts facts;
    const std::vector<Finding> findings = AuditPromptStatic(input, &facts);

    // S01:override 链是事实账(info,不判罪)。
    const Finding* override_finding = FindByCategory(findings, "prompt.override_chain");
    REQUIRE(override_finding != nullptr);
    CHECK(override_finding->finding_id == "P-AUD-S01");
    CHECK(override_finding->severity == FindingSeverity::Info);
    CHECK(override_finding->confidence == FindingConfidence::High);
    CHECK(override_finding->rule_version == "prompt-audit-v1:S01");

    // S02:同文重复(hash 相同)。
    const Finding* duplicate = FindByCategory(findings, "prompt.duplicate_content");
    REQUIRE(duplicate != nullptr);
    CHECK(duplicate->finding_id == "P-AUD-S02");
    CHECK(duplicate->severity == FindingSeverity::Warning);

    // S12:用户模块漂移(只点名,不展开差异)。
    const Finding* drift = FindByCategory(findings, "prompt.user_module_drift");
    REQUIRE(drift != nullptr);
    CHECK(drift->evidence[0].value.dump().find("core/20-rules.md") != std::string::npos);

    // 事实账:段级表按 order 升序,token 合计对得上。
    CHECK(facts.system_tokens == 500);
    REQUIRE(facts.segments.size() == 3);
    CHECK(facts.segments[0].order == 0);
    CHECK(facts.segments[0].segment_id == "core/10-identity.md");
    CHECK(facts.total_context_tokens == 500);
    CHECK(facts.budget_tokens == 0);
    // 证据不落正文,只落段名与 hash。
    const std::string dumped = override_finding->ToJson().dump();
    CHECK(dumped.find("\"SAME\"") == std::string::npos);
}

TEST_CASE("static:n-gram 高重合与预算占比") {
    StaticAuditInput input;
    const std::string base = LongText("read the file and edit the lines carefully", 12);
    const std::string near = LongText("read the file and edit the lines carefully again", 12);
    input.segment_texts["core/a.md"] = base;
    input.segment_texts["features/b.md"] = near;
    input.manifest.segments.push_back(Segment("core/a.md", base, 300, 0));
    input.manifest.segments.push_back(Segment("features/b.md", near, 300, 1));
    input.manifest.resolved_prompt_tokens_estimated = 600;
    input.context_budget_tokens = 1000;  // 600/1000 = 60% ≥ 50% 线

    PromptAuditFacts facts;
    const std::vector<Finding> findings = AuditPromptStatic(input, &facts);

    const Finding* overlap = FindByCategory(findings, "prompt.ngram_overlap");
    REQUIRE(overlap != nullptr);
    CHECK(overlap->finding_id == "P-AUD-S03");
    CHECK(overlap->severity == FindingSeverity::Warning);
    // 重合度是测量值;两段几乎同文,应报出较高百分比。
    const std::string dumped = overlap->ToJson().dump();
    CHECK(dumped.find("overlap_percent") != std::string::npos);
    // 正文绝不进 finding。
    CHECK(dumped.find("read the file") == std::string::npos);

    const Finding* share = FindByCategory(findings, "prompt.token_share");
    REQUIRE(share != nullptr);
    CHECK(share->finding_id == "P-AUD-S04");
    CHECK(share->evidence[2].value == 60);  // share_percent

    // 预算未知时占比规则不判:再来一份 budget=0 的输入。
    StaticAuditInput no_budget = input;
    no_budget.context_budget_tokens = 0;
    const std::vector<Finding> no_budget_findings = AuditPromptStatic(no_budget, nullptr);
    CHECK(FindByCategory(no_budget_findings, "prompt.token_share") == nullptr);
}

TEST_CASE("static:工具描述/schema/同名撞车/MCP 占比") {
    StaticAuditInput input;
    input.manifest.resolved_prompt_tokens_estimated = 1000;
    AuditToolDefinition bloated;
    bloated.name = "big_tool";
    bloated.description = LongText("do many things", 1400);  // ~4900 字节 → >900 token
    input.tools.push_back(bloated);

    AuditToolDefinition shapeless;
    shapeless.name = "shapeless";
    shapeless.description = "no schema";
    shapeless.input_schema = nlohmann::json{{"type", "string"}};
    input.tools.push_back(shapeless);

    AuditToolDefinition deep;
    deep.name = "deep_tool";
    deep.description = "deep";
    nlohmann::json level = nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};
    nlohmann::json nested = level;
    for (int i = 0; i < 8; ++i) {
        nested = nlohmann::json{{"type", "object"},
                                {"properties", nlohmann::json{{"inner", nested}}},
                                {"required", nlohmann::json::array({"inner"})}};
    }
    deep.input_schema = nested;
    input.tools.push_back(deep);

    AuditToolDefinition mcp_tool;
    mcp_tool.name = "mcp__srv__query";
    mcp_tool.description = LongText("query everything about the world", 200);
    mcp_tool.input_schema = nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};
    mcp_tool.source_kind = "mcp";
    input.tools.push_back(mcp_tool);

    const std::vector<Finding> findings = AuditPromptStatic(input, nullptr);
    const Finding* bloat = FindByCategory(findings, "tool.description_bloat");
    REQUIRE(bloat != nullptr);
    CHECK(bloat->evidence[0].value.dump().find("big_tool") != std::string::npos);

    const Finding* shapeless_finding = FindByCategory(findings, "tool.schema_shape");
    REQUIRE(shapeless_finding != nullptr);
    CHECK(shapeless_finding->severity == FindingSeverity::Warning);

    const Finding* depth_finding = FindByCategory(findings, "tool.schema_depth");
    REQUIRE(depth_finding != nullptr);
    CHECK(depth_finding->finding_id == "P-AUD-S07");

    // 工具定义 tokens 进事实账,MCP 权重过线报 info(是否调用明说看 runtime)。
    const Finding* mcp_weight = FindByCategory(findings, "tool.mcp_weight");
    REQUIRE(mcp_weight != nullptr);
    CHECK(mcp_weight->severity == FindingSeverity::Info);
    CHECK(mcp_weight->summary.find("runtime") != std::string::npos);

    // 同名撞车:单注册表按构造不该有;显式喂重复名要能报。
    StaticAuditInput collision_input;
    collision_input.manifest.resolved_prompt_tokens_estimated = 1000;
    AuditToolDefinition first;
    first.name = "clash";
    first.description = "one";
    first.source_kind = "builtin";
    AuditToolDefinition second;
    second.name = "clash";
    second.description = "two";
    second.source_kind = "mcp";
    collision_input.tools.push_back(first);
    collision_input.tools.push_back(second);
    const std::vector<Finding> collision_findings = AuditPromptStatic(collision_input, nullptr);
    const Finding* collision = FindByCategory(collision_findings, "tool.name_collision");
    REQUIRE(collision != nullptr);
    CHECK(collision->finding_id == "P-AUD-S08");
}

TEST_CASE("static:动态段插中间与疑似冲突(语义类只给 suspected)") {
    StaticAuditInput input;
    input.manifest.segments.push_back(Segment("core/a.md", "stable-a", 100, 0));
    input.manifest.segments.push_back(Segment("runtime/environment", "volatile", 50, 1, true));
    input.manifest.segments.push_back(Segment("core/b.md", "stable-b", 100, 2));
    input.manifest.resolved_prompt_tokens_estimated = 250;
    input.segment_texts["core/rules.md"] =
        LongText("你必须先问用户才能动手。务必先问。", 40);
    input.segment_texts["core/hurry.md"] =
        LongText("无关背景说明若干。", 40);

    const std::vector<Finding> findings = AuditPromptStatic(input, nullptr);
    const Finding* volatile_mid = FindByCategory(findings, "prompt.volatile_mid_prefix");
    REQUIRE(volatile_mid != nullptr);
    CHECK(volatile_mid->finding_id == "P-AUD-S11");
    CHECK(volatile_mid->severity == FindingSeverity::Warning);

    // 疑似冲突:一侧"必须先问"、另一侧没有"直接执行"时,不许单边报。
    CHECK(FindByCategory(findings, "instruction.absolute_conflict") == nullptr);

    // 两边都有对仗词:报 suspected(confidence=low)。
    StaticAuditInput conflicted = input;
    conflicted.segment_texts["core/hurry.md"] =
        LongText("直接执行即可,无需确认,自己判断。", 40);
    const std::vector<Finding> conflicted_findings = AuditPromptStatic(conflicted, nullptr);
    const Finding* conflict = FindByCategory(conflicted_findings, "instruction.absolute_conflict");
    REQUIRE(conflict != nullptr);
    CHECK(conflict->finding_id == "P-AUD-S09");
    CHECK(conflict->confidence == FindingConfidence::Low);
    CHECK(conflict->summary.find("疑似") != std::string::npos);
}

TEST_CASE("runtime:toolset 抖动/前缀断/缺 snapshot 与增长对 cache 的同向观察") {
    auto snapshot_with = [](const std::string& toolset, const std::string& prefix,
                            std::int64_t system_tokens) {
        agent::RequestSnapshotMetadata snapshot;
        snapshot.request_shape.toolset_hash = toolset;
        snapshot.prompt_manifest.stable_prefix_hash = prefix;
        snapshot.prompt_manifest.resolved_prompt_tokens_estimated = system_tokens;
        snapshot.prompt_manifest.segments.push_back(
            Segment("core/a.md", "stable-" + prefix, system_tokens, 0));
        snapshot.request_shape.tool_count = 10;
        snapshot.request_shape.tool_definition_tokens_estimated = 800;
        return snapshot;
    };
    RuntimeAuditInput input;
    input.session_id = "20260831-000001-PA0001";
    std::vector<RuntimeRequestView> views;
    {
        RuntimeRequestView v1;
        v1.run_id = "main-0001";
        v1.request_id = "req-1";
        v1.event_id = "main-0001:evt-00000101";
        v1.purpose = "main_turn";
        v1.snapshot = snapshot_with("th-1", "sp-1", 1000);
        v1.usage_reported = true;
        v1.total_input_tokens = 1000;
        v1.cache_read_tokens = 950;
        v1.cache_epoch = 3;
        views.push_back(v1);
        RuntimeRequestView v2 = v1;
        v2.request_id = "req-2";
        v2.event_id = "main-0001:evt-00000102";
        v2.snapshot = snapshot_with("th-2", "sp-1", 1200);
        v2.cache_read_tokens = 900;
        views.push_back(v2);
        RuntimeRequestView v3 = v1;
        v3.request_id = "req-3";
        v3.event_id = "main-0001:evt-00000103";
        v3.snapshot = snapshot_with("th-3", "sp-2", 1500);
        v3.total_input_tokens = 1500;
        v3.cache_read_tokens = 100;
        views.push_back(v3);
        RuntimeRequestView v3b = v1;
        v3b.request_id = "req-3b";
        v3b.event_id = "main-0001:evt-00000104";
        v3b.snapshot = snapshot_with("th-3", "sp-3", 1600);
        v3b.total_input_tokens = 1600;
        v3b.cache_read_tokens = 90;
        views.push_back(v3b);
        RuntimeRequestView v4;
        v4.run_id = "main-0001";
        v4.request_id = "req-4";
        v4.event_id = "main-0001:evt-00000104";
        v4.purpose = "main_turn";  // 无 snapshot:旧账
        v4.usage_reported = false;
        views.push_back(v4);
    }
    input.requests = views;

    const std::vector<Finding> findings = AuditPromptRuntime(input);
    const Finding* churn = FindByCategory(findings, "cache.toolset_churn");
    REQUIRE(churn != nullptr);
    CHECK(churn->finding_id == "P-AUD-R02");
    CHECK(churn->severity == FindingSeverity::Warning);
    // 证据回引 prepared 事件。
    bool has_event_ref = false;
    for (const auto& item : churn->evidence) {
        if (item.event_id.has_value()) {
            has_event_ref = true;
        }
    }
    CHECK(has_event_ref);

    const Finding* prefix_break = FindByCategory(findings, "cache.prefix_churn");
    REQUIRE(prefix_break != nullptr);
    CHECK(prefix_break->finding_id == "P-AUD-R03");

    const Finding* missing = FindByCategory(findings, "prompt.snapshot_missing");
    REQUIRE(missing != nullptr);
    CHECK(missing->evidence[0].value.dump().find("req-4") != std::string::npos);

    // 同向观察:system 1000→1500(+50%),cache 95%→6%(跌 89 点)。
    const Finding* growth = FindByCategory(findings, "prompt.growth_cache_miss");
    REQUIRE(growth != nullptr);
    CHECK(growth->summary.find("同向观察") != std::string::npos);
    CHECK(!growth->counter_evidence.empty());  // TTL/波动反证在册

    // 变化账:summarize 单独可用(A4 复用同一只口)。
    const RuntimeChangeSummary summary = SummarizeRuntimeChanges(views);
    CHECK(summary.comparable == 3);
    CHECK(summary.toolset_changes == 2);
    CHECK(summary.prefix_changes == 2);
    CHECK(summary.prefix_breaks_same_epoch == 2);
}

TEST_CASE("runtime 读侧:真 Journal 装配 + 隐私红线(canary 不进输出)") {
    const auto dir = insights_fixtures::PrepareDir(
        std::filesystem::temp_directory_path() / "lubancode-a3-runtime");
    const insights_fixtures::FixedClock clock;
    insights_fixtures::FixtureStream stream(dir / "main.jsonl", dir / "artifacts",
                                            kWorkspaceKey, "20260831-000001-PA0002",
                                            "main-0001", trajectory::RunKind::MainSession, 2,
                                            clock);
    stream.StartRun();
    stream.StartTurn("turn-0001");
    agent::RequestSnapshotMetadata snapshot;
    snapshot.request_shape.toolset_hash = "th-a";
    snapshot.prompt_manifest.stable_prefix_hash = "sp-a";
    snapshot.prompt_manifest.resolved_prompt_tokens_estimated = 800;
    const std::string canary_secret = "sk-CANARY-a3f0ffee";
    const std::string canary_text =
        LongText("阅读源码前先读这个模块 sk-CANARY-a3f0ffee 不要外传", 20);
    snapshot.prompt_manifest.segments.push_back(Segment("core/secret-carrier.md", canary_text, 300, 0));
    snapshot.prompt_manifest.segments.push_back(Segment("core/second.md", "b", 100, 1));
    nlohmann::json snapshot_json = snapshot.ToJson();
    // canary 直接埋进 manifest 元数据域:读侧解出来也不许流进任何输出。
    snapshot_json["prompt_manifest"]["segments"][0]["segment_id"] = "core/secret-carrier.md";
    insights_fixtures::UsageSpec usage;
    usage.input = 500;
    usage.cache_read = 3000;
    usage.output = 100;
    usage.cache_epoch = 2;
    stream.ModelExchange("turn-0001", "req-0001", "main_turn", usage, false, "", 1, false, false,
                         snapshot_json);
    stream.EndTurn("turn-0001");
    stream.Seal();

    const RuntimeRequestsRead read = CollectRuntimeRequests(dir);
    REQUIRE(read.ok);
    REQUIRE(read.requests.size() == 1);
    CHECK(read.requests[0].purpose == "main_turn");
    REQUIRE(read.requests[0].snapshot.has_value());
    CHECK(read.requests[0].snapshot->request_shape.toolset_hash == "th-a");
    CHECK(read.requests[0].usage_reported);
    CHECK(read.requests[0].total_input_tokens == 3500);
    CHECK(read.requests[0].cache_epoch == std::optional<int>(2));

    // 隐私红线:finding 与报告面 JSON 都不许带 canary。
    RuntimeAuditInput audit_input;
    audit_input.session_id = read.session_id;
    audit_input.requests = read.requests;
    const std::vector<Finding> findings = AuditPromptRuntime(audit_input);
    for (const auto& finding : findings) {
        CHECK(finding.ToJson().dump().find(canary_secret) == std::string::npos);
    }
    PromptAuditFacts facts;
    StaticAuditInput static_input;
    static_input.manifest = read.requests[0].snapshot->prompt_manifest;
    static_input.segment_texts["core/secret-carrier.md"] = canary_text;
    const std::vector<Finding> static_findings = AuditPromptStatic(static_input, &facts);
    CHECK(facts.ToJson().dump().find(canary_secret) == std::string::npos);
    for (const auto& finding : static_findings) {
        CHECK(finding.ToJson().dump().find("不要外传") == std::string::npos);
    }

    // 目录不存在:ok=false,不产残账。
    const RuntimeRequestsRead missing =
        CollectRuntimeRequests(std::filesystem::temp_directory_path() / "lubancode-a3-none");
    CHECK(!missing.ok);
    CHECK(missing.error_code == "prompt.session_not_found");
    CHECK(missing.requests.empty());
}
