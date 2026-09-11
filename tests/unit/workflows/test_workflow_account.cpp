// Workflow 编排账册(Workflow 接入 v3 第一棒):schema 冻结(身份/产物/
// checkpoint)、目录 resolver、无损恢复重放与单写者合同。恢复判据按 §十:
// node outcome=error 不判成功、commit 是下游可消费的唯一依据、悬置候选
// 采纳不重跑、定义 hash 冻结、终态 run 不复活。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "hooks/hash.hpp"
#include "platform/atomic_write.hpp"
#include "trajectory/canonical_json.hpp"
#include "workflow/account.hpp"
#include "workflow/definition.hpp"

namespace {

namespace fs = std::filesystem;
using namespace lubancode::workflow;

fs::path TempRoot(const char* tag) {
    static int counter = 0;
    ++counter;
    const fs::path dir = fs::temp_directory_path() /
                         ("lubancode_wf_account_" + std::string(tag) + "_" +
                          std::to_string(reinterpret_cast<std::uintptr_t>(&counter)) + "_" +
                          std::to_string(counter));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

std::string ReadText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::vector<std::string> LedgerLines(const fs::path& run_dir, const std::string& segment) {
    return [&] {
        std::ifstream file(run_dir / "segments" / segment / "workflow.jsonl", std::ios::binary);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty()) lines.push_back(line);
        }
        return lines;
    }();
}

// 极小定义:两个 transform 节点。
WorkflowDefinition MakeDefinition() {
    WorkflowDefinition def;
    def.schema_version = 1;
    def.id = "acc-flow";
    def.version = "1.0.0";
    def.entry = "a";
    WorkflowNode a;
    a.id = "a";
    a.kind = NodeKind::Transform;
    a.operation = "echo";
    WorkflowNode fin;
    fin.id = "fin";
    fin.kind = NodeKind::End;
    def.nodes = {a, fin};
    def.node_map.emplace("a", a);
    def.node_map.emplace("fin", fin);
    def.edges.push_back(WorkflowEdge{"a", "success", "fin"});
    def.normalized = BuildNormalizedJson(def);
    return def;
}

WorkflowRunAccount::DefinitionInfo MakeDefInfo(const WorkflowDefinition& def) {
    WorkflowRunAccount::DefinitionInfo info;
    info.workflow_id = def.id;
    info.workflow_version = def.version;
    info.content_hash = ContentHash(def);
    info.cwd = "/tmp";
    info.definition_json = BuildNormalizedJson(def).dump();
    return info;
}

const nlohmann::json kInputs = nlohmann::json{{"topic", std::string("x")}};

}  // namespace

TEST_CASE("目录 resolver:规范目录树,session 目录体系之外") {
    const fs::path root = TempRoot("resolver");
    auto resolver = WorkflowPathResolver::ForRun(root, "run-x");
    REQUIRE(resolver.has_value());
    CHECK(resolver->run_dir() == root / "run-x");
    CHECK(resolver->definition_path() == root / "run-x" / "definition.json");
    CHECK(resolver->bindings_path() == root / "run-x" / "bindings.json");
    CHECK(resolver->inputs_path() == root / "run-x" / "inputs.json");
    CHECK(resolver->segment_stream("seg-2") ==
          root / "run-x" / "segments" / "seg-2" / "workflow.jsonl");
    CHECK(resolver->checkpoint_path("cp-000001") ==
          root / "run-x" / "checkpoints" / "cp-000001.json");
    CHECK(resolver->output_path("out-000002") == root / "run-x" / "outputs" / "out-000002.json");
    CHECK(resolver->node_dir("run-x-a-d1") == root / "run-x" / "nodes" / "run-x-a-d1");
    CHECK(resolver->artifacts_dir() == root / "run-x" / "artifacts");
    CHECK(resolver->subflows_dir() == root / "run-x" / "subflows");
    CHECK(WorkflowPathResolver::NextSegmentId("") == "seg-1");
    CHECK(WorkflowPathResolver::NextSegmentId("seg-3") == "seg-4");
    CHECK_FALSE(WorkflowPathResolver::ForRun(root, "a/b").has_value());  // 非法单段名拒
}

TEST_CASE("Start:开账三件(定义快照/绑定/输入)+ 首段两事件") {
    const fs::path root = TempRoot("start");
    const WorkflowDefinition def = MakeDefinition();
    auto account = WorkflowRunAccount::Start(root, "run-1", MakeDefInfo(def), kInputs);
    REQUIRE(account.has_value());
    CHECK(fs::exists(root / "run-1" / "definition.json"));
    CHECK(fs::exists(root / "run-1" / "bindings.json"));
    CHECK(fs::exists(root / "run-1" / "inputs.json"));
    CHECK(fs::exists(root / "run-1" / "segments" / "seg-1" / "workflow.jsonl"));
    const auto lines = LedgerLines(root / "run-1", "seg-1");
    REQUIRE(lines.size() == 2);
    const nlohmann::json opened = nlohmann::json::parse(lines[0]);
    CHECK(opened.at("kind") == "workflow.definition.loaded");
    CHECK(opened.at("payload").at("definitionHash") == ContentHash(def));
    const nlohmann::json inputs_event = nlohmann::json::parse(lines[1]);
    CHECK(inputs_event.at("kind") == "workflow.inputs.committed");

    SUBCASE("run 目录已存在即拒(单写者,不覆写)") {
        auto again = WorkflowRunAccount::Start(root, "run-1", MakeDefInfo(def), kInputs);
        CHECK_FALSE(again.has_value());
        CHECK(again.error().stage == "start_dirs");
    }
}

TEST_CASE("节点执行与产物提交:reserve → dispatch → 原件 → commit 事件") {
    const fs::path root = TempRoot("commit");
    const WorkflowDefinition def = MakeDefinition();
    auto account = WorkflowRunAccount::Start(root, "run-1", MakeDefInfo(def), kInputs);
    REQUIRE(account.has_value());

    const nlohmann::json input = nlohmann::json{{"topic", std::string("x")}};
    auto reserved = account->ReserveNodeExecution("a", "transform", -1, input);
    REQUIRE(reserved.has_value());
    CHECK(reserved->node_execution_id == "run-1-a-d1");
    CHECK(reserved->attempt_id() == "run-1-a-d1-a1");
    CHECK(account->RecordNodeDispatched(*reserved));

    OutputValidation validation;  // 无 schema:过
    const nlohmann::json payload = nlohmann::json{{"rendered", std::string("hello")}};
    auto commit = account->CommitNodeOutput(*reserved, payload, validation);
    REQUIRE(commit.has_value());
    CHECK(commit->output_id == "out-000001");
    CHECK(fs::exists(root / "run-1" / "outputs" / "out-000001.json"));
    // 原件无损:读回的 payload 与提交一致(不打码)。
    const auto record = OutputCommitRecord::FromJson(
        nlohmann::json::parse(ReadText(root / "run-1" / "outputs" / "out-000001.json")));
    REQUIRE(record.has_value());
    CHECK(record->payload == payload);
    CHECK(record->validation_passed);

    CHECK(account->RecordNodeCompleted(*reserved, "success", 12, 3));

    // 事件序:opened/inputs 之后 reserved → dispatched → output.committed →
    // node.completed。
    const auto lines = LedgerLines(root / "run-1", "seg-1");
    REQUIRE(lines.size() == 6);
    std::vector<std::string> kinds;
    for (const auto& line : lines) kinds.push_back(nlohmann::json::parse(line).at("kind"));
    CHECK(kinds[2] == "workflow.node.reserved");
    CHECK(kinds[3] == "workflow.node.dispatched");
    CHECK(kinds[4] == "workflow.output.committed");
    CHECK(kinds[5] == "workflow.node.completed");
    const nlohmann::json commit_event = nlohmann::json::parse(lines[4]);
    CHECK(commit_event.at("payload").at("outputHash") == commit->payload_sha256);
    CHECK(commit_event.at("payload").at("resolvedInputHash") == reserved->input_sha256);
}

TEST_CASE("校验不过:候选留档(validationPassed=false),不落 commit 事件") {
    const fs::path root = TempRoot("reject");
    const WorkflowDefinition def = MakeDefinition();
    auto account = WorkflowRunAccount::Start(root, "run-1", MakeDefInfo(def), kInputs);
    REQUIRE(account.has_value());
    auto reserved = account->ReserveNodeExecution("a", "llm", -1, nlohmann::json::object());
    REQUIRE(reserved.has_value());

    OutputValidation failed;
    failed.passed = false;
    failed.checks.push_back(nlohmann::json{{"code", "missing_required_field"},
                                           {"field", "summary"},
                                           {"expected", std::string("必填")},
                                           {"actual", std::string("缺字段")}});
    account->SaveRejectedCandidate(*reserved, nlohmann::json{{"text", std::string("raw")}}, failed);
    const auto record = OutputCommitRecord::FromJson(
        nlohmann::json::parse(ReadText(root / "run-1" / "outputs" / "out-000001.json")));
    REQUIRE(record.has_value());
    CHECK(record->validation_passed == false);
    // 账上只有 opened/inputs/reserved 三事件——没有 output.committed。
    const auto lines = LedgerLines(root / "run-1", "seg-1");
    for (const auto& line : lines) {
        CHECK(nlohmann::json::parse(line).at("kind") != "workflow.output.committed");
    }
}

TEST_CASE("Resume:重放恢复判据(commit 判成功、failed 不误跳、悬置候选)") {
    const fs::path root = TempRoot("resume");
    const WorkflowDefinition def = MakeDefinition();
    {
        auto account = WorkflowRunAccount::Start(root, "run-1", MakeDefInfo(def), kInputs);
        REQUIRE(account.has_value());
        // a:完整成功(reserve/dispatch/commit/completed)。
        auto a = account->ReserveNodeExecution("a", "transform", -1, nlohmann::json::object());
        REQUIRE(a.has_value());
        CHECK(account->RecordNodeDispatched(*a));
        OutputValidation ok;
        auto commit = account->CommitNodeOutput(*a, nlohmann::json{{"v", 1}}, ok);
        REQUIRE(commit.has_value());
        CHECK(account->RecordNodeCompleted(*a, "success", 5, 2));
        // b:失败(outcome=error 绝不判成功)。
        auto b = account->ReserveNodeExecution("b", "transform", -1, nlohmann::json::object());
        REQUIRE(b.has_value());
        CHECK(account->RecordNodeDispatched(*b));
        CHECK(account->RecordNodeFailed(*b, "boom", "炸了", 3, 1));
        // c:悬置候选(手工落一份没有 commit 事件的原件,模拟保存原件后崩溃)。
        OutputCommitRecord dangling;
        dangling.schema = "workflow-output/1";
        dangling.output_id = "out-000003";
        dangling.workflow_run_id = "run-1";
        dangling.node_id = "c";
        dangling.node_execution_id = "run-1-c-d3";
        dangling.attempt_id = "run-1-c-d3-a1";
        dangling.payload = nlohmann::json{{"c", true}};
        dangling.payload_sha256 = commit->payload_sha256;  // 先占位,下面换真值
        {
            const auto dump = lubancode::trajectory::CanonicalJsonDump(dangling.payload);
            dangling.payload_sha256 = lubancode::hooks::Sha256Hex(*dump);
        }
        dangling.validation_passed = true;
        dangling.validation = OutputValidation{}.ToJson();
        REQUIRE(lubancode::platform::AtomicWriteFile(
                    root / "run-1" / "outputs" / "out-000003.json", dangling.ToJson().dump())
                    .has_value());
    }

    auto resumed = WorkflowRunAccount::Resume(root, "run-1");
    REQUIRE(resumed.has_value());
    const RecoveryState& recovery = resumed->recovery();
    CHECK(recovery.workflow_id == "acc-flow");
    CHECK(recovery.definition_hash == ContentHash(def));
    CHECK(recovery.inputs == kInputs);

    // a:commit 在 → committed_output 无损回灌,判成功。
    const auto a_exec = recovery.executions.find("run-1-a-d1");
    REQUIRE(a_exec != recovery.executions.end());
    CHECK(a_exec->second.output_committed);
    CHECK(a_exec->second.outcome == "success");
    CHECK(a_exec->second.committed_output == nlohmann::json{{"v", 1}});
    CHECK(recovery.node_latest_execution.at("a") == "run-1-a-d1");

    // b:失败事实保留,绝不因有事件而判成功。
    const auto b_exec = recovery.executions.find("run-1-b-d2");
    REQUIRE(b_exec != recovery.executions.end());
    CHECK(b_exec->second.outcome == "failed");
    CHECK(b_exec->second.error_code == "boom");
    CHECK_FALSE(b_exec->second.output_committed);

    // c:悬置候选收进 dangling_candidates(不丢候选,不伪造 commit)。
    REQUIRE(recovery.dangling_candidates.size() == 1);
    CHECK(recovery.dangling_candidates[0].node_id == "c");
    CHECK(recovery.dangling_candidates[0].output_id == "out-000003");

    // 预算底数与 token 账不归零。
    CHECK(recovery.tokens_used == 3);
    CHECK(recovery.dispatch_count == 2);
    // dispatch 号续起:新铸 id 不与旧执行撞名。
    const NodeExecutionIdentity next = resumed->MintExecutionId("a", -1);
    CHECK(next.node_execution_id == "run-1-a-d3");
}

TEST_CASE("Resume:checkpoint 用已提交事件,孤立候选文件不生效") {
    const fs::path root = TempRoot("checkpoint");
    const WorkflowDefinition def = MakeDefinition();
    {
        auto account = WorkflowRunAccount::Start(root, "run-1", MakeDefInfo(def), kInputs);
        REQUIRE(account.has_value());
        auto a = account->ReserveNodeExecution("a", "transform", -1, nlohmann::json::object());
        REQUIRE(a.has_value());
        (void)account->CommitNodeOutput(*a, nlohmann::json{{"v", 1}}, OutputValidation{});
        auto cp = account->CommitCheckpoint(nlohmann::json{{"inputs", kInputs},
                                                           {"nodes", nlohmann::json::object()}});
        REQUIRE(cp.has_value());
        CHECK(cp->checkpoint_id == "cp-000001");
        // 孤立 checkpoint:文件在、事件无(不生效)。
        CheckpointRecord orphan = *cp;
        orphan.checkpoint_id = "cp-000002";
        REQUIRE(lubancode::platform::AtomicWriteFile(
                    root / "run-1" / "checkpoints" / "cp-000002.json", orphan.ToJson().dump())
                    .has_value());
    }
    auto resumed = WorkflowRunAccount::Resume(root, "run-1");
    REQUIRE(resumed.has_value());
    REQUIRE(resumed->recovery().checkpoint.has_value());
    CHECK(resumed->recovery().checkpoint->checkpoint_id == "cp-000001");  // 孤立件不采
    CHECK(resumed->recovery().checkpoint->store.at("inputs") == kInputs);
}

TEST_CASE("Resume:定义快照被改(hash 不符)即拒——配置 reload 偷换不了定义") {
    const fs::path root = TempRoot("deftamper");
    const WorkflowDefinition def = MakeDefinition();
    {
        auto account = WorkflowRunAccount::Start(root, "run-1", MakeDefInfo(def), kInputs);
        REQUIRE(account.has_value());
    }
    // 篡改快照正文(同 hash 声明,内容变了)。
    REQUIRE(lubancode::platform::AtomicWriteFile(
                root / "run-1" / "definition.json",
                R"({"schema_version":1,"id":"acc-flow","version":"9.9.9","entry":"a"})")
                .has_value());
    auto resumed = WorkflowRunAccount::Resume(root, "run-1");
    CHECK_FALSE(resumed.has_value());
    CHECK(resumed.error().stage == "definition_hash");
}

TEST_CASE("终态 run 不复活:重放如实报 terminal,显式重跑另起新 run") {
    const fs::path root = TempRoot("terminal");
    const WorkflowDefinition def = MakeDefinition();
    {
        auto account = WorkflowRunAccount::Start(root, "run-1", MakeDefInfo(def), kInputs);
        REQUIRE(account.has_value());
        CHECK(account->RecordRunFailed("node_failed", "a 炸了"));
        CHECK_FALSE(account->RecordRunCompleted(nlohmann::json::object()));  // 终态唯一
    }
    auto resumed = WorkflowRunAccount::Resume(root, "run-1");
    REQUIRE(resumed.has_value());
    CHECK(resumed->recovery().run_terminal == "failed");
}

TEST_CASE("OpenSegment:开恢复段链接源水位,单写者不覆写") {
    const fs::path root = TempRoot("segment");
    const WorkflowDefinition def = MakeDefinition();
    std::string seg2_tail_hash;
    {
        auto account = WorkflowRunAccount::Start(root, "run-1", MakeDefInfo(def), kInputs);
        REQUIRE(account.has_value());
        auto segment = account->OpenSegment();
        REQUIRE(segment.has_value());
        CHECK(*segment == "seg-2");
        const auto lines = LedgerLines(root / "run-1", "seg-2");
        REQUIRE(lines.size() == 1);
        const nlohmann::json opened = nlohmann::json::parse(lines[0]);
        CHECK(opened.at("kind") == "workflow.segment.opened");
        const nlohmann::json source = opened.at("payload").at("sourceRef");
        CHECK(source.at("runId") == "seg-1");
        CHECK(source.at("sessionId") == "run-1");
        CHECK(source.at("seq").get<std::uint64_t>() == 2);  // seg-1 末行 seq
        CHECK(source.at("id").get<std::string>().rfind("evt-", 0) == 0);
        CHECK(lubancode::trajectory::v3::IsHex64(source.at("hash").get<std::string>()));
        seg2_tail_hash = opened.at("lineHash").get<std::string>();
    }
    // 恢复重放后再开段:续在 seg-2 之后(seg-3),并拒绝覆盖已存在的段。
    auto resumed = WorkflowRunAccount::Resume(root, "run-1");
    REQUIRE(resumed.has_value());
    CHECK(resumed->recovery().tail_segment_id == "seg-2");
    CHECK(resumed->recovery().tail_segment_last_seq == 1);  // seg-2 只有一行
    CHECK(resumed->recovery().tail_segment_last_hash == seg2_tail_hash);
    auto segment = resumed->OpenSegment();
    REQUIRE(segment.has_value());
    CHECK(*segment == "seg-3");
    // 段已存在(另一恢复者先开):单写者拒。
    auto resumed_again = WorkflowRunAccount::Resume(root, "run-1");
    REQUIRE(resumed_again.has_value());
    auto conflict = resumed_again->OpenSegment();
    CHECK_FALSE(conflict.has_value());
    CHECK(conflict.error().stage == "segment_open");
}
