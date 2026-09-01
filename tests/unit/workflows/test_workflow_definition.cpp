// Workflows 单第 1 批:定义/解析/归一化/hash/catalog/校验/图渲染。
//
// 纯逻辑直测,不碰网络;盘上用例走临时目录。fixture 的坏 YAML 全在
// 用例里现写现删,不往 tests/fixtures 塞(它们是字符串断言,不是复跑资产)。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <sstream>

#include "workflow/catalog.hpp"
#include "workflow/graph_view.hpp"
#include "workflow/parser.hpp"
#include "workflow/validator.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_workflow_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    const fs::path& Get() const { return dir_; }

private:
    fs::path dir_;
};

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << content;
}

// 单内草案那份论文检索图的精简版:四路并行 + all_settled + 汇总。
const char* kPaperYaml = R"YAML(
schema_version: 1
id: paper-research
version: 1.0.0
name: 论文检索
description: 多源检索、去重、排序并写成 Markdown
alias: 论文检索
scope: project

entry: parse_query
limits:
  max_concurrency: 4
  max_nodes: 64
  max_steps: 128
  timeout: 10m
  tool_calls: 100
  tokens: 120000

nodes:
  parse_query:
    type: llm
    prompt: prompts/parse-query.md
    input: { query: "${inputs.topic}" }
  search_sources:
    type: parallel
    branches: [arxiv, dblp]
    join: all_settled
    max_concurrency: 4
  arxiv:
    type: tool
    tool: plugin__papers__arxiv_search
    input: { query: "${nodes.parse_query.output.query}" }
    retry: { attempts: 3, backoff: exponential, initial: 1s, max: 15s }
    on_unavailable: skip
  dblp:
    type: tool
    tool: plugin__papers__dblp_search
    input: { query: "${nodes.parse_query.output.query}" }
    on_unavailable: skip
  dedupe:
    type: transform
    operation: paper_deduplicate
    input: "${nodes.search_sources.outputs}"
  markdown:
    type: template
    template: prompts/report.md
    input: "${nodes.dedupe.output}"
  end_report:
    type: end

edges:
  - { from: parse_query, on: success, to: search_sources }
  - { from: search_sources, on: joined, to: dedupe }
  - { from: dedupe, on: success, to: markdown }
  - { from: markdown, on: success, to: end_report }

result:
  markdown: "${nodes.markdown.output}"
)YAML";

}  // namespace

TEST_CASE("ParseWorkflowYaml: 论文检索草案解析成 AST") {
    const auto parsed = lubancode::workflow::ParseWorkflowYaml(kPaperYaml);
    REQUIRE(parsed.has_value());
    const auto& def = *parsed;

    CHECK(def.id == "paper-research");
    CHECK(def.version == "1.0.0");
    CHECK(def.alias == "论文检索");
    CHECK(def.entry == "parse_query");
    CHECK(def.limits.timeout_secs == 600);  // 10m
    CHECK(def.limits.max_concurrency == 4);
    REQUIRE(def.nodes.size() == 7);

    const auto& parallel = def.node_map.at("search_sources");
    CHECK(parallel.kind == lubancode::workflow::NodeKind::Parallel);
    CHECK(parallel.join == lubancode::workflow::JoinPolicy::AllSettled);
    REQUIRE(parallel.branches.size() == 2);
    CHECK(parallel.branches[0] == "arxiv");

    const auto& arxiv = def.node_map.at("arxiv");
    CHECK(arxiv.kind == lubancode::workflow::NodeKind::Tool);
    CHECK(arxiv.tool == "plugin__papers__arxiv_search");
    REQUIRE(arxiv.retry.has_value());
    CHECK(arxiv.retry->attempts == 3);
    CHECK(arxiv.retry->backoff == lubancode::workflow::BackoffKind::Exponential);
    CHECK(arxiv.retry->initial_ms == 1000);
    CHECK(arxiv.retry->max_ms == 15000);
    CHECK(arxiv.on_unavailable == lubancode::workflow::OnUnavailable::Skip);

    CHECK(def.node_map.at("dedupe").operation == "paper_deduplicate");
    CHECK(def.node_map.at("markdown").template_path == "prompts/report.md");
    CHECK(def.edges.size() == 4);
    CHECK(def.edges[1].outcome == "joined");
}

TEST_CASE("ParseWorkflowYaml: 认不得的 schema_version 拒跑") {
    const auto parsed = lubancode::workflow::ParseWorkflowYaml(
        "schema_version: 99\nid: x\nversion: 1.0.0\nentry: a\nnodes:\n  a:\n    type: end\n");
    REQUIRE(!parsed.has_value());
    REQUIRE(parsed.error().size() == 1);
    CHECK(parsed.error()[0].message.find("不认") != std::string::npos);
}

TEST_CASE("ParseWorkflowYaml: 坏 YAML、越界路径、坏时长各报各的位置") {
    SUBCASE("坏缩进") {
        const auto parsed = lubancode::workflow::ParseWorkflowYaml("id: [\n  broken");
        REQUIRE(!parsed.has_value());
    }
    SUBCASE("prompt 越界") {
        const auto parsed = lubancode::workflow::ParseWorkflowYaml(
            "schema_version: 1\nid: a\nversion: 1.0.0\nentry: x\nnodes:\n"
            "  x:\n    type: llm\n    prompt: ../outside.md\n");
        REQUIRE(!parsed.has_value());
        bool found = false;
        for (const auto& issue : parsed.error()) {
            if (issue.message.find("越界") != std::string::npos) found = true;
        }
        CHECK(found);
    }
    SUBCASE("坏时长") {
        const auto parsed = lubancode::workflow::ParseWorkflowYaml(
            "schema_version: 1\nid: a\nversion: 1.0.0\nentry: x\nlimits:\n  timeout: 10x\nnodes:\n"
            "  x:\n    type: end\n");
        REQUIRE(!parsed.has_value());
    }
}

TEST_CASE("YAML round-trip: Emit 后再解析,内容 hash 不变") {
    const auto parsed = lubancode::workflow::ParseWorkflowYaml(kPaperYaml);
    REQUIRE(parsed.has_value());
    const std::string emitted = lubancode::workflow::EmitWorkflowYaml(*parsed);
    const auto reparsed = lubancode::workflow::ParseWorkflowYaml(emitted);
    if (!reparsed.has_value()) {
        for (const auto& issue : reparsed.error()) {
            MESSAGE("reparse issue: ", issue.location, ": ", issue.message);
        }
    }
    REQUIRE(reparsed.has_value());
    CHECK(lubancode::workflow::ContentHash(*parsed) == lubancode::workflow::ContentHash(*reparsed));
    // 归一化 JSON 逐字节一致(三平台一致性的锚)。
    CHECK(parsed->normalized.dump() == reparsed->normalized.dump());
}

TEST_CASE("JSON round-trip: ToJson/FromJson 保真") {
    const auto parsed = lubancode::workflow::ParseWorkflowYaml(kPaperYaml);
    REQUIRE(parsed.has_value());
    const auto restored = lubancode::workflow::WorkflowDefinition::FromJson(parsed->ToJson());
    CHECK(restored.id == parsed->id);
    CHECK(restored.nodes.size() == parsed->nodes.size());
    CHECK(restored.edges == parsed->edges);
    CHECK(lubancode::workflow::ContentHash(restored) == lubancode::workflow::ContentHash(*parsed));
}

TEST_CASE("agent 节点 turn_limit(任务总 turn 帽):解析、往返保真、与 step_limit 同现明拒") {
    const char* yaml = R"YAML(
schema_version: 1
id: turn-cap-flow
version: 1.0.0
entry: probe
nodes:
  probe:
    type: agent
    task: prompts/probe.md
    turn_limit: 12
)YAML";
    const auto parsed = lubancode::workflow::ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->nodes.size() == 1);
    CHECK(parsed->nodes[0].turn_limit == 12);
    CHECK(parsed->nodes[0].step_limit == 0);

    // YAML 往返与 JSON 往返都保真。
    const auto reparsed = lubancode::workflow::ParseWorkflowYaml(lubancode::workflow::EmitWorkflowYaml(*parsed));
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->nodes[0].turn_limit == 12);
    const auto restored = lubancode::workflow::WorkflowDefinition::FromJson(parsed->ToJson());
    CHECK(restored.nodes[0].turn_limit == 12);

    // 新旧限制同现:解析失败,要求作者删掉一枚(turn 预算单 §4.3——
    // 两者作用域不同,静默择一会猜错)。
    const char* conflicting = R"YAML(
schema_version: 1
id: turn-cap-conflict
version: 1.0.0
entry: probe
nodes:
  probe:
    type: agent
    task: prompts/probe.md
    step_limit: 8
    turn_limit: 12
)YAML";
    const auto rejected = lubancode::workflow::ParseWorkflowYaml(conflicting);
    REQUIRE_FALSE(rejected.has_value());
    bool saw_conflict = false;
    for (const auto& issue : rejected.error()) {
        if (issue.message.find("不能同现") != std::string::npos) {
            saw_conflict = true;
        }
    }
    CHECK(saw_conflict);
}

TEST_CASE("loop:解析、归一化与有帽校验") {
    const char* yaml = R"YAML(
schema_version: 1
id: review-loop
version: 1.0.0
entry: review
nodes:
  review:
    type: loop
    body: [draft, inspect]
    until: { op: equals, path: "${nodes.inspect.output.approved}", value: true }
    min_iterations: 1
    max_iterations: "${inputs.review_limit}"
    hard_limit: 12
  draft:
    type: transform
    operation: echo
    input: { previous: "${nodes.review.output.previous}" }
  inspect:
    type: transform
    operation: echo
    input: { draft: "${nodes.draft.output}" }
  fin:
    type: end
  exhausted:
    type: end
edges:
  - { from: review, on: success, to: fin }
  - { from: review, on: exhausted, to: exhausted }
)YAML";
    const auto parsed = lubancode::workflow::ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    const auto& loop = parsed->node_map.at("review");
    CHECK(loop.kind == lubancode::workflow::NodeKind::Loop);
    CHECK(loop.loop_body == std::vector<std::string>{"draft", "inspect"});
    REQUIRE(loop.loop_until.has_value());
    CHECK(loop.loop_until->op == lubancode::workflow::ConditionOp::Equals);
    CHECK(loop.loop_max_iterations == "${inputs.review_limit}");
    CHECK(loop.loop_hard_limit == 12);
    CHECK(lubancode::workflow::ValidateDefinition(*parsed, std::nullopt).ok());

    const std::string emitted = lubancode::workflow::EmitWorkflowYaml(*parsed);
    const auto reparsed = lubancode::workflow::ParseWorkflowYaml(emitted);
    REQUIRE(reparsed.has_value());
    CHECK(lubancode::workflow::ContentHash(*parsed) == lubancode::workflow::ContentHash(*reparsed));
    const auto restored = lubancode::workflow::WorkflowDefinition::FromJson(parsed->ToJson());
    CHECK(restored.node_map.at("review") == loop);
}

TEST_CASE("loop:无 exhausted 出边、越硬帽与普通回边都拒绝") {
    const auto has_issue = [](const lubancode::workflow::ValidationResult& result, const std::string& code) {
        return std::any_of(result.issues.begin(), result.issues.end(), [&](const auto& issue) {
            return issue.code == code;
        });
    };
    const char* no_exhausted = R"YAML(
schema_version: 1
id: bad-loop
version: 1
entry: loop
nodes:
  loop:
    type: loop
    body: [work]
    until: { op: equals, path: "${nodes.work.output.done}", value: true }
    max_iterations: 9
    hard_limit: 3
  work: { type: transform, operation: echo }
  fin: { type: end }
edges:
  - { from: loop, on: success, to: fin }
)YAML";
    const auto parsed = lubancode::workflow::ParseWorkflowYaml(no_exhausted);
    REQUIRE(parsed.has_value());
    const auto result = lubancode::workflow::ValidateDefinition(*parsed, std::nullopt);
    CHECK(has_issue(result, "missing_loop_exhausted_edge"));
    CHECK(has_issue(result, "loop_limit_exceeds_hard_limit"));

    const auto cycle = lubancode::workflow::ParseWorkflowYaml(
        "schema_version: 1\nid: cycle\nversion: 1\nentry: x\nnodes:\n"
        "  x: { type: transform, operation: echo }\n"
        "  y: { type: transform, operation: echo }\n"
        "edges:\n  - { from: x, on: success, to: y }\n  - { from: y, on: success, to: x }\n");
    REQUIRE(cycle.has_value());
    CHECK(has_issue(lubancode::workflow::ValidateDefinition(*cycle, std::nullopt), "cycle_detected"));
}

TEST_CASE("示例 workflow:三省六部两层宪制——官制归数据") {
    const fs::path example = fs::path(__FILE__).parent_path() / "../../../examples/workflows/sansheng-liubu/workflow.yaml";
    const auto parsed = lubancode::workflow::LoadWorkflowDefinition(example.lexically_normal());
    if (!parsed.has_value()) {
        for (const auto& issue : parsed.error()) MESSAGE(issue.location, ": ", issue.message);
    }
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->inputs["properties"]["requirement"].is_object());
    CHECK(parsed->inputs["properties"]["requirement"]["description"] == "皇上，您有什么需求？");
    // 行政层是数据:几案并陈、几部分牒都是带默认值的数组。
    REQUIRE(parsed->inputs["properties"]["lanes"]["default"].is_array());
    CHECK(parsed->inputs["properties"]["lanes"]["default"].size() == 3);
    REQUIRE(parsed->inputs["properties"]["ministries"]["default"].is_array());
    CHECK(parsed->inputs["properties"]["ministries"]["default"].size() == 3);

    // 宪法层:entry 是 moulue,map 的 items 引 inputs.lanes,body 是 mouyi。
    CHECK(parsed->entry == "moulue");
    REQUIRE(parsed->node_map.contains("moulue"));
    CHECK(parsed->node_map.at("moulue").kind == lubancode::workflow::NodeKind::Map);
    CHECK(parsed->node_map.at("moulue").items_ref == "${inputs.lanes}");
    CHECK(parsed->node_map.at("moulue").map_body == "mouyi");
    REQUIRE(parsed->node_map.contains("mouyi"));
    CHECK(parsed->node_map.at("mouyi").kind == lubancode::workflow::NodeKind::Llm);
    CHECK(parsed->node_map.at("mouyi").model_role == "lao");

    // 封驳 loop:body 恰为拟诏、封驳、画敕三节点,until 读御批 complete。
    REQUIRE(parsed->node_map.contains("fengbo"));
    CHECK(parsed->node_map.at("fengbo").kind == lubancode::workflow::NodeKind::Loop);
    const auto& loop_body = parsed->node_map.at("fengbo").loop_body;
    REQUIRE(loop_body.size() == 3);
    CHECK(loop_body[0] == "zhongshu");
    CHECK(loop_body[1] == "menxia");
    CHECK(loop_body[2] == "chengzhi");
    REQUIRE(parsed->node_map.at("fengbo").loop_until.has_value());
    CHECK(parsed->node_map.at("fengbo").loop_until->path == "${nodes.chengzhi.output.complete}");

    // 门下不见用户原话;御前画敕带墨敕一档。
    REQUIRE(parsed->node_map.contains("menxia"));
    CHECK_FALSE(parsed->node_map.at("menxia").input.contains("requirement"));
    REQUIRE(parsed->node_map.contains("chengzhi"));
    REQUIRE(parsed->node_map.at("chengzhi").input.contains("override_answers"));
    CHECK(parsed->node_map.at("chengzhi").input["override_answers"].is_array());

    // 尚书照路由表分牒,定牒誊单,发牌 foreach 按单序串行(验-修-验有依赖,
    // 并发 map 会让基线被修复动作污染——0.26.85 真机实翻)。
    REQUIRE(parsed->node_map.contains("shangshu"));
    CHECK(parsed->node_map.at("shangshu").kind == lubancode::workflow::NodeKind::Llm);
    CHECK(parsed->node_map.at("shangshu").input.contains("edict"));
    CHECK(parsed->node_map.at("shangshu").input.contains("ministries"));
    REQUIRE(parsed->node_map.contains("fapai"));
    CHECK(parsed->node_map.at("fapai").kind == lubancode::workflow::NodeKind::Foreach);
    CHECK(parsed->node_map.at("fapai").items_ref == "${nodes.dingdie.output.dispatches}");
    CHECK(parsed->node_map.at("fapai").map_body == "banshi");

    // 密封差遣:banshi 是 agent,input 无自有字段,只吃引擎注入的 item。
    REQUIRE(parsed->node_map.contains("banshi"));
    CHECK(parsed->node_map.at("banshi").kind == lubancode::workflow::NodeKind::Agent);
    REQUIRE(parsed->node_map.at("banshi").input.is_object());
    CHECK(parsed->node_map.at("banshi").input.empty());

    // 行政链各 success 边都在。
    const auto has_edge = [&](const std::string& from, const std::string& to) {
        return std::ranges::any_of(parsed->edges, [&](const auto& edge) {
            return edge.from == from && edge.outcome == "success" && edge.to == to;
        });
    };
    CHECK(has_edge("moulue", "fengbo"));
    CHECK(has_edge("fengbo", "shangshu"));
    CHECK(has_edge("shangshu", "jiaqian"));
    CHECK(has_edge("jiaqian", "dingdie"));
    CHECK(has_edge("dingdie", "fapai"));
    CHECK(has_edge("fapai", "fuming"));
    CHECK(has_edge("fuming", "shouwei"));
    CHECK(std::ranges::any_of(parsed->edges, [](const auto& edge) {
        return edge.from == "fengbo" && edge.outcome == "exhausted" && edge.to == "weijue";
    }));

    const auto validation = lubancode::workflow::ValidateDefinition(*parsed, std::nullopt);
    for (const auto& issue : validation.issues) MESSAGE(issue.code, " ", issue.path, ": ", issue.message);
    CHECK(validation.ok());
}

TEST_CASE("IsSafePackageRelative 与时长解析") {
    using lubancode::workflow::IsSafePackageRelative;
    CHECK(IsSafePackageRelative("prompts/parse-query.md"));
    CHECK(IsSafePackageRelative("fixtures/smoke.json"));
    CHECK_FALSE(IsSafePackageRelative("../outside.md"));
    CHECK_FALSE(IsSafePackageRelative("a/../../b.md"));
    CHECK_FALSE(IsSafePackageRelative("/abs.md"));
    CHECK_FALSE(IsSafePackageRelative("C:/x.md"));
    CHECK_FALSE(IsSafePackageRelative("a\\b.md"));
    CHECK_FALSE(IsSafePackageRelative(""));

    CHECK(*lubancode::workflow::ParseDurationSecs("90s") == 90);
    CHECK(*lubancode::workflow::ParseDurationSecs("10m") == 600);
    CHECK(*lubancode::workflow::ParseDurationSecs("1h") == 3600);
    CHECK(*lubancode::workflow::ParseDurationSecs("45") == 45);
    CHECK_FALSE(lubancode::workflow::ParseDurationSecs("10x").has_value());
    CHECK_FALSE(lubancode::workflow::ParseDurationSecs("").has_value());
}

TEST_CASE("Catalog: 项目级遮用户级、broken 标坏、撞名禁用") {
    TempDir tmp;
    const fs::path project = tmp.Get() / "proj";
    const fs::path home = tmp.Get() / "home";
    WriteFile(project / ".lubancode" / "workflows" / "paper-research" / "workflow.yaml", kPaperYaml);
    // 用户级同 id:被遮,但要报来源。
    std::string user_yaml = kPaperYaml;
    const std::string user_yaml_text = std::string(kPaperYaml);
    WriteFile(home / ".lubancode" / "workflows" / "paper-research" / "workflow.yaml", user_yaml_text);
    // 用户级另一份,alias 撞 skill "record"。
    WriteFile(home / ".lubancode" / "workflows" / "other-flow" / "workflow.yaml",
              "schema_version: 1\nid: other-flow\nversion: 1.0.0\nname: other\nalias: record\nentry: only\n"
              "nodes:\n  only:\n    type: end\n");
    // 坏定义。
    WriteFile(home / ".lubancode" / "workflows" / "broken-one" / "workflow.yaml", "id: [");

    const auto catalog = lubancode::workflow::LoadCatalog(project, home);
    REQUIRE(catalog.entries.size() == 3);
    // 找 paper-research 拿到的是项目级那份。
    const auto* paper = catalog.Find("paper-research");
    REQUIRE(paper != nullptr);
    CHECK(paper->scope == lubancode::workflow::WorkflowScope::Project);
    // 被遮的冲突要露脸。
    bool shadowed = false;
    for (const auto& conflict : catalog.conflicts) {
        if (conflict.kind == "shadowed") shadowed = true;
    }
    CHECK(shadowed);
    // broken 条目在列。
    bool has_broken = false;
    for (const auto& entry : catalog.entries) {
        if (entry.definition.id == "broken-one" && entry.broken) has_broken = true;
    }
    CHECK(has_broken);

    // 撞名检查:alias "record" 撞 skill -> 禁用直呼。
    lubancode::workflow::Catalog with_conflicts = catalog;
    lubancode::workflow::DetectAliasConflicts(with_conflicts, {"record"}, {"/workflow", "/help"});
    CHECK(with_conflicts.disabled_aliases.count("论文检索") == 0);
    CHECK(with_conflicts.disabled_aliases.count("record") == 1);
    CHECK(with_conflicts.FindByAlias("record") == nullptr);
    CHECK(with_conflicts.FindByAlias("论文检索") != nullptr);
}

TEST_CASE("Validator: 悬空边、entry 缺失、outcome 歧义、无界环、缺 Tool") {
    using lubancode::workflow::ValidationIssue;
    const auto make = [](const char* yaml) {
        return lubancode::workflow::ParseWorkflowYaml(yaml);
    };

    SUBCASE("悬空边") {
        const auto parsed = make("schema_version: 1\nid: a\nversion: 1\nentry: x\nnodes:\n"
                                 "  x:\n    type: end\nedges:\n  - {from: x, on: success, to: ghost}\n");
        REQUIRE(parsed.has_value());
        const auto result = lubancode::workflow::ValidateDefinition(*parsed, std::nullopt);
        REQUIRE_FALSE(result.ok());
        CHECK(result.issues[0].code == "dangling_edge");
    }

    SUBCASE("entry 不存在") {
        const auto parsed = make("schema_version: 1\nid: a\nversion: 1\nentry: nope\nnodes:\n"
                                 "  x:\n    type: end\n");
        REQUIRE(parsed.has_value());
        const auto result = lubancode::workflow::ValidateDefinition(*parsed, std::nullopt);
        REQUIRE_FALSE(result.ok());
        CHECK(result.issues[0].code == "unknown_entry");
    }

    SUBCASE("同 outcome 两条边") {
        const auto parsed = make(
            "schema_version: 1\nid: a\nversion: 1\nentry: x\nnodes:\n"
            "  x:\n    type: llm\n    prompt: p.md\n  y:\n    type: end\n  z:\n    type: end\n"
            "edges:\n  - {from: x, on: success, to: y}\n  - {from: x, on: success, to: z}\n");
        REQUIRE(parsed.has_value());
        const auto result = lubancode::workflow::ValidateDefinition(*parsed, std::nullopt);
        bool found = false;
        for (const auto& issue : result.issues) {
            if (issue.code == "ambiguous_outcome") found = true;
        }
        CHECK(found);
    }

    SUBCASE("无界环(max_steps 罩底时放行,摘帽时报)") {
        const char* loop = "schema_version: 1\nid: a\nversion: 1\nentry: x\nnodes:\n"
                           "  x:\n    type: llm\n    prompt: p.md\n  y:\n    type: llm\n    prompt: q.md\n"
                           "edges:\n  - {from: x, on: success, to: y}\n  - {from: y, on: success, to: x}\n";
        auto parsed = make(loop);
        REQUIRE(parsed.has_value());
        // 默认 max_steps=128 罩底 -> 环有上限,不报 unbounded_loop。
        auto capped = lubancode::workflow::ValidateDefinition(*parsed, std::nullopt);
        bool unbounded = false;
        for (const auto& issue : capped.issues) {
            if (issue.code == "unbounded_loop") unbounded = true;
        }
        CHECK_FALSE(unbounded);
        // 摘帽 -> 报。
        auto naked = *parsed;
        naked.limits.max_steps = 0;
        const auto flagged = lubancode::workflow::ValidateDefinition(naked, std::nullopt);
        unbounded = false;
        for (const auto& issue : flagged.issues) {
            if (issue.code == "unbounded_loop") unbounded = true;
        }
        CHECK(unbounded);
    }

    SUBCASE("缺 Tool(能力表点名)") {
        const auto parsed = make("schema_version: 1\nid: a\nversion: 1\nentry: x\nnodes:\n"
                                 "  x:\n    type: tool\n    tool: no_such_tool\n");
        REQUIRE(parsed.has_value());
        lubancode::workflow::CapabilityTable caps;
        caps.tools = {"read_file"};
        const auto result = lubancode::workflow::ValidateDefinition(*parsed, caps);
        REQUIRE_FALSE(result.ok());
        CHECK(result.issues[0].code == "unknown_tool");
        CHECK(result.issues[0].path == "nodes.x.tool");
    }

    SUBCASE("引用未来节点") {
        const auto parsed = make(
            "schema_version: 1\nid: a\nversion: 1\nentry: x\nnodes:\n"
            "  x:\n    type: llm\n    prompt: p.md\n    input: { q: \"${nodes.y.output.a}\" }\n"
            "  y:\n    type: llm\n    prompt: q.md\n"
            "edges:\n  - {from: x, on: success, to: y}\n");
        REQUIRE(parsed.has_value());
        const auto result = lubancode::workflow::ValidateDefinition(*parsed, std::nullopt);
        bool found = false;
        for (const auto& issue : result.issues) {
            if (issue.code == "forward_ref") found = true;
        }
        CHECK(found);
    }

    SUBCASE("副作用无幂等键不许重试") {
        const auto parsed = make("schema_version: 1\nid: a\nversion: 1\nentry: x\nnodes:\n"
                                 "  x:\n    type: tool\n    tool: t\n    side_effects: true\n"
                                 "    retry: { attempts: 3 }\n");
        REQUIRE(parsed.has_value());
        const auto result = lubancode::workflow::ValidateDefinition(*parsed, std::nullopt);
        bool found = false;
        for (const auto& issue : result.issues) {
            if (issue.code == "unsafe_retry") found = true;
        }
        CHECK(found);
    }

    SUBCASE("明文 secret 入定义被拒") {
        const auto parsed = make("schema_version: 1\nid: a\nversion: 1\nentry: x\nnodes:\n"
                                 "  x:\n    type: tool\n    tool: t\n    input: { api_key: sk-123 }\n");
        REQUIRE(parsed.has_value());
        const auto result = lubancode::workflow::ValidateDefinition(*parsed, std::nullopt);
        bool found = false;
        for (const auto& issue : result.issues) {
            if (issue.code == "plaintext_secret") found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("Validator: 论文检索草案过纯结构校验") {
    const auto parsed = lubancode::workflow::ParseWorkflowYaml(kPaperYaml);
    REQUIRE(parsed.has_value());
    const auto result = lubancode::workflow::ValidateDefinition(*parsed, std::nullopt);
    for (const auto& issue : result.issues) {
        MESSAGE("unexpected issue: ", issue.code, " ", issue.path, " ", issue.message);
    }
    CHECK(result.ok());
}

TEST_CASE("图渲染: ASCII 树与 Mermaid 均由图数据生成") {
    const auto parsed = lubancode::workflow::ParseWorkflowYaml(kPaperYaml);
    REQUIRE(parsed.has_value());
    const std::string ascii = lubancode::workflow::RenderAsciiGraph(*parsed);
    CHECK(ascii.find("parse_query") != std::string::npos);
    CHECK(ascii.find("arxiv") != std::string::npos);
    CHECK(ascii.find("joined") != std::string::npos);

    const std::string mermaid = lubancode::workflow::RenderMermaidGraph(*parsed);
    CHECK(mermaid.find("flowchart TD") == 0);
    // success 边不带标签(省噪声),非 success 边带 outcome 标签。
    CHECK(mermaid.find("parse_query --> search_sources") != std::string::npos);
    CHECK(mermaid.find("search_sources -->|\"joined\"| dedupe") != std::string::npos);
    // 隐含控制流(parallel 的 branches)也要画出来。
    CHECK(mermaid.find("search_sources -->|\"branch\"| arxiv") != std::string::npos);
}

TEST_CASE("async 定义:YAML/JSON 往返,body 归外壳独占") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: async-roundtrip
version: 1.0.0
entry: wait
nodes:
  wait: { type: async, body: fetch }
  fetch: { type: tool, tool: check_inbox }
  fin: { type: end }
edges:
  - { from: wait, on: success, to: fin }
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    CHECK(parsed->node_map.at("wait").kind == NodeKind::Async);
    CHECK(parsed->node_map.at("wait").async_body == "fetch");
    const WorkflowDefinition restored = WorkflowDefinition::FromJson(parsed->ToJson());
    CHECK(restored.node_map.at("wait").async_body == "fetch");
    CHECK(ValidateDefinition(restored, std::nullopt).ok());

    WorkflowDefinition unsafe = restored;
    unsafe.edges.push_back({"fetch", "success", "fin"});
    unsafe.normalized = BuildNormalizedJson(unsafe);
    const auto validation = ValidateDefinition(unsafe, std::nullopt);
    CHECK(std::any_of(validation.issues.begin(), validation.issues.end(), [](const auto& issue) {
        return issue.code == "control_body_edge";
    }));
}

TEST_CASE("alias/id 合法性") {
    using lubancode::workflow::IsValidAlias;
    CHECK(IsValidAlias("论文检索"));
    CHECK(IsValidAlias("paper-search"));
    CHECK(IsValidAlias("build_2"));
    CHECK_FALSE(IsValidAlias("has space"));
    CHECK_FALSE(IsValidAlias("a/b"));
    CHECK_FALSE(IsValidAlias("a\\b"));
    CHECK_FALSE(IsValidAlias(std::string("a") + '\x01'));

    using lubancode::workflow::IsValidWorkflowId;
    CHECK(IsValidWorkflowId("paper-research"));
    CHECK(IsValidWorkflowId("a"));
    CHECK_FALSE(IsValidWorkflowId("Paper"));
    CHECK_FALSE(IsValidWorkflowId("1abc"));
    CHECK_FALSE(IsValidWorkflowId("has_underscore"));
}
