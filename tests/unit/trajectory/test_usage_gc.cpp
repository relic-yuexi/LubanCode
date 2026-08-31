// P0-4 容量账与 derived-only GC(§12.2):四笔容量分类、GC 次序
// temp→index→checkpoint→derived、canonical JSONL 与 artifacts/ blob 永不
// 进候选表(候选生成阶段就没这条路)、DryRun 不动盘、DerivedOnly 只删
// 可重建物且逐条容错。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "trajectory/usage_gc.hpp"

using namespace lubancode::trajectory;

namespace {

std::filesystem::path FreshDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void PutFile(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << content;
}

// 一间像样的 session:canonical 各件、artifacts blob、可重建 indexes/
// checkpoints(含 workflow 的)、derived/exports、一个崩溃残留的临时件。
std::filesystem::path FabricateSession(const std::filesystem::path& parent) {
    const auto session = parent / "20260831-120000-AAAAAA";
    PutFile(session / "main.jsonl", "{}\n");
    PutFile(session / "session.json", "{}");
    PutFile(session / "session.lock", "lock");
    PutFile(session / "subagents/agent-1.jsonl", "{}\n");
    PutFile(session / "workflows/wf-1/workflow.jsonl", "{}\n");
    PutFile(session / "workflows/wf-1/definition.json", "{}");
    PutFile(session / "workflows/wf-1/nodes/node-1.jsonl", "{}\n");
    PutFile(session / "artifacts/sha256/ab" / std::string(62, 'b'), "blob-body");
    PutFile(session / "artifacts/deadbeef.tmp-7", "half-written");  // 崩溃残留(分桶外)
    PutFile(session / "indexes/timeline.json", "{}");
    PutFile(session / "checkpoints/main-8.json", "{}");
    PutFile(session / "workflows/wf-1/checkpoints/node-2.json", "{}");
    PutFile(session / "derived/episode-1.json", "{}");
    PutFile(session / "exports/training-v1.json", "{}");
    PutFile(session / "goals/g-1.jsonl", "{}\n");
    PutFile(session / "loops/l-1.jsonl", "{}\n");
    return session;
}

}  // namespace

TEST_CASE("ScanSessionCapacity:四笔容量各归各账") {
    const auto root = FreshDir("lubancode-p4-usage");
    const auto session = FabricateSession(root);

    const auto usage = ScanSessionCapacity(session);
    CHECK(usage.session_id == "20260831-120000-AAAAAA");

    // journal:main/subagents/workflows 的 jsonl + definition + session.json/
    // session.lock/goals/loops。
    CHECK(usage.journal_files == 9);
    CHECK(usage.journal_bytes > 0);

    // blobs:artifacts/ 下两件(成品 blob 与 .tmp-7 残留都算 artifacts 子树)。
    CHECK(usage.referenced_blob_files == 2);

    // rebuildable:indexes/ + checkpoints/ + workflow checkpoints/。
    CHECK(usage.rebuildable_files == 3);

    // derived:derived/ + exports/。
    CHECK(usage.derived_files == 2);

    CHECK(usage.total_files() == 16);
    CHECK(usage.total_bytes() > 0);
}

TEST_CASE("PlanSessionGc:次序定死,canonical 与 blob 永不进候选表") {
    const auto root = FreshDir("lubancode-p4-gc-plan");
    const auto session = FabricateSession(root);

    const auto plan = PlanSessionGc(session);
    REQUIRE_FALSE(plan.items.empty());

    // 次序:temp -> index -> checkpoint -> checkpoint -> derived -> derived。
    const std::vector<std::string> categories = [&] {
        std::vector<std::string> out;
        for (const auto& item : plan.items) {
            out.push_back(item.category);
        }
        return out;
    }();
    const std::vector<std::string> expected = {"temp",     "index",  "checkpoint",
                                               "checkpoint", "derived", "derived"};
    CHECK(categories == expected);

    // 候选表里没有 canonical 与成品 blob:main.jsonl/subagents/workflows 的
    // jsonl/definition/session.json/session.lock/artifacts/sha256/**。
    for (const auto& item : plan.items) {
        const std::string text = item.path.generic_string();
        CHECK_FALSE(text.find("main.jsonl") != std::string::npos);
        CHECK_FALSE(text.find("agent-1.jsonl") != std::string::npos);
        CHECK_FALSE(text.find("workflow.jsonl") != std::string::npos);
        CHECK_FALSE(text.find("node-1.jsonl") != std::string::npos);
        CHECK_FALSE(text.find("definition.json") != std::string::npos);
        CHECK_FALSE(text.find("session.json") != std::string::npos);
        CHECK_FALSE(text.find("session.lock") != std::string::npos);
        CHECK_FALSE(text.find("sha256") != std::string::npos);
        CHECK_FALSE(text.find("g-1.jsonl") != std::string::npos);
        CHECK_FALSE(text.find("l-1.jsonl") != std::string::npos);
    }
    CHECK(plan.reclaimable_bytes > 0);
}

TEST_CASE("RunSessionGc:DryRun 不动盘,DerivedOnly 只删可重建物") {
    const auto root = FreshDir("lubancode-p4-gc-run");
    const auto session = FabricateSession(root);

    // DryRun:一个字节不动。
    const auto dry = RunSessionGc(session, GcScope::DryRun);
    CHECK_FALSE(dry.applied);
    CHECK(dry.deleted_files == 0);
    CHECK(std::filesystem::exists(session / "indexes/timeline.json"));
    CHECK(std::filesystem::exists(session / "derived/episode-1.json"));

    // DerivedOnly:四类候选全删,canonical 与 blob 原地不动。
    const auto applied = RunSessionGc(session, GcScope::DerivedOnly);
    CHECK(applied.applied);
    CHECK(applied.deleted_files == 6);
    CHECK(applied.errors.empty());
    CHECK_FALSE(std::filesystem::exists(session / "artifacts/deadbeef.tmp-7"));
    CHECK_FALSE(std::filesystem::exists(session / "indexes/timeline.json"));
    CHECK_FALSE(std::filesystem::exists(session / "checkpoints/main-8.json"));
    CHECK_FALSE(std::filesystem::exists(session / "workflows/wf-1/checkpoints/node-2.json"));
    CHECK_FALSE(std::filesystem::exists(session / "derived/episode-1.json"));
    CHECK_FALSE(std::filesystem::exists(session / "exports/training-v1.json"));
    // 这些必须在:canonical 与成品 blob 是真账。
    CHECK(std::filesystem::exists(session / "main.jsonl"));
    CHECK(std::filesystem::exists(session / "subagents/agent-1.jsonl"));
    CHECK(std::filesystem::exists(session / "workflows/wf-1/workflow.jsonl"));
    CHECK(std::filesystem::exists(session / "workflows/wf-1/nodes/node-1.jsonl"));
    CHECK(std::filesystem::exists(session / "workflows/wf-1/definition.json"));
    CHECK(std::filesystem::exists(session / "session.json"));
    CHECK(std::filesystem::exists(session / "session.lock"));
    CHECK(std::filesystem::exists(session / "artifacts/sha256/ab" / std::string(62, 'b')));
    CHECK(std::filesystem::exists(session / "goals/g-1.jsonl"));
    CHECK(std::filesystem::exists(session / "loops/l-1.jsonl"));

    // 再扫:rebuildable/derived 两笔归零,blob 只剩成品那件。
    const auto after = ScanSessionCapacity(session);
    CHECK(after.rebuildable_files == 0);
    CHECK(after.derived_files == 0);
    CHECK(after.referenced_blob_files == 1);
    CHECK(after.journal_files == 9);
}

TEST_CASE("ScanWorkspaceUsage:workspace 汇总按 session 升序") {
    const auto root = FreshDir("lubancode-p4-workspace");
    std::filesystem::create_directories(root / "sessions");
    PutFile(FabricateSession(root / "sessions") / "extra.flag", "x");
    PutFile(root / "sessions/20260831-130000-BBBBBB/main.jsonl", "{}\n");

    const auto report = ScanWorkspaceUsage(root / "sessions", "demo-abc123");
    REQUIRE(report.sessions.size() == 2);
    CHECK(report.sessions[0].session_id == "20260831-120000-AAAAAA");
    CHECK(report.sessions[1].session_id == "20260831-130000-BBBBBB");
    CHECK(report.workspace_key == "demo-abc123");
    CHECK(report.total_bytes > 0);
}
