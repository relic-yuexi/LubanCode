// P0-1:workspace v2 manifest 的读写、版本协商、checkout 登记、key 对账
// 与 doctor 报表接线。合同冻结在 P0-0-contracts.md §二;本册验"照合同落"。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "trajectory/metrics.hpp"
#include "workspace/identity.hpp"
#include "workspace/manifest.hpp"
#include "workspace/storage_contracts.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    const fs::path root = fs::temp_directory_path() / ("lubancode-ws-manifest-" + name);
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

std::string ReadAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::string out((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return out;
}

}  // namespace

TEST_CASE("manifest:首仓原子写 v2,字段照冻结合同") {
    const fs::path root = TempRoot("first");
    const auto identity = workspace::MakeFallbackIdentity(root / "proj");
    bool created = false;
    auto manifest = workspace::OpenOrRegisterWorkspace(root / "workspaces", identity, 1000, &created);
    REQUIRE(manifest.has_value());
    CHECK(created);
    CHECK(manifest->workspace_key == identity.workspace_key);
    CHECK(manifest->created_at_ms == 1000);
    CHECK(manifest->last_opened_at_ms == 1000);
    REQUIRE(manifest->checkouts.size() == 1);
    CHECK(manifest->checkouts[0].first_seen_at_ms == 1000);

    // 盘上的 JSON 带 schema/version 双键,身份四件齐。
    const auto json = nlohmann::json::parse(
        ReadAll(root / "workspaces" / identity.workspace_key / "workspace.json"), nullptr, false);
    REQUIRE_FALSE(json.is_discarded());
    CHECK(json["schema"] == std::string(workspace::contracts::kWorkspaceSchemaName));
    CHECK(json["version"] == workspace::contracts::kWorkspaceSchemaVersion);
    CHECK(json["workspace_key"] == identity.workspace_key);
    CHECK(json["identity_kind"] == identity.identity_kind);
}

TEST_CASE("manifest:二次开仓不覆盖首仓账,checkout upsert 各记各的") {
    const fs::path root = TempRoot("reopen");
    const auto main_identity = workspace::MakeFallbackIdentity(root / "repo");
    const auto other_identity = workspace::MakeFallbackIdentity(root / "elsewhere");

    bool created = false;
    REQUIRE(workspace::OpenOrRegisterWorkspace(root / "workspaces", main_identity, 1000, &created)
                .has_value());
    CHECK(created);
    created = true;
    REQUIRE(workspace::OpenOrRegisterWorkspace(root / "workspaces", main_identity, 2000, &created)
                .has_value());
    CHECK_FALSE(created);  // 不是首仓

    // linked worktree:同 key 不同 checkout —— 登记里两行,同 key 只更新一行。
    workspace::WorkspaceIdentity wt_identity = main_identity;
    wt_identity.checkout_root = fs::weakly_canonical(root / "repo-wt");
    wt_identity.launch_cwd = wt_identity.checkout_root;
    auto updated = workspace::OpenOrRegisterWorkspace(root / "workspaces", wt_identity, 3000);
    REQUIRE(updated.has_value());
    REQUIRE(updated->checkouts.size() == 2);
    CHECK(updated->checkouts[0].root != updated->checkouts[1].root);
    CHECK(updated->checkouts[0].last_seen_at_ms == 2000);  // 主树登记不被误改
    CHECK(updated->checkouts[1].first_seen_at_ms == 3000);
    CHECK(updated->created_at_ms == 1000);  // 首仓时间以旧账为准

    // 同 checkout 再开:只更新 last_seen,不加行。
    auto again = workspace::OpenOrRegisterWorkspace(root / "workspaces", wt_identity, 4000);
    REQUIRE(again.has_value());
    CHECK(again->checkouts.size() == 2);
    CHECK(again->checkouts[1].last_seen_at_ms == 4000);
    CHECK(again->checkouts[1].first_seen_at_ms == 3000);

    (void)other_identity;
}

TEST_CASE("manifest:版本协商——version 超限整份拒读,不猜不降级") {
    const fs::path root = TempRoot("version");
    const auto identity = workspace::MakeFallbackIdentity(root / "proj");
    const fs::path dir = root / "workspaces" / identity.workspace_key;
    Write(dir / "workspace.json",
          nlohmann::json{{"schema", "lubancode.workspace"},
                         {"version", 3},
                         {"workspace_key", identity.workspace_key},
                         {"identity_kind", "cwd_fallback"}}
              .dump());

    const auto read = workspace::ReadWorkspaceManifest(dir);
    CHECK(read.status == workspace::ManifestRead::Status::UnsupportedVersion);
    CHECK(read.error_code == "schema.unsupported_version");

    // 对账登记也拒绝开这间房(隔离语义)。
    auto registered = workspace::OpenOrRegisterWorkspace(root / "workspaces", identity, 1000);
    CHECK_FALSE(registered.has_value());
    CHECK(registered.error().find("schema.unsupported_version") != std::string::npos);
}

TEST_CASE("manifest:key 对账——与算法重算不合即 identity.key_mismatch 隔离") {
    const fs::path root = TempRoot("mismatch");
    const auto identity = workspace::MakeFallbackIdentity(root / "proj");
    const fs::path dir = root / "workspaces" / identity.workspace_key;
    // 伪造:目录名与 manifest key 不合(路径搬家后同名目录)。
    Write(dir / "workspace.json",
          nlohmann::json{{"schema", "lubancode.workspace"},
                         {"version", 2},
                         {"workspace_key", "moved-repo-0000000000000000"},
                         {"display_name", "moved-repo"},
                         {"identity_kind", "git_common"},
                         {"identity_root", "D:/gone/.git"},
                         {"created_at_ms", 1},
                         {"last_opened_at_ms", 1}}
              .dump());

    auto registered = workspace::OpenOrRegisterWorkspace(root / "workspaces", identity, 1000);
    CHECK_FALSE(registered.has_value());
    CHECK(registered.error().find(std::string(workspace::contracts::kErrIdentityKeyMismatch)) !=
          std::string::npos);

    // Reconcile 同一判:重算 key 与 manifest key 逐字比。
    const auto read = workspace::ReadWorkspaceManifest(dir);
    REQUIRE(read.status == workspace::ManifestRead::Status::Ok);
    const auto reconcile = workspace::ReconcileWorkspaceManifest(read.manifest);
    CHECK_FALSE(reconcile.ok);
    CHECK(reconcile.error_code == std::string(workspace::contracts::kErrIdentityKeyMismatch));
    CHECK(reconcile.expected_key != read.manifest.workspace_key);
}

TEST_CASE("manifest:对账通过样例(git/config/cwd 三态)") {
    workspace::WorkspaceManifest manifest;
    manifest.workspace_key = workspace::ComputeWorkspaceKeyFromSeed("git:/x/demo/.git", "demo");
    manifest.display_name = "demo";
    manifest.identity_kind = "git_common";
    manifest.identity_root = "/x/demo/.git";
    CHECK(workspace::ReconcileWorkspaceManifest(manifest).ok);

    manifest.identity_kind = "config_root";
    manifest.identity_root = "/x/demo";
    manifest.workspace_key = workspace::ComputeWorkspaceKeyFromSeed("path:/x/demo", "demo");
    CHECK(workspace::ReconcileWorkspaceManifest(manifest).ok);

    // marker:不递声明 id 报缺;递了就能重算。
    manifest.identity_kind = "explicit_marker";
    manifest.workspace_key = workspace::ComputeWorkspaceKeyFromSeed("marker:team-42", "demo");
    const auto missing = workspace::ReconcileWorkspaceManifest(manifest);
    CHECK_FALSE(missing.ok);
    CHECK(missing.error_code == "schema.missing_field");
    CHECK(workspace::ReconcileWorkspaceManifest(manifest, std::string("team-42")).ok);

    // identity_kind 出四值封闭集:拒绝。
    manifest.identity_kind = "remote_url";
    CHECK(workspace::ReconcileWorkspaceManifest(manifest, std::string("team-42")).error_code ==
          "schema.missing_field");
}

TEST_CASE("doctor 报表:manifest 对账进 /doctor trajectory 的账") {
    const fs::path root = TempRoot("doctor");
    const auto identity = workspace::MakeFallbackIdentity(root / "proj");
    bool created = false;
    REQUIRE(workspace::OpenOrRegisterWorkspace(root / "trajectories" / "workspaces", identity, 1000,
                                               &created)
                .has_value());
    const fs::path workspace_dir =
        root / "trajectories" / "workspaces" / identity.workspace_key;

    auto report = trajectory::BuildWorkspaceDoctorReport(
        root / "trajectories", workspace_dir, identity.workspace_key, std::nullopt, {});
    REQUIRE_FALSE(report.manifest_issues.empty());
    CHECK(report.manifest_issues[0].find("manifest 对账通过") != std::string::npos);
    const auto lines = trajectory::FormatWorkspaceDoctorReport(report);
    bool printed = false;
    for (const std::string& line : lines) {
        if (line.find("manifest 对账:") != std::string::npos) printed = true;
    }
    CHECK(printed);

    // 把 manifest 改坏(key 漂移):doctor 报 identity.key_mismatch。
    Write(workspace_dir / "workspace.json",
          nlohmann::json{{"schema", "lubancode.workspace"},
                         {"version", 2},
                         {"workspace_key", "tampered-0000000000000000"},
                         {"display_name", "tampered"},
                         {"identity_kind", "cwd_fallback"},
                         {"identity_root", "/nowhere"}}
              .dump());
    auto broken = trajectory::BuildWorkspaceDoctorReport(
        root / "trajectories", workspace_dir, "tampered-0000000000000000", std::nullopt, {});
    REQUIRE_FALSE(broken.manifest_issues.empty());
    CHECK(broken.manifest_issues[0].find(std::string(workspace::contracts::kErrIdentityKeyMismatch)) !=
          std::string::npos);
}
