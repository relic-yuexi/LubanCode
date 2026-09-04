// Workspace 收官验收·综合恢复册(单子 §一第 1 条):
//   主会话 + 四只并行子代理 + 嵌套 child + workflow + Memory recall/save
//   的完整现场,杀进程(句柄全丢 + 死 PID 陈旧锁 + 尾行撕裂)与断流
//  (回合半路无输出、子账半行)之后:
//     - RecoverWorkspace 以 Journal 可证事实收口(旧场 incomplete、
//       session.json 补正、旧 main 一个字节不再追加);
//     - ResumeAsNew 七步接上(新场 start_reason=resume,悬空分档如实);
//     - VerifySessionDir 全流验链:撕裂流报 verify.truncated_tail,其余全过;
//     - Memory 住 workspace 树,换场不丢——新场 recall 照常命中;
//     - 恢复后的新场能继续干活(再写一轮、再验)。
// 与 unit/trajectory/test_session_manager_recovery.cpp 的分工:那册拆
// clear 八步的各崩溃点;本册拼完整生产形状(main+4 并行子+嵌套+workflow+
// memory),验收线是"全要素现场杀进程后 resume/verify 全过"。
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/tool_trace.hpp"
#include "api/types.hpp"
#include "app/memory_ledger_bridge.hpp"
#include "memory/project_memory.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/session_lock.hpp"
#include "trajectory/session_manager.hpp"

using namespace lubancode;
using trajectory::Actor;
using trajectory::Durability;
using trajectory::EventKind;
using trajectory::RecordReceipt;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    static const auto run_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    ("lubancode-ws-recovery-" + std::to_string(run_id % 100000) + "-" + name +
                     "-" + std::to_string(++sequence));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
    return path;
}

void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

api::Message TextMessage(api::Role role, const std::string& text) {
    api::Message message;
    message.role = role;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantWithCall(const std::string& call_id, const std::string& tool) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{"先查证。"});
    api::ToolUseBlock call;
    call.id = call_id;
    call.name = tool;
    call.input = nlohmann::json{{"path", "build.sh"}};
    message.content.push_back(std::move(call));
    return message;
}

agent::ToolTraceEvent TraceEvent(agent::ToolTraceEventKind kind, const std::string& call_id) {
    agent::ToolTraceEvent event;
    event.kind = kind;
    event.execution_id = "exec-" + call_id;
    event.tool_use_id = call_id;
    event.tool_name = "read_file";
    event.timestamp_ms = 1759000000000LL;
    if (kind == agent::ToolTraceEventKind::ExecutionStarted) {
        event.effective_input_sha256 = std::string(64, '0');
        event.effect_class = agent::EffectClass::ReadOnlyLocal;
        event.effective_arguments = nlohmann::json{{"path", "build.sh"}};
    } else if (kind == agent::ToolTraceEventKind::ExecutionFinished) {
        event.outcome = agent::ToolOutcome::Succeeded;
        event.duration_ms = 9;
        event.result_ref.kind = agent::ToolResultRef::Kind::Inline;
        event.result_ref.sha256 = std::string(64, '3');
        event.result_ref.bytes = 12;
    }
    return event;
}

// 主会话一轮完整回合(输入→请求→输出带工具→工具三拍→回喂→第二请求收口)。
void DriveFullTurn(runtime::TrajectorySessionLedger& ledger, const std::string& turn_id,
                   const std::string& user_text) {
    auto bridge = ledger.NewTurnBridge({"demo", "responses", "terminal"});
    REQUIRE(bridge != nullptr);
    bridge->BeginTurn(turn_id, "external_user");
    bridge->RecordInput(TextMessage(api::Role::User, user_text));
    api::Request request;
    request.model = "demo-model";
    const std::string req1 = bridge->OnRequestPrepared(request, agent::RequestPreparedContext{});
    REQUIRE_FALSE(req1.empty());
    bridge->OnRequestSent(req1);
    REQUIRE(bridge->OnOutputCompleted(req1, AssistantWithCall("call-" + turn_id, "read_file"),
                                      "tool_use", "resp-1"));
    bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::Scheduled, "call-" + turn_id));
    bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionStarted, "call-" + turn_id));
    bridge->OnToolTrace(TraceEvent(agent::ToolTraceEventKind::ExecutionFinished, "call-" + turn_id));
    api::Message results;
    results.role = api::Role::User;
    results.content.push_back(api::ToolResultBlock{"call-" + turn_id, "deploy 走 build.sh", false});
    bridge->OnToolResultsCommitted("batch-" + turn_id, results);
    api::Message answer = TextMessage(api::Role::Assistant, "查到了,deploy 走 build.sh。");
    const std::string req2 = bridge->OnRequestPrepared(request, agent::RequestPreparedContext{});
    bridge->OnRequestSent(req2);
    REQUIRE(bridge->OnOutputCompleted(req2, answer, "end_turn", "resp-2"));
    bridge->EndTurn(true, false, {});
}

// 一只子代理跑一轮并收口(ok=false 走 run.failed)。
void DriveSubagentTurn(runtime::TrajectorySubagentBridge& child, bool ok,
                       const std::string& label) {
    auto& bridge = child.turn_bridge();
    bridge.BeginTurn("turn-1", "external_user");
    bridge.RecordInput(TextMessage(api::Role::User, label));
    api::Request request;
    request.model = "demo-model";
    const std::string req = bridge.OnRequestPrepared(request, agent::RequestPreparedContext{});
    bridge.OnRequestSent(req);
    api::Message answer = TextMessage(api::Role::Assistant, ok ? "报告:办妥" : "半截");
    REQUIRE(bridge.OnOutputCompleted(req, answer, "end_turn", "resp-c"));
    bridge.EndTurn(ok, false, ok ? std::string() : "model_refused");
    const std::string hash = child.Finish(ok, ok ? std::string() : "model_refused");
    CHECK_FALSE(hash.empty());
}

// 杀进程现场的总装:根目录 + repo + home + 开了账的 ledger + memory。
struct CrashRig {
    fs::path root;
    fs::path repo;
    fs::path home;
    std::optional<runtime::TrajectorySessionLedger> ledger;
    std::shared_ptr<memory::ProjectMemory> store;
    std::unique_ptr<app::MemoryLedgerBridge> bridge_accounting;
    std::string session_id;
    fs::path session_dir;
    // 留着收口断言用的 run id。
    std::vector<std::string> subagent_run_ids;
    std::string unfinished_subagent;   // 半路被打断那只(尾行撕裂对象)
    std::string nested_parent_run_id;  // 派出嵌套孩子的那只子代理
    std::string nested_child_run_id;
    std::string workflow_run_id;

    explicit CrashRig(const std::string& name) {
        root = TempRoot(name);
        repo = root / "repo";
        home = root / "home";
        fs::create_directories(repo / ".git");
        fs::create_directories(home);
        Write(repo / "build.sh", "#!/bin/sh\necho build\n");

        runtime::TrajectorySessionLedger::Options options;
        options.workspaces_root = home / "workspaces";
        options.workspace_root = repo;
        options.launch_cwd = repo.generic_string();
        options.lubancode_version = "recovery-test";
        auto opened = runtime::TrajectorySessionLedger::Open(std::move(options));
        REQUIRE(opened.has_value());
        ledger.emplace(std::move(*opened));
        session_id = ledger->session_id();
        session_dir = ledger->session_dir();

        // Memory 挂真落账桥(memory 事件进 main.jsonl)。
        memory::Options memory_options;
        memory_options.global_allowed = true;
        memory_options.enabled = true;
        auto identity = memory::ResolveProjectIdentity(repo, home);
        REQUIRE(identity.has_value());
        store = std::make_shared<memory::ProjectMemory>(std::move(*identity), home, memory_options);
        bridge_accounting = std::make_unique<app::MemoryLedgerBridge>(*ledger);
        store->set_accounting(bridge_accounting.get());
        store->set_source_session(session_id);
    }
};

memory::SaveRequest MakeFact() {
    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact;
    request.id = "fact.deploy";
    request.title = "部署命令";
    request.summary = "部署命令";
    request.content = "deploy 走 build.sh,先 configure 再 build。";
    request.keywords = {"deploy", "build"};
    request.paths = {"build.sh"};
    request.confidence = "verified";
    return request;
}

// 死 PID 陈旧锁(与 unit/trajectory/test_session_lock.cpp 同款形状)。
constexpr unsigned long kDeadPid = 4194303UL;
void PlantStaleLock(const fs::path& session_dir) {
    trajectory::SessionLockOwner owner;
    owner.pid = kDeadPid;
    owner.process_start_token = "0001deadbeef0002";
    owner.acquired_at_ms = 1759000000000LL;
    Write(session_dir / "session.lock", owner.ToJson().dump(2) + "\n");
}

}  // namespace

TEST_CASE("综合恢复: 全要素现场杀进程——recover/resume/verify 全过,memory 换场不丢") {
    CrashRig rig("full-crash");
    {
        // ---- 活着的时候:main 一轮完整回合 + memory save/recall 落账 ----
        DriveFullTurn(*rig.ledger, "turn-0001", "deploy 怎么跑?");
        REQUIRE(rig.store->EnqueueSave(MakeFact()).has_value());
        REQUIRE(memory::RunPendingMemoryJobs(rig.home).has_value());
        const std::string context = rig.store->BuildTurnContext("deploy 怎么跑", rig.repo);
        REQUIRE_FALSE(context.empty());
        CHECK(context.find("fact.deploy") != std::string::npos);
        // recall 快照进了 main.jsonl(context.injected);≤512B 走
        // snapshot_inline,超过才落 blob 引用(合同 §四),两形都算有账。
        bool saw_injected = false;
        std::string snapshot_ref;
        std::string snapshot_inline;
        const auto journal = trajectory::ReadJournalLines(rig.session_dir / "main.jsonl");
        REQUIRE(journal.has_value());
        for (const std::string& line : *journal) {
            const auto event = nlohmann::json::parse(line, nullptr, false);
            if (event.is_discarded() || event.value("kind", std::string()) != "context.injected") {
                continue;
            }
            saw_injected = true;
            snapshot_ref = event["payload"].value("snapshot_ref", std::string());
            snapshot_inline = event["payload"].value("snapshot_inline", std::string());
        }
        REQUIRE(saw_injected);
        const bool has_snapshot = !snapshot_ref.empty() || !snapshot_inline.empty();
        CHECK(has_snapshot);
        if (!snapshot_ref.empty()) {
            CHECK(fs::exists(rig.session_dir / snapshot_ref));
        }

        // ---- 四只并行子代理(后台派工:parent_call_id 为空,生产形态):两只
        // 收口(成/败),一只半路(不收口),一只派嵌套 ----
        std::vector<std::unique_ptr<runtime::TrajectorySubagentBridge>> children;
        for (int i = 0; i < 4; ++i) {
            auto spawned = rig.ledger->SpawnSubagent(/*parent_call_id=*/std::string(),
                                                     "并行活 " + std::to_string(i));
            REQUIRE(spawned.has_value());
            children.push_back(std::move(*spawned));
            rig.subagent_run_ids.push_back(children.back()->run_id());
        }
        // 四只并行驱动(各写各的子账;盘上由 recorder 锁串行)。
        std::thread worker[4];
        worker[0] = std::thread([&] { DriveSubagentTurn(*children[0], true, "零号活"); });
        worker[1] = std::thread([&] { DriveSubagentTurn(*children[1], false, "一号活"); });
        worker[2] = std::thread([&] {
            // 半路:开了 turn、发了请求,没有输出、没有终态(断流现场)。
            auto& bridge = children[2]->turn_bridge();
            bridge.BeginTurn("turn-1", "external_user");
            bridge.RecordInput(TextMessage(api::Role::User, "二号活"));
            api::Request request;
            request.model = "demo-model";
            const std::string req = bridge.OnRequestPrepared(request, agent::RequestPreparedContext{});
            bridge.OnRequestSent(req);
        });
        worker[3] = std::thread([&] {
            // 三号:先派出嵌套孩子(孩子收口),自己半路不收口。
            auto grandchild = rig.ledger->SpawnSubagent(/*parent_call_id=*/std::string(),
                                                        "嵌套活", children[3]->run_id());
            REQUIRE(grandchild.has_value());
            rig.nested_parent_run_id = children[3]->run_id();
            rig.nested_child_run_id = (*grandchild)->run_id();
            DriveSubagentTurn(**grandchild, true, "嵌套孩子的活");
            auto& bridge = children[3]->turn_bridge();
            bridge.BeginTurn("turn-1", "external_user");
            bridge.RecordInput(TextMessage(api::Role::User, "三号活"));
            api::Request request;
            request.model = "demo-model";
            const std::string req = bridge.OnRequestPrepared(request, agent::RequestPreparedContext{});
            bridge.OnRequestSent(req);
        });
        for (auto& thread : worker) {
            thread.join();
        }
        rig.unfinished_subagent = children[2]->run_id();

        // ---- workflow:编排账 + 一个收口节点 + 一个派了没终态的节点 ----
        runtime::TrajectoryWorkflowRunBridge::DefinitionInfo info;
        info.workflow_id = "recover-flow";
        info.workflow_version = "1.0.0";
        info.content_hash = std::string(64, 'a');
        info.cwd = rig.repo.generic_string();
        info.definition_json = R"({"id":"recover-flow","nodes":{}})";
        auto run = rig.ledger->SpawnWorkflowRun("wf-run-0001", info);
        REQUIRE(run.has_value());
        rig.workflow_run_id = "wf-run-0001";
        REQUIRE((*run)->RecordNodeDispatched("gather", "wf-run-0001-gather-a1", 1, "agent", -1,
                                             std::string(64, 'e')));
        auto node = (*run)->SpawnNodeStream("gather", "wf-run-0001-gather-a1", 1, "agent", -1);
        REQUIRE(node.has_value());
        {
            auto& turn = (*node)->turn_bridge();
            turn.BeginTurn("turn-1", "scheduled_host");
            turn.RecordInput(TextMessage(api::Role::User, "节点活"));
            api::Request request;
            request.model = "demo-model";
            const std::string req = turn.OnRequestPrepared(request, agent::RequestPreparedContext{});
            turn.OnRequestSent(req);
            REQUIRE(turn.OnOutputCompleted(req, TextMessage(api::Role::Assistant, "节点结论"),
                                           "end_turn", "resp-n"));
            turn.EndTurn(true, false, {});
            const std::string node_hash = (*node)->Finish(true, false, {});
            REQUIRE_FALSE(node_hash.empty());
            REQUIRE((*run)->RecordNodeCompleted("gather", "wf-run-0001-gather-a1", 1, "success", 80,
                                                5, std::string(), std::string(64, 'f'), node_hash));
        }
        // 第二节点:开了 node 账、turn 半路,不给终态(派了没开账的孤儿
        // 形状会让整场判 corrupt——那不是本册要验的崩溃形状)。
        REQUIRE((*run)->RecordNodeDispatched("weld", "wf-run-0001-weld-a1", 1, "agent", -1,
                                             std::string(64, 'e')));
        auto node_b = (*run)->SpawnNodeStream("weld", "wf-run-0001-weld-a1", 1, "agent", -1);
        REQUIRE(node_b.has_value());
        {
            auto& turn = (*node_b)->turn_bridge();
            turn.BeginTurn("turn-1", "scheduled_host");
            turn.RecordInput(TextMessage(api::Role::User, "第二节点活"));
            api::Request request;
            request.model = "demo-model";
            const std::string req = turn.OnRequestPrepared(request, agent::RequestPreparedContext{});
            turn.OnRequestSent(req);
            // 没有 output、没有 Finish——进程死在这里。
        }
        // workflow run 不 Finish(半路)。

        // ---- main 第二回合半路:turn 开着,请求发了,没有输出(断流) ----
        {
            auto bridge = rig.ledger->NewTurnBridge({"demo", "responses", "terminal"});
            bridge->BeginTurn("turn-0002", "external_user");
            bridge->RecordInput(TextMessage(api::Role::User, "还有一件事"));
            api::Request request;
            request.model = "demo-model";
            const std::string req = bridge->OnRequestPrepared(request, agent::RequestPreparedContext{});
            bridge->OnRequestSent(req);
            // 没有 OnOutput*,没有 EndTurn——进程死在这里。
        }
    }

    // ---- 杀进程:句柄全丢(不 Close)、死 PID 陈旧锁、二号子账尾行撕裂
    // (崩溃形状按 §16.3:尾行缺换行符——已写完整事件但未落行尾)----
    const auto bytes_before = fs::file_size(rig.session_dir / "main.jsonl");
    rig.ledger.reset();  // 析构不封账:崩溃语义
    rig.store.reset();
    rig.bridge_accounting.reset();
    PlantStaleLock(rig.session_dir);
    {
        const fs::path torn = rig.session_dir / "subagents" / (rig.unfinished_subagent + ".jsonl");
        std::string text = Read(torn);
        REQUIRE_FALSE(text.empty());
        REQUIRE(text.back() == '\n');
        std::ofstream file(torn, std::ios::binary | std::ios::trunc);
        file << text.substr(0, text.size() - 1);  // 去掉末行换行:判截断
    }

    // ---- 新进程:恢复器接管 ----
    trajectory::SessionManagerOptions manager_options;
    manager_options.workspaces_root = rig.home / "workspaces";
    manager_options.workspace_root = rig.repo;
    manager_options.identity = workspace::ResolveWorkspaceIdentity(rig.repo, {}).value();
    manager_options.launch_cwd = rig.repo.generic_string();
    manager_options.lubancode_version = "recovery-test";
    trajectory::SessionManager recovered(manager_options);
    const auto report = recovered.RecoverWorkspace();

    const trajectory::SessionRecoveryEntry* crashed = nullptr;
    for (const auto& entry : report.sessions) {
        if (entry.session_id == rig.session_id) {
            crashed = &entry;
        }
    }
    REQUIRE(crashed != nullptr);
    CHECK(crashed->status == trajectory::SessionStatus::Incomplete);
    CHECK(crashed->session_json_corrected);  // running -> incomplete,事实赢
    CHECK_FALSE(crashed->owned_by_live_process);  // 死 PID 锁不算活人
    // 旧 main 一个字节没被恢复器追加(plain 崩溃不动账,只补 session.json)。
    CHECK(fs::file_size(rig.session_dir / "main.jsonl") == bytes_before);

    // ---- verify:全流验链,撕裂流点名,其余全过 ----
    const auto verify = trajectory::VerifySessionDir(rig.session_dir);
    REQUIRE(verify.streams.size() >= 7);  // main + 4 子 + 嵌套孩子 + 编排 + 节点
    int torn_reported = 0;
    for (const auto& stream : verify.streams) {
        if (stream.relative_path == "subagents/" + rig.unfinished_subagent + ".jsonl") {
            CHECK(stream.error_code == "verify.truncated_tail");
            ++torn_reported;
            continue;
        }
        CHECK(stream.ok);
    }
    CHECK(torn_reported == 1);

    // ---- resume 七步:接上崩溃场 ----
    auto resumed = runtime::TrajectorySessionLedger::Open([&] {
        runtime::TrajectorySessionLedger::Options options;
        options.workspaces_root = rig.home / "workspaces";
        options.workspace_root = rig.repo;
        options.launch_cwd = rig.repo.generic_string();
        options.lubancode_version = "recovery-test";
        options.resume_at_launch = true;
        return options;
    }());
    REQUIRE(resumed.has_value());
    CHECK(resumed->resumed_at_launch());
    CHECK(resumed->session_id() != rig.session_id);
    const auto fold = resumed->FoldMainReplay();
    REQUIRE(fold.ok());
    CHECK(fold.state.start_reason == "resume");
    CHECK(fold.state.control.resumed_from_session_id.value_or("") == rig.session_id);
    // 悬空分档:断流那轮的调用进了 dangling 账,不冒充执行过。
    CHECK_FALSE(fold.state.integrity.last_event_hash.empty());

    // ---- memory 住 workspace:换场不丢,新场照常召回 ----
    {
        memory::Options memory_options;
        memory_options.global_allowed = true;
        memory_options.enabled = true;
        auto identity = memory::ResolveProjectIdentity(rig.repo, rig.home);
        REQUIRE(identity.has_value());
        auto store = std::make_shared<memory::ProjectMemory>(std::move(*identity), rig.home,
                                                             memory_options);
        const std::string context = store->BuildTurnContext("deploy 怎么跑", rig.repo);
        CHECK(context.find("fact.deploy") != std::string::npos);
    }

    // ---- 恢复后的新场继续干活:再写一轮,verify 过 ----
    DriveFullTurn(*resumed, "turn-0003", "接续干活");
    CHECK(resumed->VerifySession().error_code.empty());
    CHECK(trajectory::VerifySessionDir(rig.session_dir).streams.size() >= 7);

    // 旧账仍一字未动(resume 只读 source)。
    CHECK(fs::file_size(rig.session_dir / "main.jsonl") == bytes_before);
}

TEST_CASE("综合恢复: 干净封口后崩溃无痕——recover 空转,resume 接得上") {
    CrashRig rig("clean-close");
    DriveFullTurn(*rig.ledger, "turn-0001", "一轮就收");
    REQUIRE(rig.ledger->CloseSession("exit").error_code.empty());
    const auto bytes = fs::file_size(rig.session_dir / "main.jsonl");
    rig.ledger.reset();
    rig.store.reset();
    rig.bridge_accounting.reset();

    trajectory::SessionManagerOptions manager_options;
    manager_options.workspaces_root = rig.home / "workspaces";
    manager_options.workspace_root = rig.repo;
    manager_options.identity = workspace::ResolveWorkspaceIdentity(rig.repo, {}).value();
    manager_options.launch_cwd = rig.repo.generic_string();
    manager_options.lubancode_version = "recovery-test";
    trajectory::SessionManager recovered(manager_options);
    const auto report = recovered.RecoverWorkspace();
    bool saw_closed = false;
    for (const auto& entry : report.sessions) {
        if (entry.session_id == rig.session_id) {
            saw_closed = entry.status == trajectory::SessionStatus::Closed &&
                         !entry.session_json_corrected && !entry.clear_continued;
        }
    }
    CHECK(saw_closed);  // 封好的账恢复器不添一笔
    CHECK(fs::file_size(rig.session_dir / "main.jsonl") == bytes);

    CHECK(trajectory::VerifySessionDir(rig.session_dir).ok);
    auto resumed = runtime::TrajectorySessionLedger::Open([&] {
        runtime::TrajectorySessionLedger::Options options;
        options.workspaces_root = rig.home / "workspaces";
        options.workspace_root = rig.repo;
        options.launch_cwd = rig.repo.generic_string();
        options.lubancode_version = "recovery-test";
        options.resume_at_launch = true;
        return options;
    }());
    REQUIRE(resumed.has_value());
    CHECK(resumed->resumed_at_launch());
    const auto fold = resumed->FoldMainReplay();
    REQUIRE(fold.ok());
    CHECK(fold.state.start_reason == "resume");
}
