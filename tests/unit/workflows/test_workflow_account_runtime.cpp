// Workflow 编排账 × runtime 册(Workflow 接入 v3 第一棒):§十二 表的本棒
// 先行列,全部经 WorkflowRuntime 真跑——
//   1. 纯 template 图零伪造模型消息(事件账只有 event 行)
//   2. 输出 schema 不合:保存候选,不提交 success、不推进 success 边
//   3. node error 后 resume 不误跳成成功
//   4. commit 后崩溃补内存不重跑 / 保存原件后崩溃从候选续,不重跑
//   5. 编排提交写失败:未落稳的节点不成功,后继零派发
// 另钉:恢复沿用 run 身份、预算计数不归零、旧 RunJournal 在账路不启。

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "workflow/account.hpp"
#include "workflow/parser.hpp"
#include "workflow/runtime.hpp"

namespace {

namespace fs = std::filesystem;
using namespace lubancode::workflow;

fs::path TempRoot(const char* tag) {
    static int counter = 0;
    ++counter;
    const fs::path dir =
        fs::temp_directory_path() / ("lubancode_wf_rt3_" + std::string(tag) + "_" +
                                     std::to_string(reinterpret_cast<std::uintptr_t>(&counter)) +
                                     "_" + std::to_string(counter));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

std::vector<std::string> SegmentLines(const fs::path& run_dir, const std::string& segment) {
    std::ifstream file(run_dir / "segments" / segment / "workflow.jsonl", std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::vector<nlohmann::json> SegmentEvents(const fs::path& run_dir, const std::string& segment) {
    std::vector<nlohmann::json> events;
    for (const auto& raw : SegmentLines(run_dir, segment)) {
        events.push_back(nlohmann::json::parse(raw));
    }
    return events;
}

// 按节点计数的假执行器:每个节点一档脚本,耗尽后重复最后一格。
class PerNodeExecutor : public NodeExecutor {
public:
    struct Step {
        bool ok = true;
        std::string error_code;
        nlohmann::json output = nlohmann::json::object();
        std::int64_t tokens = 0;
    };
    std::map<std::string, std::vector<Step>> script;
    std::map<std::string, int> calls;

    NodeExecResult Execute(const NodeExecRequest& request) override {
        const std::string id = request.node->id;
        calls[id] += 1;
        const auto it = script.find(id);
        if (it == script.end() || it->second.empty()) {
            NodeExecResult result;
            result.ok = true;
            result.output = nlohmann::json{{"echo", true}};
            return result;
        }
        const std::size_t index = std::min(static_cast<std::size_t>(calls[id]), it->second.size()) - 1;
        const Step& step = it->second[index];
        NodeExecResult result;
        result.ok = step.ok;
        result.error_code = step.error_code;
        result.output = step.output;
        result.tokens_used = step.tokens;
        return result;
    }
};

RuntimeOptions BaseOptions(const fs::path& account_root, std::shared_ptr<NodeExecutor> executor) {
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    options.executors[NodeKind::Template] = executor;
    options.account_root = account_root;
    options.run_id_generator = [] { return "run-acc"; };
    return options;
}

const char* kLinearYaml = R"YAML(
schema_version: 1
id: acc-linear
version: 1.0.0
entry: a
nodes:
  a: { type: transform, operation: echo }
  b: { type: transform, operation: echo }
  fin: { type: end }
edges:
  - { from: a, on: success, to: b }
  - { from: b, on: success, to: fin }
result:
  a: "${nodes.a.output}"
)YAML";

}  // namespace

TEST_CASE("案1:纯 template 图零伪造模型消息,事件账只有 event 行") {
    const fs::path root = TempRoot("pure");
    auto parsed = ParseWorkflowYaml(kLinearYaml);
    REQUIRE(parsed.has_value());
    auto executor = std::make_shared<PerNodeExecutor>();
    executor->script["a"] = {{true, "", nlohmann::json{{"text", std::string("甲")}}}};
    executor->script["b"] = {{true, "", nlohmann::json{{"text", std::string("乙")}}}};
    WorkflowRuntime runtime(BaseOptions(root, executor));
    const auto summary = runtime.Run(*parsed, RunInputs{});
    REQUIRE(summary.state == RunState::Succeeded);

    const fs::path run_dir = root / "run-acc";
    const auto events = SegmentEvents(run_dir, "seg-1");
    REQUIRE(events.size() >= 6);
    for (const auto& event : events) {
        // 零伪造:没有 system、没有 message 行——编排账不冒充模型对话。
        CHECK(event.at("type").get<std::string>() == "event");
        CHECK(event.at("kind").get<std::string>().rfind("workflow.", 0) == 0);
    }
    // 结构性证明:事件账验卷(profile 只认 event 行)全绿。
    const auto report = lubancode::trajectory::v3::VerifyV3EventLedgerFile(
        run_dir / "segments" / "seg-1" / "workflow.jsonl");
    CHECK(report.ok);
    // 账路不启旧 RunJournal:没有 events.jsonl/manifest.json(单事实源)。
    CHECK_FALSE(fs::exists(run_dir / "events.jsonl"));
    CHECK_FALSE(fs::exists(run_dir / "manifest.json"));
    // 无损产物在 outputs/ 落稳。
    CHECK(fs::exists(run_dir / "outputs" / "out-000001.json"));
    CHECK(fs::exists(run_dir / "outputs" / "out-000002.json"));
}

TEST_CASE("案2:输出 schema 不合——保存候选,不提交 success、不推进 success 边") {
    const fs::path root = TempRoot("schema");
    const char* yaml = R"YAML(
schema_version: 1
id: acc-schema
version: 1.0.0
entry: gen
nodes:
  gen:
    type: transform
    operation: echo
    output_schema:
      type: object
      required: [summary]
      properties:
        summary: { type: string }
  fin: { type: end }
edges:
  - { from: gen, on: success, to: fin }
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    auto executor = std::make_shared<PerNodeExecutor>();
    // 候选能解析、字段缺失——content 外壳冒充不了合格产物。
    executor->script["gen"] = {{true, "", nlohmann::json{{"text", std::string("裸文本")}}}};
    WorkflowRuntime runtime(BaseOptions(root, executor));
    const auto summary = runtime.Run(*parsed, RunInputs{});

    // 校验失败:节点失败,run 收在失败态(没有 success 出边可走)。
    REQUIRE(summary.state == RunState::Failed);
    CHECK(summary.error_code == "node_failed");
    CHECK(summary.nodes.at("gen").state == NodeState::Failed);

    const fs::path run_dir = root / "run-acc";
    // 候选照常保存(观察件,validationPassed=false)。
    const auto saved = OutputCommitRecord::FromJson(
        nlohmann::json::parse(std::ifstream(run_dir / "outputs" / "out-000001.json")));
    REQUIRE(saved.has_value());
    CHECK(saved->validation_passed == false);
    CHECK(saved->payload == nlohmann::json{{"text", std::string("裸文本")}});
    // 账上无该产物的 output.committed——不提交 success。
    bool has_commit = false;
    for (const auto& event : SegmentEvents(run_dir, "seg-1")) {
        if (event.at("kind") == "workflow.output.committed") has_commit = true;
    }
    CHECK_FALSE(has_commit);
}

TEST_CASE("案3:node error 后 resume 不误跳成成功") {
    const fs::path root = TempRoot("nodefail");
    const char* yaml = R"YAML(
schema_version: 1
id: acc-nodefail
version: 1.0.0
entry: a
nodes:
  a:
    type: transform
    operation: echo
    retry: { attempts: 1 }
  b: { type: transform, operation: echo }
  fin: { type: end }
edges:
  - { from: a, on: error, to: b }
  - { from: a, on: success, to: fin }
  - { from: b, on: success, to: fin }
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());

    // 第一幕:a 失败走 error 边;b 派发前账断(注入)——run 无终态收场。
    auto first_executor = std::make_shared<PerNodeExecutor>();
    first_executor->script["a"] = {{false, "boom"}};
    first_executor->script["b"] = {{true, "", nlohmann::json{{"after", std::string("b")}}}};
    RuntimeOptions first_options = BaseOptions(root, first_executor);
    int writes = 0;
    first_options.account_fault = [&writes] {
        ++writes;
        // 事件序:definition.loaded / inputs / a.reserved / a.dispatched /
        // a.failed / b.reserved / b.dispatched —— b 的派发事实落不住。
        return writes >= 7 ? std::optional<std::string>("test.injected") : std::nullopt;
    };
    WorkflowRuntime first(first_options);
    const auto crashed = first.Run(*parsed, RunInputs{});
    REQUIRE(crashed.state == RunState::Failed);
    CHECK(crashed.error_code == "orchestration_ledger_broken");
    CHECK(first_executor->calls["a"] == 1);
    CHECK(first_executor->calls.count("b") == 0);  // 写失败:后继零派发(兼案5)

    // 第二幕:resume。a 的失败事实在账上(outcome=failed);a 绝不因
    // "有过 node 事件"被当成成功跳过——重放判据只认 commit。
    auto resumed_account = WorkflowRunAccount::Resume(root, "run-acc");
    REQUIRE(resumed_account.has_value());
    const RecoveryState& recovery = resumed_account->recovery();
    const auto a_exec = recovery.executions.find("run-acc-a-d1");
    REQUIRE(a_exec != recovery.executions.end());
    CHECK(a_exec->second.outcome == "failed");
    CHECK_FALSE(a_exec->second.output_committed);

    auto second_executor = std::make_shared<PerNodeExecutor>();
    second_executor->script["a"] = {{false, "boom"}};  // a 依旧失败
    second_executor->script["b"] = {{true, "", nlohmann::json{{"after", std::string("b")}}}};
    RuntimeOptions resume_options = BaseOptions(root, second_executor);
    resume_options.account_fault = nullptr;
    WorkflowRuntime resumer(resume_options);
    const auto resumed = resumer.Resume(root / "run-acc");
    REQUIRE(resumed.has_value());
    // a 被重跑(仍失败)而非误跳成功;b 经 error 边执行;run 成功。
    CHECK(second_executor->calls["a"] == 1);
    CHECK(second_executor->calls["b"] == 1);
    CHECK(resumed->state == RunState::Succeeded);
    CHECK(resumed->nodes.at("a").state == NodeState::Failed);  // 原失败结局不翻案
    CHECK(resumed->run_id == "run-acc");  // 恢复沿用逻辑 run 身份

    // 终态后 resume 拒绝复活:显式重跑另起新 run。
    const auto again = resumer.Resume(root / "run-acc");
    CHECK_FALSE(again.has_value());
}

TEST_CASE("案4a:commit 后崩溃——恢复补内存,不重跑副作用") {
    const fs::path root = TempRoot("commitcrash");
    auto parsed = ParseWorkflowYaml(kLinearYaml);
    REQUIRE(parsed.has_value());

    auto first_executor = std::make_shared<PerNodeExecutor>();
    first_executor->script["a"] = {{true, "", nlohmann::json{{"who", std::string("a")}}}};
    first_executor->script["b"] = {{true, "", nlohmann::json{{"who", std::string("b")}}}};
    RuntimeOptions first_options = BaseOptions(root, first_executor);
    int writes = 0;
    first_options.account_fault = [&writes] {
        ++writes;
        // a.output.committed(第 5 枚)落稳后,a.node.completed(第 6 枚)
        // 注入失败——"模型/工具已完成,commit 已落,收口没落"。
        return writes == 6 ? std::optional<std::string>("test.injected") : std::nullopt;
    };
    WorkflowRuntime first(first_options);
    const auto crashed = first.Run(*parsed, RunInputs{});
    REQUIRE(crashed.state == RunState::Failed);
    CHECK(crashed.error_code == "orchestration_ledger_broken");

    auto second_executor = std::make_shared<PerNodeExecutor>();
    second_executor->script["a"] = {{true, "", nlohmann::json{{"who", std::string("a2")}}}};
    second_executor->script["b"] = {{true, "", nlohmann::json{{"who", std::string("b")}}}};
    WorkflowRuntime resumer(BaseOptions(root, second_executor));
    const auto resumed = resumer.Resume(root / "run-acc");
    REQUIRE(resumed.has_value());
    REQUIRE(resumed->state == RunState::Succeeded);
    // a 不重跑(副作用不做两遍);a 的产物沿用崩溃前那份("a" 非 "a2")。
    CHECK(second_executor->calls.count("a") == 0);
    CHECK(resumed->result.at("a").at("who") == "a");
    CHECK(second_executor->calls["b"] == 1);
}

TEST_CASE("案4b:保存原件后崩溃——从候选续校验提交,不重跑执行") {
    const fs::path root = TempRoot("candidate");
    auto parsed = ParseWorkflowYaml(kLinearYaml);
    REQUIRE(parsed.has_value());

    auto first_executor = std::make_shared<PerNodeExecutor>();
    first_executor->script["a"] = {{true, "", nlohmann::json{{"who", std::string("a")}}}};
    first_executor->script["b"] = {{true, "", nlohmann::json{{"who", std::string("b")}}}};
    RuntimeOptions first_options = BaseOptions(root, first_executor);
    int writes = 0;
    first_options.account_fault = [&writes] {
        ++writes;
        // a.output.committed 事件本身(第 5 枚)注入失败:原件已在
        // outputs/ 落稳,commit 事件没落——悬置候选。
        return writes == 5 ? std::optional<std::string>("test.injected") : std::nullopt;
    };
    WorkflowRuntime first(first_options);
    const auto crashed = first.Run(*parsed, RunInputs{});
    REQUIRE(crashed.state == RunState::Failed);
    // 原件已在、账上无 commit 事件(悬置候选在)。
    auto mid = WorkflowRunAccount::Resume(root, "run-acc");
    REQUIRE(mid.has_value());
    REQUIRE(mid->recovery().dangling_candidates.size() == 1);
    CHECK(mid->recovery().dangling_candidates[0].node_id == "a");

    auto second_executor = std::make_shared<PerNodeExecutor>();
    second_executor->script["a"] = {{true, "", nlohmann::json{{"who", std::string("a2")}}}};
    second_executor->script["b"] = {{true, "", nlohmann::json{{"who", std::string("b")}}}};
    WorkflowRuntime resumer(BaseOptions(root, second_executor));
    const auto resumed = resumer.Resume(root / "run-acc");
    REQUIRE(resumed.has_value());
    REQUIRE(resumed->state == RunState::Succeeded);
    // a 不重跑;悬置候选被采纳提交(沿用崩溃前的产物正文)。
    CHECK(second_executor->calls.count("a") == 0);
    CHECK(resumed->result.at("a").at("who") == "a");
    CHECK(second_executor->calls["b"] == 1);
    // 采纳落在恢复段(seg-2):悬置件变成有 commit 事件的正式产物。
    bool adopted_commit = false;
    for (const auto& event : SegmentEvents(root / "run-acc", "seg-2")) {
        if (event.at("kind") == "workflow.output.committed" &&
            event.at("payload").at("nodeId") == "a") {
            adopted_commit = true;
        }
    }
    CHECK(adopted_commit);
}

TEST_CASE("案5:编排提交写失败——未落稳的节点不成功,后继零派发") {
    const fs::path root = TempRoot("writefail");
    auto parsed = ParseWorkflowYaml(kLinearYaml);
    REQUIRE(parsed.has_value());

    auto executor = std::make_shared<PerNodeExecutor>();
    executor->script["a"] = {{true, "", nlohmann::json{{"who", std::string("a")}}}};
    RuntimeOptions options = BaseOptions(root, executor);
    int writes = 0;
    options.account_fault = [&writes] {
        ++writes;
        // a.reserved 事件(第 3 枚)就落不住:节点连执行都不开始。
        return writes >= 3 ? std::optional<std::string>("test.injected") : std::nullopt;
    };
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(*parsed, RunInputs{});
    REQUIRE(summary.state == RunState::Failed);
    CHECK(summary.error_code == "orchestration_ledger_broken");
    CHECK(summary.nodes.at("a").state == NodeState::Failed);
    // a 没执行、b 零派发、没有任何产物提交。
    CHECK(executor->calls.empty());
    const fs::path run_dir = root / "run-acc";
    for (const auto& event : SegmentEvents(run_dir, "seg-1")) {
        CHECK(event.at("kind") != "workflow.output.committed");
    }
    CHECK_FALSE(fs::exists(run_dir / "outputs" / "out-000001.json"));
}

TEST_CASE("账路恢复:预算计数不归零,token 账跨恢复延续") {
    const fs::path root = TempRoot("budget");
    auto parsed = ParseWorkflowYaml(kLinearYaml);
    REQUIRE(parsed.has_value());

    auto first_executor = std::make_shared<PerNodeExecutor>();
    first_executor->script["a"] = {{true, "", nlohmann::json{{"who", std::string("a")}}, 7}};
    first_executor->script["b"] = {{true, "", nlohmann::json{{"who", std::string("b")}}, 3}};
    RuntimeOptions first_options = BaseOptions(root, first_executor);
    int writes = 0;
    first_options.account_fault = [&writes] {
        ++writes;
        // a/b 全部收口(#1-#10 落稳),终态前 checkpoint(#11)注入失败:
        // 执行事实齐全、run 无终态——恢复只补收口,零新执行。
        return writes == 11 ? std::optional<std::string>("test.injected") : std::nullopt;
    };
    WorkflowRuntime first(first_options);
    const auto interrupted = first.Run(*parsed, RunInputs{});
    // 执行全成、终态没写住:summary 如实带 account_terminal_unwritten。
    REQUIRE(interrupted.state == RunState::Succeeded);
    CHECK(interrupted.error_code == "account_terminal_unwritten");
    CHECK(interrupted.tokens_used == 10);

    auto second_executor = std::make_shared<PerNodeExecutor>();
    WorkflowRuntime resumer(BaseOptions(root, second_executor));
    const auto resumed = resumer.Resume(root / "run-acc");
    REQUIRE(resumed.has_value());
    CHECK(resumed->state == RunState::Succeeded);
    CHECK(second_executor->calls.empty());  // 事实齐全:恢复零新执行
    // 计数不归零:两只节点的 token 账(7+3)原样带进恢复后的 run。
    CHECK(resumed->tokens_used == 10);
    // 终态这次写住了:再 resume 拒绝复活。
    CHECK_FALSE(resumer.Resume(root / "run-acc").has_value());
}
