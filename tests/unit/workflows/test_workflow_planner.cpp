// Workflows 单第 6 批:受约束 GraphPatch、风险评定、snapshot 与增量事件。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>

#include "workflow/frontend.hpp"
#include "workflow/journal.hpp"
#include "workflow/parser.hpp"
#include "workflow/planner.hpp"
#include "workflow/runtime.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_wf_planner_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

// 带模板授权的图:template 前缀的节点不进执行路径(entry 出发不可达),
// 但 planner 拿它当选积木。
const char* kPlannerYaml = R"YAML(
schema_version: 1
id: dynamic-flow
version: 1.0.0
name: dyn
entry: start
nodes:
  start:
    type: transform
    operation: echo
    input: { q: "${inputs.topic}" }
  fin:
    type: end
  plan_more_arxiv:
    label: "template:arxiv-dive"
    type: tool
    tool: arxiv_search
  plan_more_dedupe:
    label: "template:dedupe-step"
    type: transform
    operation: paper_deduplicate
edges:
  - { from: start, on: success, to: fin }
result:
  q: "${inputs.topic}"
)YAML";

}  // namespace

TEST_SUITE("workflows-planner") {

TEST_CASE("ExtractAllowance:只认 template: 前缀;没授权就关死") {
    using namespace lubancode::workflow;
    auto parsed = ParseWorkflowYaml(kPlannerYaml);
    REQUIRE(parsed.has_value());
    const PlannerAllowance allowance = ExtractAllowance(*parsed);
    REQUIRE(allowance.node_templates.size() == 2);
    REQUIRE(allowance.tool_allowlist.size() == 1);
    CHECK(allowance.tool_allowlist[0] == "arxiv_search");

    // 没有任何模板的图:planner 无米下锅。
    const auto plain = ParseWorkflowYaml("schema_version: 1\nid: p2\nversion: 1\nentry: x\nnodes:\n"
                                         "  x:\n    type: end\n");
    REQUIRE(plain.has_value());
    CHECK(ExtractAllowance(*plain).node_templates.empty());
}

TEST_CASE("ApplyGraphPatch:append-only,越权/悬边/改既有全拒") {
    using namespace lubancode::workflow;
    auto parsed = ParseWorkflowYaml(kPlannerYaml);
    REQUIRE(parsed.has_value());
    const PlannerAllowance allowance = ExtractAllowance(*parsed);
    CapabilityTable caps;
    caps.tools = {"arxiv_search"};
    caps.transforms = {"paper_deduplicate", "echo"};

    SUBCASE("合法补丁:模板内节点 + 边") {
        GraphPatch patch;
        WorkflowNode dive;
        dive.id = "arxiv_dive_1";
        dive.kind = NodeKind::Tool;
        dive.tool = "arxiv_search";
        patch.append_nodes.push_back(dive);
        patch.append_edges.push_back(WorkflowEdge{"start", "success", "arxiv_dive_1"});
        patch.append_edges.push_back(WorkflowEdge{"arxiv_dive_1", "success", "fin"});
        patch.attach_after = "start";
        auto applied = ApplyGraphPatch(*parsed, patch, allowance, caps);
        REQUIRE(applied.has_value());
        CHECK(applied->nodes.size() == parsed->nodes.size() + 1);
        // 补丁序列化 round-trip。
        GraphPatch round;
        round.append_nodes = patch.append_nodes;
        round.append_edges = patch.append_edges;
        round.attach_after = patch.attach_after;
        const std::string text = SerializeGraphPatch(round);
        const auto revived = ParseGraphPatch(text);
        REQUIRE(revived.has_value());
        CHECK(revived->append_nodes[0].id == "arxiv_dive_1");
    }
    SUBCASE("模板外的工具:拒") {
        GraphPatch patch;
        WorkflowNode rogue;
        rogue.id = "scholar_dive";
        rogue.kind = NodeKind::Tool;
        rogue.tool = "google_scholar_search";  // 不在 allowlist
        patch.append_nodes.push_back(rogue);
        patch.attach_after = "start";
        auto rejected = ApplyGraphPatch(*parsed, patch, allowance, caps);
        REQUIRE(!rejected.has_value());
        CHECK(rejected.error().code == "patch_forbidden_node");
    }
    SUBCASE("attach 指向未知节点:拒") {
        GraphPatch patch;
        patch.attach_after = "ghost";
        auto rejected = ApplyGraphPatch(*parsed, patch, allowance, caps);
        REQUIRE(!rejected.has_value());
        CHECK(rejected.error().code == "patch_unknown_attach");
    }
    SUBCASE("悬边:拒") {
        GraphPatch patch;
        WorkflowNode dive;
        dive.id = "arxiv_dive_2";
        dive.kind = NodeKind::Tool;
        dive.tool = "arxiv_search";
        patch.append_nodes.push_back(dive);
        patch.append_edges.push_back(WorkflowEdge{"start", "success", "not_in_patch"});
        patch.attach_after = "start";
        auto rejected = ApplyGraphPatch(*parsed, patch, allowance, caps);
        REQUIRE(!rejected.has_value());
        CHECK(rejected.error().code == "patch_dangling_edge");
    }
    SUBCASE("没有授权:全拒") {
        GraphPatch patch;
        patch.attach_after = "start";
        PlannerAllowance empty;
        auto rejected = ApplyGraphPatch(*parsed, patch, empty, caps);
        REQUIRE(!rejected.has_value());
        CHECK(rejected.error().code == "patch_no_allowance");
    }
}

TEST_CASE("ValidatePatchedDefinition:改/删既有节点报,节点数帽报") {
    using namespace lubancode::workflow;
    auto parsed = ParseWorkflowYaml(kPlannerYaml);
    REQUIRE(parsed.has_value());
    WorkflowDefinition patched = *parsed;

    SUBCASE("原样 = 过") {
        CHECK(ValidatePatchedDefinition(patched, *parsed).ok());
    }
    SUBCASE("改既有节点:报") {
        patched.node_map.at("start").operation = "something_else";
        const auto result = ValidatePatchedDefinition(patched, *parsed);
        REQUIRE_FALSE(result.ok());
        CHECK(result.issues[0].code == "patch_modified_node");
    }
    SUBCASE("删既有节点:报") {
        patched.node_map.erase("start");
        const auto result = ValidatePatchedDefinition(patched, *parsed);
        bool found = false;
        for (const auto& issue : result.issues) {
            if (issue.code == "patch_removed_node") found = true;
        }
        CHECK(found);
    }
    SUBCASE("节点数越帽:报") {
        patched.limits.max_nodes = 2;
        const auto result = ValidatePatchedDefinition(patched, *parsed);
        REQUIRE_FALSE(result.ok());
        CHECK(result.issues[0].code == "max_nodes");
    }
}

TEST_CASE("AssessPatchRisk:副作用/交互节点要再问用户") {
    using namespace lubancode::workflow;
    auto parsed = ParseWorkflowYaml(kPlannerYaml);
    REQUIRE(parsed.has_value());
    WorkflowDefinition patched = *parsed;
    WorkflowNode side;
    side.id = "commit_result";
    side.kind = NodeKind::Tool;
    side.tool = "arxiv_search";
    side.has_side_effects = true;
    patched.nodes.push_back(side);
    patched.node_map.emplace("commit_result", side);
    patched.normalized = BuildNormalizedJson(patched);

    const PatchRisk risk = AssessPatchRisk(patched, *parsed);
    CHECK(risk.adds_side_effects);
    REQUIRE_FALSE(risk.reasons.empty());

    // 无副作用的新节点:不必问。
    WorkflowDefinition tame = *parsed;
    WorkflowNode extra;
    extra.id = "more_dedupe";
    extra.kind = NodeKind::Transform;
    extra.operation = "paper_deduplicate";
    tame.nodes.push_back(extra);
    tame.node_map.emplace("more_dedupe", extra);
    const PatchRisk tame_risk = AssessPatchRisk(tame, *parsed);
    CHECK_FALSE(tame_risk.adds_side_effects);
    CHECK(tame_risk.reasons.empty());
}

TEST_CASE("snapshot 与增量事件:重连不重画、不漏节点") {
    using namespace lubancode::workflow;
    // 跑一场真 run(带 journal)。
    TempDir tmp;
    const char* yaml = R"YAML(
schema_version: 1
id: snap-flow
version: 1.0.0
name: s
entry: x
nodes:
  x:
    type: transform
    operation: echo
  y:
    type: transform
    operation: echo
  fin:
    type: end
edges:
  - { from: x, on: success, to: y }
  - { from: y, on: success, to: fin }
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    auto echo = std::make_shared<TransformExecutor>();
    echo->Register("echo", [](const nlohmann::json& in) { return in; });
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = echo;
    options.runs_root = tmp.Get();
    options.run_id_generator = [] { return "run-snap-1"; };
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(*parsed, RunInputs{});
    REQUIRE(summary.state == RunState::Succeeded);

    // snapshot 从 journal 拼:last_seq 与节点账齐。
    const std::vector<JournalEvent> events = ReadJournalEvents(tmp.Get() / "run-snap-1");
    REQUIRE(events.size() >= 5);
    const WorkflowRunSnapshot snapshot = BuildSnapshot(summary, events);
    CHECK(snapshot.state == "succeeded");
    CHECK(snapshot.last_seq == events.back().seq);
    CHECK(snapshot.nodes.count("x") == 1);
    CHECK(snapshot.nodes.at("x")["state"] == "succeeded");

    // JSON round-trip。
    const auto revived = WorkflowRunSnapshot::FromJson(snapshot.ToJson());
    REQUIRE(revived.has_value());
    CHECK(revived->run_id == snapshot.run_id);
    CHECK(revived->last_seq == snapshot.last_seq);
    CHECK(revived->nodes.size() == snapshot.nodes.size());

    // 增量事件:从 last_seq 起不重画(只接 seq 更大的)。
    const std::uint64_t seen = events[2].seq;
    const auto incremental = BuildIncrementalEvents(events, "thread-1", seen);
    REQUIRE(incremental.size() == events.size() - 3);
    CHECK(incremental[0].envelope.seq == seen + 1);
    CHECK(incremental[0].envelope.thread_id == "thread-1");
    CHECK(incremental[0].payload["type"] == events[3].type);

    // 事件不带 ANSI/终端宽(合同):payload 是纯领域数据。
    for (const auto& event : incremental) {
        const std::string dumped = event.payload.dump();
        CHECK(dumped.find("\x1b[") == std::string::npos);
    }

    // 磁盘 snapshot(前端不读私有目录,经这只口子)。
    const auto from_disk = LoadSnapshotFromDisk(tmp.Get() / "run-snap-1");
    REQUIRE(from_disk.has_value());
    CHECK(from_disk->state == "succeeded");
    CHECK(from_disk->nodes.count("x") == 1);
}

}  // TEST_SUITE(workflows-planner)
