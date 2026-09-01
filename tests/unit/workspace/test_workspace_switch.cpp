// P0-1(§4.5):同 workspace cwd change 与跨 workspace session switch 的
// 机制账。同 common git dir 内换 cwd → control.cwd.changed + checkout 登记,
// 账不换房;跨 workspace → 封旧场、开新场,旧账留在旧 workspace。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "runtime/session_runtime.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/session_manager.hpp"
#include "platform/paths.hpp"  // PathToUtf8:cwd 事件文本口径
#include "workspace/identity.hpp"
#include "workspace/manifest.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    const fs::path root = fs::temp_directory_path() / ("lubancode-ws-switch-" + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

void MakeRepo(const fs::path& repo) {
    fs::create_directories(repo / ".git");
}

void MakeLinkedWorktree(const fs::path& main_repo, const fs::path& worktree,
                        const std::string& name) {
    const fs::path git_dir = main_repo / ".git" / "worktrees" / name;
    fs::create_directories(git_dir);
    fs::create_directories(worktree);
    Write(worktree / ".git", "gitdir: " + git_dir.generic_string() + "\n");
    Write(git_dir / "commondir", "../..\n");
}

bool MainJsonlHasCwdEvent(const fs::path& session_dir, const std::string& cwd_text) {
    const auto lines = trajectory::ReadJournalLines(session_dir / "main.jsonl");
    if (!lines.has_value()) return false;
    for (const std::string& line : *lines) {
        const auto json = nlohmann::json::parse(line, nullptr, false);
        if (json.is_discarded()) continue;
        if (json.value("kind", std::string()) == "control.cwd.changed" &&
            json["payload"].value("cwd", std::string()) == cwd_text) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("SessionManager:冻结身份递进,workspace 目录与 manifest 同 key") {
    const fs::path root = TempRoot("manager");
    MakeRepo(root / "demo-repo");
    const auto identity = workspace::ResolveWorkspaceIdentity(root / "demo-repo" / "src", {})
                              .value();

    trajectory::SessionManagerOptions options;
    options.trajectories_root = root / "trajectories";
    options.identity = identity;
    trajectory::SessionManager manager(options);
    CHECK(manager.workspace_key() == identity.workspace_key);
    CHECK(manager.workspace_dir().filename().generic_string() == identity.workspace_key);

    REQUIRE(manager.LaunchSession().has_value());
    const auto read = workspace::ReadWorkspaceManifest(manager.workspace_dir());
    REQUIRE(read.status == workspace::ManifestRead::Status::Ok);
    CHECK(read.manifest.workspace_key == identity.workspace_key);
    CHECK(read.manifest.identity_kind == "git_common");
    REQUIRE(read.manifest.checkouts.size() == 1);
    CHECK(read.manifest.checkouts[0].root ==
          workspace::NormalizeIdentityPathText(identity.checkout_root));
}

TEST_CASE("RegisterCheckout:同 key upsert,异 key 拒绝(key_mismatch 隔离)") {
    const fs::path root = TempRoot("checkout");
    MakeRepo(root / "demo-repo");
    MakeLinkedWorktree(root / "demo-repo", root / "demo-repo-wt", "wt");
    const auto main_identity = workspace::ResolveWorkspaceIdentity(root / "demo-repo", {}).value();
    const auto wt_identity = workspace::ResolveWorkspaceIdentity(root / "demo-repo-wt", {}).value();
    CHECK(main_identity.workspace_key == wt_identity.workspace_key);

    trajectory::SessionManagerOptions options;
    options.trajectories_root = root / "trajectories";
    options.identity = main_identity;
    trajectory::SessionManager manager(options);
    REQUIRE(manager.LaunchSession().has_value());

    // worktree 的 checkout 进同一间 workspace 的登记。
    REQUIRE(manager.RegisterCheckout(wt_identity).has_value());
    const auto read = workspace::ReadWorkspaceManifest(manager.workspace_dir());
    REQUIRE(read.status == workspace::ManifestRead::Status::Ok);
    REQUIRE(read.manifest.checkouts.size() == 2);

    // 异 key:拒绝,不往这间房写别人的登记。
    const fs::path other = root / "other-repo";
    MakeRepo(other);
    const auto other_identity = workspace::ResolveWorkspaceIdentity(other, {}).value();
    const auto rejected = manager.RegisterCheckout(other_identity);
    CHECK_FALSE(rejected.has_value());
    CHECK(rejected.error().find("identity.key_mismatch") != std::string::npos);
}

TEST_CASE("HandleCwdChange:同 workspace 落 cwd.changed,跨 workspace 不写旧账") {
    const fs::path root = TempRoot("cwd-change");
    MakeRepo(root / "demo-repo");
    MakeLinkedWorktree(root / "demo-repo", root / "demo-repo-wt", "wt");
    MakeRepo(root / "other-repo");
    const auto identity = workspace::ResolveWorkspaceIdentity(root / "demo-repo", {}).value();
    const auto wt_identity = workspace::ResolveWorkspaceIdentity(root / "demo-repo-wt", {}).value();
    const auto other_identity = workspace::ResolveWorkspaceIdentity(root / "other-repo", {}).value();

    runtime::TrajectorySessionLedger::Options options;
    options.trajectories_root = root / "trajectories";
    options.workspace_identity = identity;
    options.lubancode_version = "test";
    auto ledger = runtime::TrajectorySessionLedger::Open(options);
    REQUIRE(ledger.has_value());
    const fs::path first_session_dir = ledger->session_dir();
    const std::string first_session_id = ledger->session_id();

    // 同 workspace:进 linked worktree,账不换房,落 cwd.changed + 登记。
    auto wt_change = ledger->HandleCwdChange(wt_identity);
    CHECK(wt_change.same_workspace);
    CHECK(wt_change.workspace_key == wt_identity.workspace_key);
    CHECK(MainJsonlHasCwdEvent(first_session_dir,
                               platform::PathToUtf8(wt_identity.launch_cwd)));
    const auto read = workspace::ReadWorkspaceManifest(
        root / "trajectories" / "workspaces" / identity.workspace_key);
    REQUIRE(read.status == workspace::ManifestRead::Status::Ok);
    REQUIRE(read.manifest.checkouts.size() == 2);
    CHECK(ledger->session_id() == first_session_id);  // session 不换

    // 跨 workspace:一个字不写,报告 same_workspace=false 带新 key。
    auto other_change = ledger->HandleCwdChange(other_identity);
    CHECK_FALSE(other_change.same_workspace);
    CHECK(other_change.workspace_key == other_identity.workspace_key);
    CHECK(other_change.error.empty());
    CHECK_FALSE(MainJsonlHasCwdEvent(first_session_dir,
                                     platform::PathToUtf8(other_identity.launch_cwd)));
}

TEST_CASE("SessionRuntime:跨 workspace 切换封旧场开新场,旧账留旧房") {
    const fs::path root = TempRoot("switch");
    MakeRepo(root / "repo-a");
    MakeRepo(root / "repo-b");
    const auto identity_a = workspace::ResolveWorkspaceIdentity(root / "repo-a", {}).value();
    const auto identity_b = workspace::ResolveWorkspaceIdentity(root / "repo-b", {}).value();

    runtime::SessionRuntime::Options options;
    options.trajectory_enabled = true;
    options.trajectory_workspace_identity = identity_a;
    options.trajectory_trajectories_root = root / "trajectories";
    options.lubancode_version = "test";
    runtime::SessionRuntime session(options);
    REQUIRE(session.trajectory() != nullptr);
    const fs::path first_session_dir = session.trajectory()->session_dir();
    const fs::path workspace_a_dir = root / "trajectories" / "workspaces" /
                                     identity_a.workspace_key;

    // 同 workspace 换 cwd(进子目录):不换场。
    fs::create_directories(root / "repo-a" / "src");
    CHECK(session.NoteWorkingDirectoryChanged(root / "repo-a" / "src").empty());
    CHECK(session.trajectory()->session_dir() == first_session_dir);

    // 跨 workspace:封旧开新。
    CHECK(session.NoteWorkingDirectoryChanged(root / "repo-b").empty());
    const fs::path second_session_dir = session.trajectory()->session_dir();
    CHECK(second_session_dir != first_session_dir);
    CHECK(second_session_dir.parent_path().parent_path() ==
          root / "trajectories" / "workspaces" / identity_b.workspace_key);

    // 旧场的账留在旧 workspace,可查(封口后的 session.json 落 closed)。
    const auto first_manifest = trajectory::ReadSessionJson(first_session_dir);
    REQUIRE(first_manifest.has_value());
    CHECK(first_manifest->status == "closed");
    // 新 workspace 的 manifest 也开出来了。
    const auto read_b = workspace::ReadWorkspaceManifest(
        root / "trajectories" / "workspaces" / identity_b.workspace_key);
    REQUIRE(read_b.status == workspace::ManifestRead::Status::Ok);
    CHECK(read_b.manifest.workspace_key == identity_b.workspace_key);

    // flag 关的老路:口子是 no-op。
    runtime::SessionRuntime::Options off_options;
    off_options.trajectory_enabled = false;
    runtime::SessionRuntime off(off_options);
    CHECK(off.NoteWorkingDirectoryChanged(root / "repo-a").empty());
}
