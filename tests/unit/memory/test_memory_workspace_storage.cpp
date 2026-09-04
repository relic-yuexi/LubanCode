// 存储 v2 P0-3:项目 Memory 搬进 Workspace 的域内账。
//
// 盖五件事:
//   1. 记忆根住 <home>/workspaces/<key>/memory/,candidates 同树;全程不在
//      <home>/projects/ 新建一个文件(验收线)。
//   2. source refs 全限定(workspace/session/run/event),不再落裸 session id。
//   3. 召回快照经落账口出账:字段齐、指纹对得上;快照落不稳时本轮不注入
//      该条,trace 记 snapshot_failed(§9.2"不得注了却无账")。
//   4. 派工召回走冻结口:target_run_id 进记录,子代理路径与主路分开。
//   5. worker 回执进 workspace lifecycle(intent+result),重复 commit 幂等;
//      指旧 projects/ 的存量 job 拒办挪 failed,旧树零写入。
//   6. recall trace 落 .state/recall-traces/,键名 workspace_key。
//
// 桥到真 Trajectory 账的端到端(事件进 main.jsonl、artifact 可读回)在
// tests/unit/app/test_memory_ledger_bridge.cpp。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "memory/project_memory.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    static const auto run_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    ("lubancode-mem-ws-" + std::to_string(run_id) + "-" + name + "-" +
                     std::to_string(++sequence));
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

// 落账口的记账桩:抓记录、按需制造快照失败。
class RecordingAccounting final : public memory::MemoryAccounting {
public:
    std::expected<void, std::string> RecordRecallInjection(
        const memory::InjectedMemoryRecord& record) override {
        if (fail_snapshot) return std::unexpected("memory.recall_snapshot_failed: 测试注入失败");
        injections.push_back(record);
        return {};
    }

    std::string RecordSaveRequested(const memory::SaveLedgerNote& note) override {
        saves.push_back(note);
        return "workspace_key=" + workspace_key + "/session_id=s-1/run_id=r-1/event_id=r-1:evt-00000042";
    }

    std::string workspace_key;
    bool fail_snapshot = false;
    std::vector<memory::InjectedMemoryRecord> injections;
    std::vector<memory::SaveLedgerNote> saves;
};

struct Rig {
    fs::path root;
    fs::path repo;
    fs::path home;
    std::shared_ptr<memory::ProjectMemory> store;
    std::shared_ptr<RecordingAccounting> accounting;

    explicit Rig(const std::string& name) {
        root = TempRoot(name);
        repo = root / "repo";
        home = root / "home";
        fs::create_directories(repo / ".git");
        fs::create_directories(home);
        Write(repo / "build.sh", "#!/bin/sh\necho build\n");
        memory::Options options;
        options.global_allowed = true;
        options.enabled = true;
        auto identity = memory::ResolveProjectIdentity(repo, home);
        REQUIRE(identity.has_value());
        accounting = std::make_shared<RecordingAccounting>();
        accounting->workspace_key = identity->workspace_key;
        store = std::make_shared<memory::ProjectMemory>(std::move(*identity), home, options);
        store->set_accounting(accounting.get());
    }
};

memory::SaveRequest MakeFact(const std::string& id, const std::string& title,
                             const std::string& content) {
    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact;
    request.id = id;
    request.title = title;
    request.summary = title;
    request.content = content;
    request.keywords = {"deploy"};
    request.paths = {"build.sh"};
    request.confidence = "verified";
    return request;
}

}  // namespace

TEST_CASE("P0-3: 项目记忆根住 workspace,全程不在 projects/ 新建文件") {
    Rig rig("root-move");
    REQUIRE(rig.store->EnqueueSave(MakeFact("fact.deploy", "部署命令", "deploy 走 build.sh。")).has_value());
    REQUIRE(memory::RunPendingMemoryJobs(rig.home).has_value());

    const auto& identity = rig.store->identity();
    CHECK(fs::exists(identity.workspace_dir / "workspace.json"));
    CHECK(fs::exists(rig.store->memory_dir() / "facts" / "deploy.md"));
    CHECK(fs::exists(rig.store->memory_dir() / "memory-candidates") == false);  // 没候选就不建目录
    // 验收线:<home>/projects/ 一个文件都不许出现。
    std::error_code ec;
    CHECK_FALSE(fs::exists(rig.home / "projects", ec));
}

TEST_CASE("P0-3: source refs 全限定,事件引用进主题") {
    Rig rig("full-ref");
    REQUIRE(rig.store->EnqueueSave(MakeFact("fact.deploy", "部署命令", "deploy 走 build.sh。")).has_value());
    REQUIRE(memory::RunPendingMemoryJobs(rig.home).has_value());

    // 落账口拦到了 save 申报,回的全限定引用写进了主题的 source_sessions。
    REQUIRE(rig.accounting->saves.size() == 1);
    CHECK(rig.accounting->saves[0].operation == "upsert");
    CHECK(rig.accounting->saves[0].layer == "project");
    const auto entries = rig.store->ListEntries();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].source_sessions.size() == 1);
    const std::string& ref = entries[0].source_sessions[0];
    CHECK(ref.starts_with("workspace_key=" + rig.store->identity().workspace_key + "/session_id=s-1/"));
    CHECK(ref.find("run_id=r-1/event_id=r-1:evt-") != std::string::npos);
}

TEST_CASE("P0-3: 召回快照经落账口出账,快照不稳本轮不注入") {
    Rig rig("snapshot");
    REQUIRE(rig.store->EnqueueSave(MakeFact("fact.deploy", "部署命令", "deploy 走 build.sh。")).has_value());
    REQUIRE(memory::RunPendingMemoryJobs(rig.home).has_value());

    const std::string context = rig.store->BuildTurnContext("deploy 怎么跑", rig.repo);
    REQUIRE_FALSE(context.empty());
    REQUIRE(rig.accounting->injections.size() == 1);
    const auto& record = rig.accounting->injections[0];
    CHECK(record.memory_id == "fact.deploy");
    CHECK(record.memory_level == "project");
    CHECK(record.memory_schema == 3);
    CHECK(record.injected_bytes == record.content.size());
    CHECK(record.content_sha256 == hooks::Sha256Hex(record.content));
    CHECK(record.target_run_id.empty());
    CHECK(context.find("fact.deploy") != std::string::npos);

    // 快照落不稳:该条不注入,trace 记 snapshot_failed。
    rig.accounting->fail_snapshot = true;
    rig.accounting->injections.clear();
    const std::string blocked = rig.store->BuildTurnContext("deploy 怎么跑", rig.repo);
    CHECK(blocked.empty());
    CHECK(rig.accounting->injections.empty());
    const auto trace = rig.store->LastTrace();
    REQUIRE(trace.valid);
    REQUIRE(trace.entries.size() == 1);
    CHECK(trace.entries[0].snapshot_failed);
    CHECK_FALSE(trace.entries[0].injected);
    CHECK(trace.workspace_key == rig.store->identity().workspace_key);
    // trace 落在新家:.state/recall-traces/trace-last.json,键名 workspace_key。
    // schema 4(记忆检索与注入改进单):条目加 content_hits/content_truncated/
    // drop_reason;schema 5(记忆幻觉根治单):条目加 weak/cooccur/weak_dropped。
    const auto trace_json = nlohmann::json::parse(
        Read(rig.store->memory_dir() / ".state" / "recall-traces" / "trace-last.json"));
    CHECK(trace_json.value("schema", 0) == 5);
    CHECK(trace_json.value("workspace_key", std::string()) == rig.store->identity().workspace_key);
    CHECK(trace_json.contains("project_key") == false);
}

TEST_CASE("P0-3: 派工召回走冻结口,target_run_id 进记录") {
    Rig rig("dispatch");
    REQUIRE(rig.store->EnqueueSave(MakeFact("fact.deploy", "部署命令", "deploy 走 build.sh。")).has_value());
    REQUIRE(memory::RunPendingMemoryJobs(rig.home).has_value());

    const std::string dispatched =
        rig.store->BuildTurnContextForDispatch("查 deploy 的跑法", rig.repo, "agent-run-77");
    REQUIRE_FALSE(dispatched.empty());
    REQUIRE(rig.accounting->injections.size() == 1);
    CHECK(rig.accounting->injections[0].target_run_id == "agent-run-77");
}

TEST_CASE("P0-3: worker 回执进 workspace lifecycle,重复 commit 幂等") {
    Rig rig("lifecycle");
    REQUIRE(rig.store->EnqueueSave(MakeFact("fact.deploy", "部署命令", "deploy 走 build.sh。")).has_value());
    REQUIRE(memory::RunPendingMemoryJobs(rig.home).has_value());

    const fs::path lifecycle = rig.store->workspace_dir() / "lifecycle";
    std::error_code ec;
    fs::directory_iterator it(lifecycle, ec);
    REQUIRE_FALSE(ec);
    std::vector<fs::path> ops;
    for (const auto& item : it) ops.push_back(item.path());
    REQUIRE(ops.size() == 1);
    const auto intent = nlohmann::json::parse(Read(ops[0] / "intent.json"));
    CHECK(intent.value("operation", std::string()) == "memory_save");
    CHECK(intent.value("workspace_key", std::string()) == rig.store->identity().workspace_key);
    const auto result = nlohmann::json::parse(Read(ops[0] / "result.json"));
    CHECK(result.value("status", std::string()) == "completed");
    CHECK(result["outcome"].value("memory_id", std::string()) == "fact.deploy");
    CHECK(result["outcome"].value("content_sha256", std::string()).size() == 64);
    const std::string first_result_text = Read(ops[0] / "result.json");

    // 再造一份 rebuild job:operation_id 不同,正常落第二笔回执。
    nlohmann::json job{
        {"schema", 1},
        {"operation", "rebuild"},
        {"workspace_key", rig.store->identity().workspace_key},
        {"project_root", rig.repo.generic_string()},
        {"memory_dir", rig.store->memory_dir().generic_string()},
    };
    const fs::path pending = rig.home / "memory-jobs" / "pending" / "99000-1.json";
    Write(pending, job.dump(2) + "\n");
    REQUIRE(memory::RunPendingMemoryJobs(rig.home).has_value());
    fs::directory_iterator after(lifecycle, ec);
    REQUIRE_FALSE(ec);
    std::size_t count = 0;
    for (const auto& item : after) ++count;
    CHECK(count == 2);

    // 崩溃续跑的重复 commit:同一文件名的 job 再投(operation_id 相同,
    // result 已在),幂等放行且不造第二份 result。
    nlohmann::json upsert{
        {"schema", 1},
        {"operation", "upsert"},
        {"workspace_key", rig.store->identity().workspace_key},
        {"project_root", rig.repo.generic_string()},
        {"memory_dir", rig.store->memory_dir().generic_string()},
        {"kind", "fact"},
        {"id", "fact.deploy"},
        {"title", "部署命令"},
        {"summary", "部署命令"},
        {"content", "deploy 走 build.sh(重复投递)。"},
        {"paths", nlohmann::json::array({"build.sh"})},
    };
    const fs::path dup = rig.home / "memory-jobs" / "pending" / "97000-1.json";
    Write(dup, upsert.dump(2) + "\n");
    REQUIRE(memory::RunPendingMemoryJobs(rig.home).has_value());
    REQUIRE(fs::exists(lifecycle / "memsave-97000-1" / "result.json", ec));
    const std::string dup_result = Read(lifecycle / "memsave-97000-1" / "result.json");
    // 同一文件名再投一次(崩溃续跑的重复 commit):幂等放行,回执不改写。
    Write(dup, upsert.dump(2) + "\n");
    REQUIRE(memory::RunPendingMemoryJobs(rig.home).has_value());
    CHECK_FALSE(fs::exists(dup, ec));
    CHECK(Read(lifecycle / "memsave-97000-1" / "result.json") == dup_result);
    // 已提交的旧回执一字不动。
    CHECK(Read(ops[0] / "result.json") == first_result_text);
}

TEST_CASE("P0-3: 指旧 projects/ 的存量 job 拒办挪 failed,旧树零写入") {
    Rig rig("legacy-job");
    const fs::path legacy_dir = rig.home / "projects" / "old-key" / "memory";
    nlohmann::json job{
        {"schema", 1},
        {"operation", "rebuild"},
        {"workspace_key", "old-key"},
        {"project_root", rig.repo.generic_string()},
        {"memory_dir", legacy_dir.generic_string()},
    };
    Write(rig.home / "memory-jobs" / "pending" / "98000-1.json", job.dump(2) + "\n");
    REQUIRE(memory::RunPendingMemoryJobs(rig.home).has_value());
    CHECK(fs::exists(rig.home / "memory-jobs" / "failed" / "98000-1.json"));
    CHECK(fs::exists(rig.home / "memory-jobs" / "failed" / "98000-1.json.error.txt"));
    std::error_code ec;
    CHECK_FALSE(fs::exists(legacy_dir, ec));
}
