// 存储 v2 P0-3:memory 落账桥到真 Trajectory 账的端到端。
//
// 验收线(单子 §十 P0-3):改动或删除当前 Memory 后,旧 session 仍可重建
// 当时注入版本——context.injected 进 main.jsonl,快照进 session artifacts
// 的内容寻址 blob,Memory 后来怎么改,旧账上的 hash 与快照一字不动。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "app/memory_ledger_bridge.hpp"
#include "hooks/hash.hpp"
#include "memory/project_memory.hpp"
#include "runtime/trajectory_session.hpp"
#include "workspace/identity.hpp"
#include "trajectory/blob_store.hpp"
#include "trajectory/journal.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    static const auto run_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    // 路径要短:blob 内容寻址文件名 70 字符,Windows 260 上限经不起深
    // 临时目录 + 长前缀的折腾。
    fs::path path = fs::temp_directory_path() /
                    ("lmb-" + std::to_string(run_id % 100000) + "-" + name + "-" +
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

std::string BigBody(const std::string& head) {
    // 超 512B 才走 snapshot_ref(小内容按合同内联)。
    std::string body = head + "\n";
    while (body.size() < 700) body += "补充行:快照须与今天的 Memory 分账。\n";
    return body;
}

std::optional<nlohmann::json> FindEvent(const fs::path& stream, const std::string& kind) {
    const auto lines = trajectory::ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("kind")) continue;
        if (parsed["kind"] == kind) return parsed;
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("P0-3: 召回快照进 main.jsonl,Memory 改后旧账不动") {
    const fs::path root = TempRoot("recall");
    const fs::path repo = root / "repo";
    const fs::path home = root / "home";
    fs::create_directories(repo);
    fs::create_directories(home);
    Write(repo / "build.sh", "#!/bin/sh\necho build v1\n");

    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    auto identity = memory::ResolveProjectIdentity(repo, home);
    REQUIRE(identity.has_value());
    memory::ProjectMemory store(std::move(*identity), home, options);

    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact;
    request.id = "fact.deploy";
    request.title = "部署命令";
    request.summary = "部署命令";
    request.content = BigBody("deploy 走 build.sh。");
    request.keywords = {"deploy"};
    request.paths = {"build.sh"};
    request.confidence = "verified";
    REQUIRE(store.EnqueueSave(request).has_value());
    REQUIRE(memory::RunPendingMemoryJobs(home).has_value());

    // 开轨迹账(与 memory 同一把 cwd_fallback 钥匙),挂落账桥。
    runtime::TrajectorySessionLedger::Options ledger_options;
    ledger_options.workspaces_root = home / "trajectories";
    ledger_options.workspace_root = repo;
    // 显式递冻结身份(§4.5):非 Git 临时目录不给 resolver 爬到真用户主目录
    // 的 .lubancode/config.json 里去。
    ledger_options.workspace_identity = workspace::MakeFallbackIdentity(repo);
    ledger_options.lubancode_version = "test";
    auto ledger = runtime::TrajectorySessionLedger::Open(ledger_options);
    REQUIRE(ledger.has_value());
    app::MemoryLedgerBridge bridge(*ledger);
    store.set_accounting(&bridge);
    store.set_source_session(ledger->session_id());

    const std::string context = store.BuildTurnContext("deploy 怎么跑", repo);
    REQUIRE_FALSE(context.empty());

    const fs::path main_stream = ledger->session_dir() / "main.jsonl";
    const auto injected = FindEvent(main_stream, "context.injected");
    REQUIRE(injected.has_value());
    const auto& payload = (*injected)["payload"];
    CHECK(payload.value("kind", std::string()) == "memory_recall");
    CHECK(payload.value("memory_level", std::string()) == "project");
    CHECK(payload.value("memory_id", std::string()) == "fact.deploy");
    CHECK(payload.value("memory_schema", 0) == 3);
    CHECK(payload.value("content_sha256", std::string()).size() == 64);
    CHECK(payload.contains("snapshot_ref"));
    CHECK(payload.contains("snapshot_inline") == false);
    CHECK(payload.value("injected_bytes", 0) > 512);

    // 快照可从 session artifacts 读回,指纹对得上。
    trajectory::BlobStore blobs(ledger->session_dir() / "artifacts");
    trajectory::BlobRef ref;
    ref.sha256 = payload.value("content_sha256", std::string());
    ref.size = payload.value("injected_bytes", std::uint64_t{0});
    const auto snapshot = blobs.ReadVerified(ref);
    REQUIRE(snapshot.has_value());
    CHECK(hooks::Sha256Hex(*snapshot) == payload.value("content_sha256", std::string()));

    // 改掉当前 Memory(用户手编正文,绕过 worker):旧事件与快照一字不动,
    // 新一轮召回出新一枚事件。
    const std::string first_event_line = [&] {
        const auto lines = trajectory::ReadJournalLines(main_stream);
        for (const std::string& line : *lines) {
            if (line.find("context.injected") != std::string::npos) return line;
        }
        return std::string();
    }();
    {
        const std::string needle = "deploy 走 build.sh。";
        const std::string topic = Read(store.memory_dir() / "facts" / "deploy.md");
        const std::size_t body = topic.find(needle);
        REQUIRE(body != std::string::npos);
        Write(store.memory_dir() / "facts" / "deploy.md",
              topic.substr(0, body) + "deploy 走 build.sh,已改成 v2。" + topic.substr(body + needle.size()));
    }
    REQUIRE(memory::RebuildMemoryIndex(store.memory_dir()).has_value());
    const std::string second = store.BuildTurnContext("deploy 怎么跑", repo);
    REQUIRE_FALSE(second.empty());

    const auto lines = trajectory::ReadJournalLines(main_stream);
    REQUIRE(lines.has_value());
    std::size_t injected_count = 0;
    std::string last_hash;
    for (const std::string& line : *lines) {
        if (line.find("context.injected") == std::string::npos) continue;
        ++injected_count;
        last_hash = nlohmann::json::parse(line)["payload"].value("content_sha256", std::string());
    }
    CHECK(injected_count == 2);
    CHECK(last_hash != payload.value("content_sha256", std::string()));
    // 旧事件原样仍在(逐字节),Replay 重建的是当时那一版。
    bool first_line_intact = false;
    for (const std::string& line : *lines) {
        if (line == first_event_line) first_line_intact = true;
    }
    CHECK(first_line_intact);
}

TEST_CASE("P0-3: memory.save.requested 落因果边,引用全限定") {
    const fs::path root = TempRoot("save-edge");
    const fs::path repo = root / "repo";
    const fs::path home = root / "home";
    fs::create_directories(repo);
    fs::create_directories(home);

    runtime::TrajectorySessionLedger::Options ledger_options;
    ledger_options.workspaces_root = home / "trajectories";
    ledger_options.workspace_root = repo;
    // 显式递冻结身份(§4.5):非 Git 临时目录不给 resolver 爬到真用户主目录
    // 的 .lubancode/config.json 里去。
    ledger_options.workspace_identity = workspace::MakeFallbackIdentity(repo);
    ledger_options.lubancode_version = "test";
    auto ledger = runtime::TrajectorySessionLedger::Open(ledger_options);
    REQUIRE(ledger.has_value());

    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    auto identity = memory::ResolveProjectIdentity(repo, home);
    REQUIRE(identity.has_value());
    memory::ProjectMemory store(std::move(*identity), home, options);
    app::MemoryLedgerBridge bridge(*ledger);
    store.set_accounting(&bridge);

    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact;
    request.id = "fact.tools";
    request.title = "工具表";
    request.summary = "工具表";
    request.content = "工具表在 src/tools 下。";
    request.confidence = "verified";
    const auto queued = store.EnqueueSave(request);
    REQUIRE(queued.has_value());

    const auto edge = FindEvent(ledger->session_dir() / "main.jsonl", "memory.save.requested");
    REQUIRE(edge.has_value());
    const auto& payload = (*edge)["payload"];
    CHECK(payload["request"].value("operation", std::string()) == "upsert");
    CHECK(payload["request"].value("memory_id", std::string()) == "fact.tools");

    // 全限定引用进了 pending job,worker 落进主题的 source_sessions。
    std::error_code ec;
    fs::directory_iterator it(home / "memory-jobs" / "pending", ec);
    REQUIRE_FALSE(ec);
    std::string job_ref;
    for (const auto& item : it) {
        const auto job = nlohmann::json::parse(Read(item.path()));
        job_ref = job.value("source_event_ref", std::string());
    }
    REQUIRE_FALSE(job_ref.empty());
    CHECK(job_ref.starts_with("workspace_key=" + ledger->workspace_key() + "/session_id=" +
                              ledger->session_id() + "/run_id="));
    CHECK(job_ref.find("event_id=") != std::string::npos);
}

TEST_CASE("P0-3: 派工快照事件带 relations.child_run_id") {
    const fs::path root = TempRoot("dispatch");
    const fs::path repo = root / "repo";
    const fs::path home = root / "home";
    fs::create_directories(repo);
    fs::create_directories(home);
    Write(repo / "build.sh", "#!/bin/sh\necho build\n");

    runtime::TrajectorySessionLedger::Options ledger_options;
    ledger_options.workspaces_root = home / "trajectories";
    ledger_options.workspace_root = repo;
    // 显式递冻结身份(§4.5):非 Git 临时目录不给 resolver 爬到真用户主目录
    // 的 .lubancode/config.json 里去。
    ledger_options.workspace_identity = workspace::MakeFallbackIdentity(repo);
    ledger_options.lubancode_version = "test";
    auto ledger = runtime::TrajectorySessionLedger::Open(ledger_options);
    REQUIRE(ledger.has_value());

    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    auto identity = memory::ResolveProjectIdentity(repo, home);
    REQUIRE(identity.has_value());
    memory::ProjectMemory store(std::move(*identity), home, options);
    app::MemoryLedgerBridge bridge(*ledger);
    store.set_accounting(&bridge);

    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact;
    request.id = "fact.deploy";
    request.title = "部署命令";
    request.summary = "部署命令";
    request.content = "deploy 走 build.sh。";
    request.keywords = {"deploy"};
    request.paths = {"build.sh"};
    request.confidence = "verified";
    REQUIRE(store.EnqueueSave(request).has_value());
    REQUIRE(memory::RunPendingMemoryJobs(home).has_value());

    REQUIRE_FALSE(store.BuildTurnContextForDispatch("查 deploy 的跑法", repo, "agent-run-42").empty());

    const auto lines = trajectory::ReadJournalLines(ledger->session_dir() / "main.jsonl");
    REQUIRE(lines.has_value());
    bool found = false;
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || parsed.value("kind", std::string()) != "context.injected") continue;
        CHECK(parsed.value("actor", std::string()) == "host");
        CHECK(parsed.value("origin", std::string()) == "memory_recall");
        CHECK(parsed["relations"].value("child_run_id", std::string()) == "agent-run-42");
        found = true;
    }
    CHECK(found);
}
