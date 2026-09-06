// Workspace 收官验收·linked worktree 共享与切换册(单子 §一第 2/3 条):
//   - 主树与 linked worktree(common git dir 同源)共 workspace、共 Memory:
//     两处各开一场 session,同 key 同 workspace.json,checkouts[] 双登记;
//     主树存的事实,worktree 里召回得到——记忆跟仓库走,不跟目录走。
//   - session 各带 checkout 现场:两场的 launch_cwd 各是各的检出根,
//     同一间 workspace 的 sessions 索引里都列得出。
//   - 跨 workspace 切换封旧开新:旧场留旧房(session.json closed,
//     end_reason=workspace_switch),新 workspace 开新场;两边 lifecycle
//     各记一笔 create_session 的 intent+result 回执,旧账一个字节不搬。
// 与 unit/workspace/test_workspace_switch.cpp 的分工:那册钉机制
// (RegisterCheckout/HandleCwdChange/SessionRuntime 切换),本册验收
// "共 Memory + 各带 checkout 现场 + 回执齐全"的整链。
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "memory/project_memory.hpp"
#include "platform/paths.hpp"
#include "runtime/session_runtime.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/directory.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/session_index.hpp"
#include "workspace/identity.hpp"
#include "workspace/index.hpp"  // 账本制:key 反查房门
#include "workspace/manifest.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    static const auto run_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    ("lubancode-ws-worktree-" + std::to_string(run_id % 100000) + "-" + name +
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

// 主树(真 .git 目录)+ linked worktree(.git 文件指 worktrees/<x>,
// commondir 回主树)——与 git worktree add 落盘形状逐字一致。
void MakeRepo(const fs::path& repo) { fs::create_directories(repo / ".git"); }

void MakeLinkedWorktree(const fs::path& main_repo, const fs::path& worktree,
                        const std::string& name) {
    const fs::path git_dir = main_repo / ".git" / "worktrees" / name;
    fs::create_directories(git_dir);
    fs::create_directories(worktree);
    Write(worktree / ".git", "gitdir: " + git_dir.generic_string() + "\n");
    Write(git_dir / "commondir", "../..\n");
}

memory::Options MemoryOptions() {
    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    return options;
}

memory::SaveRequest MakeFact(const std::string& id, const std::string& content) {
    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact;
    request.id = id;
    request.title = "部署命令";
    request.summary = "部署命令";
    request.content = content;
    request.keywords = {"deploy", "build"};
    request.paths = {"build.sh"};
    request.confidence = "verified";
    return request;
}

}  // namespace

TEST_CASE("worktree 共享: 同 key 同 workspace.json,checkouts 双登记,session 各带现场") {
    const fs::path root = TempRoot("shared");
    const fs::path repo = root / "demo-repo";
    const fs::path worktree = root / "demo-repo-wt";
    const fs::path home = root / "home";
    MakeRepo(repo);
    MakeLinkedWorktree(repo, worktree, "wt");
    fs::create_directories(home);
    Write(repo / "build.sh", "#!/bin/sh\necho build\n");

    const auto main_identity = workspace::ResolveWorkspaceIdentity(repo, {}).value();
    const auto wt_identity = workspace::ResolveWorkspaceIdentity(worktree, {}).value();
    REQUIRE(main_identity.workspace_key == wt_identity.workspace_key);
    REQUIRE(main_identity.git_common_dir == wt_identity.git_common_dir);

    // 主树开一场。
    runtime::TrajectorySessionLedger::Options main_options;
    main_options.workspaces_root = home / "workspaces";
    main_options.workspace_root = repo;
    main_options.launch_cwd = platform::PathToUtf8(main_identity.launch_cwd);
    main_options.lubancode_version = "wt-test";
    auto main_ledger = runtime::TrajectorySessionLedger::Open(std::move(main_options));
    REQUIRE(main_ledger.has_value());
    const fs::path main_session_dir = main_ledger->session_dir();

    // worktree 再开一场(同 workspace,session.json 的 launch_cwd 是 worktree)。
    runtime::TrajectorySessionLedger::Options wt_options;
    wt_options.workspaces_root = home / "workspaces";
    wt_options.workspace_identity = wt_identity;
    wt_options.launch_cwd = platform::PathToUtf8(wt_identity.launch_cwd);
    wt_options.lubancode_version = "wt-test";
    auto wt_ledger = runtime::TrajectorySessionLedger::Open(std::move(wt_options));
    REQUIRE(wt_ledger.has_value());
    const fs::path wt_session_dir = wt_ledger->session_dir();

    // 两场落在同一间 workspace(账本制:房门按 key 反查,目录名是门牌)。
    const fs::path workspace_dir = *workspace::index::ResolveDirByWorkspaceKey(
        home / "workspaces", main_identity.workspace_key);
    CHECK(main_session_dir.parent_path() == workspace_dir / "sessions");
    CHECK(wt_session_dir.parent_path() == workspace_dir / "sessions");

    // manifest:同 key,两个 checkout 都登记在案。
    const auto read = workspace::ReadWorkspaceManifest(workspace_dir);
    REQUIRE(read.status == workspace::ManifestRead::Status::Ok);
    CHECK(read.manifest.workspace_key == main_identity.workspace_key);
    REQUIRE(read.manifest.checkouts.size() == 2);
    CHECK(read.manifest.checkouts[0].root ==
          workspace::NormalizeIdentityPathText(main_identity.checkout_root));
    CHECK(read.manifest.checkouts[1].root ==
          workspace::NormalizeIdentityPathText(wt_identity.checkout_root));

    // session 各带 checkout 现场:launch_cwd 各是各的检出根。
    const auto main_manifest = trajectory::ReadSessionJson(main_session_dir);
    const auto wt_manifest = trajectory::ReadSessionJson(wt_session_dir);
    REQUIRE(main_manifest.has_value());
    REQUIRE(wt_manifest.has_value());
    CHECK(main_manifest->launch_cwd == platform::PathToUtf8(main_identity.launch_cwd));
    CHECK(wt_manifest->launch_cwd == platform::PathToUtf8(wt_identity.launch_cwd));
    CHECK(main_manifest->launch_cwd != wt_manifest->launch_cwd);

    // 两场都进同一份 sessions 索引(按 workspace 查,不按 cwd 分家)。
    trajectory::SessionIndexQuery query;
    query.current_workspace_key = main_identity.workspace_key;
    const auto page = trajectory::QueryWorkspaceSessions(home / "workspaces", query);
    REQUIRE(page.entries.size() == 2);
    for (const auto& entry : page.entries) {
        CHECK(entry.workspace_key == main_identity.workspace_key);
    }

    // 共 Memory:主树存,worktree 召回。
    {
        auto identity = memory::ResolveProjectIdentity(repo, home);
        REQUIRE(identity.has_value());
        auto store = std::make_shared<memory::ProjectMemory>(std::move(*identity), home,
                                                             MemoryOptions());
        REQUIRE(store->EnqueueSave(MakeFact("fact.deploy", "deploy 走 build.sh。")).has_value());
        REQUIRE(memory::RunPendingMemoryJobs(home).has_value());
    }
    {
        auto identity = memory::ResolveProjectIdentity(worktree, home);
        REQUIRE(identity.has_value());
        CHECK(identity->workspace_key == main_identity.workspace_key);
        auto viewer = std::make_shared<memory::ProjectMemory>(std::move(*identity), home,
                                                              MemoryOptions());
        const std::string context = viewer->BuildTurnContext("deploy 怎么跑", worktree);
        CHECK(context.find("fact.deploy") != std::string::npos);
    }
}

TEST_CASE("跨 workspace 切换: 封旧开新,回执两笔,旧账一字不搬") {
    const fs::path root = TempRoot("switch");
    const fs::path repo_a = root / "repo-a";
    const fs::path repo_b = root / "repo-b";
    MakeRepo(repo_a);
    MakeRepo(repo_b);

    const auto identity_a = workspace::ResolveWorkspaceIdentity(repo_a, {}).value();
    const auto identity_b = workspace::ResolveWorkspaceIdentity(repo_b, {}).value();
    REQUIRE(identity_a.workspace_key != identity_b.workspace_key);

    runtime::SessionRuntime::Options options;
    options.trajectory_workspace_identity = identity_a;
    options.trajectory_workspaces_root = root / "workspaces";
    options.lubancode_version = "wt-test";
    runtime::SessionRuntime session(options);
    REQUIRE(session.trajectory() != nullptr);
    const fs::path first_session_dir = session.trajectory()->session_dir();
    const std::string first_session_id = session.trajectory()->session_id();
    const fs::path workspace_a =
        *workspace::index::ResolveDirByWorkspaceKey(root / "workspaces", identity_a.workspace_key);

    // 切到 repo-b:封旧开新。
    REQUIRE(session.NoteWorkingDirectoryChanged(repo_b).empty());
    const fs::path second_session_dir = session.trajectory()->session_dir();
    CHECK(second_session_dir != first_session_dir);
    const fs::path workspace_b =
        *workspace::index::ResolveDirByWorkspaceKey(root / "workspaces", identity_b.workspace_key);
    CHECK(second_session_dir.parent_path() == workspace_b / "sessions");

    // 旧场:closed + workspace_switch 收口,留在旧房。
    const auto old_manifest = trajectory::ReadSessionJson(first_session_dir);
    REQUIRE(old_manifest.has_value());
    CHECK(old_manifest->status == "closed");
    bool ended_switch = false;
    const auto journal = trajectory::ReadJournalLines(first_session_dir / "main.jsonl");
    REQUIRE(journal.has_value());
    for (const std::string& line : *journal) {
        const auto event = nlohmann::json::parse(line, nullptr, false);
        if (event.is_discarded() ||
            (event.value("kind", std::string()) != "session.ended")) {
            continue;
        }
        ended_switch = event["payload"].value("reason", std::string()) == "workspace_switch";
    }
    CHECK(ended_switch);

    // 回执:两间 workspace 各有一笔 create_session 的 intent+result,
    // 新场的回执在新的 lifecycle 下。
    const auto CountLifecycle = [](const fs::path& workspace) {
        int ops = 0;
        std::error_code ec;
        if (!fs::exists(workspace / "lifecycle", ec)) {
            return 0;
        }
        for (const auto& entry : fs::directory_iterator(workspace / "lifecycle", ec)) {
            const bool has_intent = fs::exists(entry.path() / "intent.json", ec);
            const bool has_result = fs::exists(entry.path() / "result.json", ec);
            if (has_intent && has_result) {
                ++ops;
            }
        }
        return ops;
    };
    CHECK(CountLifecycle(workspace_a) >= 1);
    CHECK(CountLifecycle(workspace_b) >= 1);
    // 新场自己的 create 回执指认的就是它自己。
    {
        bool found_self = false;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(workspace_b / "lifecycle", ec)) {
            const auto intent = nlohmann::json::parse(Read(entry.path() / "intent.json"),
                                                      nullptr, false);
            if (intent.is_discarded() ||
                intent.value("operation", std::string()) != "create_session") {
                continue;
            }
            if (intent.value("session_id", std::string()) ==
                session.trajectory()->session_id()) {
                const auto result = nlohmann::json::parse(Read(entry.path() / "result.json"),
                                                          nullptr, false);
                found_self = !result.is_discarded() &&
                             result.value("status", std::string()) == "completed";
            }
        }
        CHECK(found_self);
    }

    // 旧 workspace 索引仍列得出旧场(closed 照列,事实不灭);
    // 新 workspace 索引只有新场。
    {
        trajectory::SessionIndexQuery in_a;
        in_a.current_workspace_key = identity_a.workspace_key;
        const auto page_a = trajectory::QueryWorkspaceSessions(root / "workspaces", in_a);
        REQUIRE(page_a.entries.size() == 1);
        CHECK(page_a.entries[0].session_id == first_session_id);
        CHECK(page_a.entries[0].status == "closed");

        trajectory::SessionIndexQuery in_b;
        in_b.current_workspace_key = identity_b.workspace_key;
        const auto page_b = trajectory::QueryWorkspaceSessions(root / "workspaces", in_b);
        REQUIRE(page_b.entries.size() == 1);
        CHECK(page_b.entries[0].session_id == session.trajectory()->session_id());
    }

    // 旧账验得过,新账验得过;两本各是各。
    CHECK(trajectory::VerifySessionDir(first_session_dir).ok);
    CHECK(trajectory::VerifySessionDir(second_session_dir).ok);
}
