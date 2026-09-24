// GAP-05 棒二册:节点独立 v3 session × 编排账。全部经 WorkflowRuntime 真跑,
// 挂真 TrajectorySessionLedger(v3 场)——
//   1. 模型节点(llm)各开独立场:run 目录 nodes/ 下落卷、父 session 持
//      spawn/linked 边、首行 systemMeta 带 nodeExecutionRef;usage 并进
//      /usage 主账口径(ReadSessionUsage 树走可见);编排账 terminal 事件
//      带 sessionRef 与场终态 hash。
//   2. kill 中途崩溃(注入)——resume 从最后 commit 续跑:已 commit 节点
//      不重执行、不重开场,产物沿用崩溃前那份。
//   3. 重试新开一场:attempt 递增,每 attempt 独立 session,账上 retrying。
//   4. 非模型节点零伪造:纯 transform 图不开任何场,父账零 spawn 边。
#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/session_usage_reader.hpp"  // ReadSessionUsage(读侧消费,不改)
#include "runtime/trajectory_session.hpp"
#include "workflow/account.hpp"
#include "workflow/host_executors.hpp"  // LlmExecutor(真执行器,走 SampleModel 边界)
#include "workflow/parser.hpp"
#include "workflow/runtime.hpp"

namespace {

namespace fs = std::filesystem;
using namespace lubancode::workflow;

// 会话格式守卫(T16 分账:本册经 TrajectorySessionLedger::Open 建场口且
// 断言 v3 行为,册内自证 explicit-v3——ctest 对未迁域注入的
// LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0 在这被盖成 1,与生产默认同拍)。
struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

fs::path FreshDir(const std::string& name) {
    static int counter = 0;
    ++counter;
    const fs::path dir = fs::temp_directory_path() /
                         ("lubancode_wf_ns_" + name + "_" +
                          std::to_string(reinterpret_cast<std::uintptr_t>(&counter)) + "_" +
                          std::to_string(counter));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

std::optional<lubancode::runtime::TrajectorySessionLedger> OpenLedger(const fs::path& root) {
    lubancode::runtime::TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.lubancode_version = "test";
    std::error_code ec;
    fs::create_directories(root / "repo", ec);
    auto ledger = lubancode::runtime::TrajectorySessionLedger::Open(std::move(options));
    if (!ledger.has_value()) {
        return std::nullopt;
    }
    return std::move(*ledger);
}

WorkflowDefinition ParseOrDie(const char* yaml) {
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

// 假 backend:吐一段固定 JSON 文本,带 usage(llm 节点经 SampleModel 原语,
// 边界记账走节点场)。
class FakeBackend : public lubancode::api::Backend {
public:
    std::string reply = "{\"plan\": \"ok\"}";
    std::int64_t input_tokens = 100;
    std::int64_t output_tokens = 50;
    int calls = 0;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        (void)request;
        (void)cancel;
        ++calls;
        on_event(lubancode::api::TextDelta{reply});
        lubancode::api::MessageDone done;
        done.usage.input_tokens = input_tokens;
        done.usage.output_tokens = output_tokens;
        on_event(done);
        return {};
    }
};

// 首发即败的 backend:retry.when 命中 api_error 时验证重试换场。
class FailingBackend : public lubancode::api::Backend {
public:
    int fails = 1;  // 前几次调用失败,之后成功
    std::string reply = "{\"plan\": \"recovered\"}";
    int calls = 0;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        (void)request;
        (void)cancel;
        ++calls;
        if (calls <= fails) {
            return std::unexpected(lubancode::api::Error{lubancode::api::ErrorKind::Api, "接口崩了"});
        }
        on_event(lubancode::api::TextDelta{reply});
        lubancode::api::MessageDone done;
        done.usage.input_tokens = 30;
        done.usage.output_tokens = 10;
        on_event(done);
        return {};
    }
};

// 数调用的 prompt loader(按 prompt 名计数:llm 节点每执行一次调一次)。
struct CountingLoader {
    std::map<std::string, int> calls;
    lubancode::workflow::PromptLoader loader() {
        return [this](const std::string& prompt) {
            ++calls[prompt];
            return "系统提示(" + prompt + ")";
        };
    }
};

std::vector<nlohmann::json> LinesOf(const fs::path& jsonl) {
    std::vector<nlohmann::json> out;
    std::ifstream file(jsonl, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        out.push_back(nlohmann::json::parse(line, nullptr, false));
    }
    return out;
}

std::vector<nlohmann::json> AccountEvents(const fs::path& run_dir, const std::string& segment) {
    return LinesOf(run_dir / "segments" / segment / "workflow.jsonl");
}

int CountSessionDirs(const fs::path& exec_dir) {
    std::error_code ec;
    int count = 0;
    const fs::path sessions = exec_dir / "sessions";
    if (!fs::exists(sessions, ec)) {
        return 0;
    }
    for (const auto& entry : fs::directory_iterator(sessions, ec)) {
        if (entry.is_directory(ec)) {
            ++count;
        }
    }
    return count;
}

const char* kTwoLlmYaml = R"YAML(
schema_version: 1
id: ns-two-llm
version: 1.0.0
entry: a
nodes:
  a:
    type: llm
    prompt: prompts/a.md
    input: { q: "${inputs.topic}" }
  b:
    type: llm
    prompt: prompts/b.md
    input: { from: "${nodes.a.output}" }
  fin: { type: end }
edges:
  - { from: a, on: success, to: b }
  - { from: b, on: success, to: fin }
result:
  a: "${nodes.a.output}"
)YAML";

}  // namespace

TEST_CASE("GAP-05 案1:模型节点各开独立场,usage 并进 /usage 主账口径") {
    EnvGuard v3_pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const fs::path root = FreshDir("two-llm");
    auto opened = OpenLedger(root);
    REQUIRE(opened.has_value());
    auto ledger = std::make_unique<lubancode::runtime::TrajectorySessionLedger>(std::move(*opened));
    REQUIRE(ledger->v3_main_writer() != nullptr);  // 前提:默认新会话走 v3

    auto backend = std::make_shared<FakeBackend>();
    auto loader = std::make_shared<CountingLoader>();
    LlmExecutor::Options llm_options;
    llm_options.backend = backend.get();
    llm_options.model = "fake-model";
    llm_options.prompt_loader = loader->loader();
    auto llm = std::make_shared<LlmExecutor>(std::move(llm_options));

    RuntimeOptions options;
    options.executors[NodeKind::Llm] = llm;
    options.trajectory_ledger = ledger.get();
    options.account_root = root / "workflow-runs";
    options.run_id_generator = [] { return "run-ns"; };
    const WorkflowDefinition def = ParseOrDie(kTwoLlmYaml);
    const WorkflowRunSummary summary =
        WorkflowRuntime(std::move(options)).Run(def, RunInputs{nlohmann::json{{"topic", "账"}}});

    REQUIRE(summary.state == RunState::Succeeded);
    CHECK(summary.tokens_used == 2 * (100 + 50));

    const fs::path run_dir = root / "workflow-runs" / "run-ns";
    // 每节点一场:nodeExecutionId 形状 run-ns-<node>-d<N>(派发号全场递增,
    // a 是首派发 d1,b 是次派发 d2)。
    REQUIRE(CountSessionDirs(run_dir / "nodes" / "run-ns-a-d1") == 1);
    REQUIRE(CountSessionDirs(run_dir / "nodes" / "run-ns-b-d2") == 1);

    // 父 session 持 spawn/linked 边(各两枚),journalPath 指进 run 目录。
    const fs::path parent_dir = ledger->session_dir();
    const std::string parent_sid = ledger->session_id();
    const std::vector<nlohmann::json> parent_lines = LinesOf(parent_dir / (parent_sid + ".jsonl"));
    int spawn_edges = 0;
    int linked_edges = 0;
    std::string a_session_id;
    for (const auto& line : parent_lines) {
        if (!line.is_object()) continue;
        const std::string kind = line.value("kind", std::string());
        if (kind == "subagent.spawn.requested") {
            ++spawn_edges;
            const auto& task_args = line.at("payload").at("taskArgs");
            CHECK(task_args.value("workflowRunId", std::string()) == "run-ns");
            CHECK(task_args.value("nodeExecutionId", std::string()).find("run-ns-") == 0);
            if (task_args.value("nodeId", std::string()) == "a") {
                a_session_id = line.at("payload").at("childSessionRef").value("sessionId", std::string());
            }
        } else if (kind == "subagent.linked") {
            ++linked_edges;
        }
    }
    CHECK(spawn_edges == 2);
    CHECK(linked_edges == 2);
    REQUIRE_FALSE(a_session_id.empty());

    // 节点场首行 system:systemMeta 带 cause=workflow_node 与 nodeExecutionRef。
    const fs::path a_jsonl = run_dir / "nodes" / "run-ns-a-d1" / "sessions" / a_session_id /
                             (a_session_id + ".jsonl");
    const std::vector<nlohmann::json> a_lines = LinesOf(a_jsonl);
    REQUIRE_FALSE(a_lines.empty());
    const nlohmann::json& first = a_lines.front();
    REQUIRE(first.value("type", std::string()) == "message");
    REQUIRE(first.at("message").value("role", std::string()) == "system");
    const nlohmann::json& meta = first.at("systemMeta");
    CHECK(meta.value("cause", std::string()) == "workflow_node");
    CHECK(meta.value("workflowRunId", std::string()) == "run-ns");
    CHECK(meta.value("nodeExecutionId", std::string()) == "run-ns-a-d1");
    CHECK(meta.value("attemptId", std::string()) == "run-ns-a-d1-a1");
    CHECK(meta.at("spawnEventRef").value("sessionId", std::string()) == parent_sid);
    // 场内有 assistant(usage owner 落在这;§4.12 usage 是行顶层键)。
    bool has_assistant = false;
    for (const auto& line : a_lines) {
        if (line.value("type", std::string()) == "message" &&
            line.at("message").value("role", std::string()) == "assistant") {
            has_assistant = true;
            CHECK(line.contains("usage"));
            CHECK(line.at("usage").is_object());
        }
    }
    CHECK(has_assistant);

    // /usage 主账口径:ReadSessionUsage 树走从父场递归收节点场(两枚样本,
    // 各 100/50;样本的 session 是节点场自己的,不是父场)。
    const auto usage = lubancode::accounting::ReadSessionUsage(parent_dir);
    REQUIRE(usage.ok);
    int node_samples = 0;
    std::int64_t node_input_tokens = 0;
    for (const auto& sample : usage.samples) {
        if (sample.session_id == parent_sid) continue;
        ++node_samples;
        node_input_tokens += sample.total_input_tokens;
        CHECK(sample.run_id.find("run-ns-") == 0);  // attemptId 作场内 run 身份
    }
    CHECK(node_samples == 2);
    CHECK(node_input_tokens == 2 * 100);

    // 编排账 terminal 事件带 sessionRef 与场终态 hash。
    bool a_completed_with_ref = false;
    for (const auto& event : AccountEvents(run_dir, "seg-1")) {
        if (!event.is_object() || event.value("kind", std::string()) != "workflow.node.completed") {
            continue;
        }
        const auto& payload = event.at("payload");
        if (payload.value("nodeId", std::string()) != "a") continue;
        REQUIRE(payload.contains("sessionRef"));
        CHECK(payload.at("sessionRef").value("sessionId", std::string()) == a_session_id);
        CHECK(payload.at("sessionRef").value("terminalHash", std::string()).size() == 64);
        a_completed_with_ref = true;
    }
    CHECK(a_completed_with_ref);

    // 会话验卷:父子边全过(spawn/linked 与子卷首行互指)。
    const auto report = ledger->VerifySession();
    for (const auto& edge : report.child_edges) {
        CHECK(edge.error_code.empty());
    }
}

TEST_CASE("GAP-05 案2:kill 中途崩溃——resume 从最后 commit 续,不重跑不重开") {
    EnvGuard v3_pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const fs::path root = FreshDir("kill-resume");
    const WorkflowDefinition def = ParseOrDie(kTwoLlmYaml);

    // 第一幕:a 提交并收口后,b 派发前注入账断——"kill 在 a 完成之后"。
    auto first_backend = std::make_shared<FakeBackend>();
    first_backend->reply = "{\"who\": \"a-first\"}";
    auto first_loader = std::make_shared<CountingLoader>();
    LlmExecutor::Options first_llm_options;
    first_llm_options.backend = first_backend.get();
    first_llm_options.model = "fake-model";
    first_llm_options.prompt_loader = first_loader->loader();
    auto first_llm = std::make_shared<LlmExecutor>(std::move(first_llm_options));
    auto opened = OpenLedger(root);
    REQUIRE(opened.has_value());
    auto ledger = std::make_unique<lubancode::runtime::TrajectorySessionLedger>(std::move(*opened));

    RuntimeOptions first_options;
    first_options.executors[NodeKind::Llm] = first_llm;
    first_options.trajectory_ledger = ledger.get();
    first_options.account_root = root / "workflow-runs";
    first_options.run_id_generator = [] { return "run-ns"; };
    int writes = 0;
    first_options.account_fault = [&writes] {
        ++writes;
        // 账序:definition.loaded / inputs / a.reserved / a.dispatched /
        // a.output.committed(5)/ a.node.completed(6)——第 6 枚注入,commit
        // 已落、收口没落,run 停在明确失败态(等价 kill 窗口)。
        return writes == 6 ? std::optional<std::string>("test.injected") : std::nullopt;
    };
    const auto crashed = WorkflowRuntime(std::move(first_options)).Run(
        def, RunInputs{nlohmann::json{{"topic", "账"}}});
    REQUIRE(crashed.state == RunState::Failed);
    CHECK(crashed.error_code == "orchestration_ledger_broken");

    // 第二幕:resume。a 的 commit 在账上——补内存不重执行、不重开场;b 续跑。
    auto second_backend = std::make_shared<FakeBackend>();
    second_backend->reply = "{\"who\": \"a-second\"}";
    auto second_loader = std::make_shared<CountingLoader>();
    LlmExecutor::Options second_llm_options;
    second_llm_options.backend = second_backend.get();
    second_llm_options.model = "fake-model";
    second_llm_options.prompt_loader = second_loader->loader();
    auto second_llm = std::make_shared<LlmExecutor>(std::move(second_llm_options));
    RuntimeOptions resume_options;
    resume_options.executors[NodeKind::Llm] = second_llm;
    resume_options.trajectory_ledger = ledger.get();
    WorkflowRuntime resumer(std::move(resume_options));
    const auto resumed = resumer.Resume(root / "workflow-runs" / "run-ns");
    REQUIRE(resumed.has_value());
    REQUIRE(resumed->state == RunState::Succeeded);

    // a 不重执行(prompt loader 零调用)、不重开场(nodes/run-ns-a-d1 仍一场)。
    CHECK(second_loader->calls["prompts/a.md"] == 0);
    CHECK(second_loader->calls["prompts/b.md"] == 1);
    CHECK(CountSessionDirs(root / "workflow-runs" / "run-ns" / "nodes" / "run-ns-a-d1") == 1);
    CHECK(CountSessionDirs(root / "workflow-runs" / "run-ns" / "nodes" / "run-ns-b-d2") == 1);
    // 产物沿用崩溃前那份("a-first" 非 "a-second")。
    CHECK(resumed->result.at("a").at("who") == "a-first");
    // 恢复段在场(seg-2),账不换号。
    CHECK(fs::exists(root / "workflow-runs" / "run-ns" / "segments" / "seg-2"));
    CHECK(resumed->run_id == "run-ns");

    // /usage:崩溃前后两幕的场都在树上(a 一场、b 一场),kill 掉的 a 场
    // 照计真实花费。
    const auto usage = lubancode::accounting::ReadSessionUsage(ledger->session_dir());
    REQUIRE(usage.ok);
    int node_samples = 0;
    for (const auto& sample : usage.samples) {
        if (sample.session_id != ledger->session_id()) {
            ++node_samples;
        }
    }
    if (node_samples != 2) {
        // 诊断(只在失配时打):树走警告 + 样本身份 + b 场账卷的行角色,
        // 下轮 CI 日志直接看穿缺哪环。
        for (const auto& warning : usage.warnings) {
            MESSAGE("usage-warning: ", warning);
        }
        for (const auto& sample : usage.samples) {
            MESSAGE("usage-sample: sid=", sample.session_id, " run=", sample.run_id,
                    " in=", sample.total_input_tokens);
        }
        const fs::path b_dir = root / "workflow-runs" / "run-ns" / "nodes" / "run-ns-b-d2" /
                               "sessions";
        std::error_code list_ec;
        for (const auto& entry : fs::directory_iterator(b_dir, list_ec)) {
            const fs::path b_jsonl = entry.path() / (entry.path().filename().string() + ".jsonl");
            MESSAGE("b-session-file: ", b_jsonl.string());
            for (const auto& line : LinesOf(b_jsonl)) {
                if (!line.is_object()) continue;
                const std::string role =
                    line.value("type", std::string()) == "message"
                        ? line.at("message").value("role", std::string())
                        : line.value("kind", std::string());
                MESSAGE("  line: ", role);
            }
        }
        for (const auto& line : LinesOf(ledger->session_dir() /
                                        (ledger->session_id() + ".jsonl"))) {
            if (line.is_object() &&
                line.value("kind", std::string()) == "subagent.spawn.requested") {
                MESSAGE("spawn-edge: ",
                        line.at("payload").at("childSessionRef").value("journalPath",
                                                                       std::string()));
            }
        }
    }
    CHECK(node_samples == 2);
}

TEST_CASE("GAP-05 案3:重试新开一场,attempt 递增入账") {
    EnvGuard v3_pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const fs::path root = FreshDir("retry");
    const char* yaml = R"YAML(
schema_version: 1
id: ns-retry
version: 1.0.0
entry: a
nodes:
  a:
    type: llm
    prompt: prompts/a.md
    retry: { attempts: 2, when: [api_error], backoff: fixed, initial: 0s, max: 0s }
  fin: { type: end }
edges:
  - { from: a, on: success, to: fin }
result:
  a: "${nodes.a.output}"
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);
    auto opened = OpenLedger(root);
    REQUIRE(opened.has_value());
    auto ledger = std::make_unique<lubancode::runtime::TrajectorySessionLedger>(std::move(*opened));

    auto backend = std::make_shared<FailingBackend>();
    auto loader = std::make_shared<CountingLoader>();
    LlmExecutor::Options llm_options;
    llm_options.backend = backend.get();
    llm_options.model = "fake-model";
    llm_options.prompt_loader = loader->loader();
    auto llm = std::make_shared<LlmExecutor>(std::move(llm_options));

    RuntimeOptions options;
    options.executors[NodeKind::Llm] = llm;
    options.trajectory_ledger = ledger.get();
    options.account_root = root / "workflow-runs";
    options.run_id_generator = [] { return "run-ns"; };
    const WorkflowRunSummary summary =
        WorkflowRuntime(std::move(options)).Run(def, RunInputs{});
    REQUIRE(summary.state == RunState::Succeeded);
    REQUIRE(backend->calls == 2);

    // 同一 nodeExecution 两个 attempt:两场独立 session,各自收口。
    const fs::path exec_dir = root / "workflow-runs" / "run-ns" / "nodes" / "run-ns-a-d1";
    CHECK(CountSessionDirs(exec_dir) == 2);
    // 账上有 retrying(编排事实,非终态)。
    bool saw_retrying = false;
    for (const auto& event : AccountEvents(root / "workflow-runs" / "run-ns", "seg-1")) {
        if (event.is_object() && event.value("kind", std::string()) == "workflow.node.retrying") {
            saw_retrying = true;
            CHECK(event.at("payload").value("nodeExecutionId", std::string()) == "run-ns-a-d1");
        }
    }
    CHECK(saw_retrying);
}

TEST_CASE("GAP-05 案4:非模型节点零伪造——纯 transform 图不开任何场") {
    EnvGuard v3_pin("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const fs::path root = FreshDir("pure");
    const char* yaml = R"YAML(
schema_version: 1
id: ns-pure
version: 1.0.0
entry: a
nodes:
  a: { type: transform, operation: echo }
  fin: { type: end }
edges:
  - { from: a, on: success, to: fin }
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);
    auto opened = OpenLedger(root);
    REQUIRE(opened.has_value());
    auto ledger = std::make_unique<lubancode::runtime::TrajectorySessionLedger>(std::move(*opened));

    class EchoExec : public NodeExecutor {
    public:
        NodeExecResult Execute(const NodeExecRequest& request) override {
            (void)request;
            NodeExecResult result;
            result.ok = true;
            result.output = nlohmann::json{{"echo", true}};
            return result;
        }
    };
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = std::make_shared<EchoExec>();
    options.trajectory_ledger = ledger.get();
    options.account_root = root / "workflow-runs";
    options.run_id_generator = [] { return "run-ns"; };
    const WorkflowRunSummary summary = WorkflowRuntime(std::move(options)).Run(def, RunInputs{});
    REQUIRE(summary.state == RunState::Succeeded);

    // 零场:nodes/ 无任何 session;父账零 spawn 边。
    std::error_code ec;
    int session_dirs = 0;
    for (const auto& entry : fs::recursive_directory_iterator(
             root / "workflow-runs" / "run-ns" / "nodes", ec)) {
        if (entry.is_directory(ec) && entry.path().filename() == "sessions") {
            ++session_dirs;
        }
    }
    CHECK(session_dirs == 0);
    const std::string parent_sid = ledger->session_id();
    for (const auto& line : LinesOf(ledger->session_dir() / (parent_sid + ".jsonl"))) {
        if (line.is_object()) {
            CHECK(line.value("kind", std::string()) != "subagent.spawn.requested");
        }
    }
}
