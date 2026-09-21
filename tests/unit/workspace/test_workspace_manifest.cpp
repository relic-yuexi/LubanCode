// P0-1:workspace v2 manifest 的读写、版本协商、checkout 登记、key 对账
// 与 doctor 报表接线。合同冻结在 P0-0-contracts.md §二;本册验"照合同落"。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"
#include "trajectory/metrics.hpp"
#include "workspace/identity.hpp"
#include "workspace/index.hpp"          // 账本制:门牌与房门反查
#include "workspace/manifest.hpp"
#include "workspace/manifest_lock.hpp"  // SV-11:读改写事务锁
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

// 把目录 mtime 拨回十分钟前(锁龄检验用;SV-11 的锁不认龄,拨旧也不许夺)。
std::error_code BackdateMtime(const fs::path& path) {
    std::error_code ec;
    fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::minutes(10), ec);
    return ec;
}

// 房里有没有 .manifest.lock.stale-* 的隔离留证。
bool HasStaleLockEvidence(const fs::path& workspace_dir) {
    std::error_code ec;
    fs::directory_iterator it(workspace_dir, ec);
    if (ec) return false;
    for (const auto& item : it) {
        if (platform::PathToUtf8(item.path().filename()).starts_with(".manifest.lock.stale-")) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("manifest:首仓原子写 v2,字段照冻结合同") {
    const fs::path root = TempRoot("first");
    const auto identity = workspace::MakeFallbackIdentity(root / "proj");
    bool created = false;
    fs::path opened_dir;
    auto manifest = workspace::OpenOrRegisterWorkspace(root / "workspaces", identity, 1000, &created,
                                                       &opened_dir);
    REQUIRE(manifest.has_value());
    CHECK(created);
    CHECK(manifest->workspace_key == identity.workspace_key);
    CHECK(manifest->created_at_ms == 1000);
    CHECK(manifest->last_opened_at_ms == 1000);
    REQUIRE(manifest->checkouts.size() == 1);
    CHECK(manifest->checkouts[0].first_seen_at_ms == 1000);

    // 账本制:房门是门牌(≠ workspace_key),目录名不撞身份串。房门就开
    // 在调用方递来的根下(macOS 临时目录经软链,比原拼法不比规范形)。
    CHECK(opened_dir.parent_path() == root / "workspaces");
    CHECK(opened_dir.filename() != fs::path(identity.workspace_key));
    CHECK(opened_dir.filename() ==
          fs::path(workspace::index::MakeWorkspaceDirName(identity)));

    // 盘上的 JSON 带 schema/version 双键,身份四件齐。
    const auto json = nlohmann::json::parse(ReadAll(opened_dir / "workspace.json"), nullptr, false);
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
    // 账本制:手植的房得摆在门牌名下,开房路查账/重算门牌才会撞见它。
    const fs::path dir = root / "workspaces" /
                         fs::path(workspace::index::MakeWorkspaceDirName(identity));
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
    // 账本制:手植的房摆在门牌名下,开房路才会进这间。
    const fs::path dir = root / "workspaces" /
                         fs::path(workspace::index::MakeWorkspaceDirName(identity));
    // 伪造:房里的 manifest key 与算法重算不合(路径搬家后的旧账)。
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
    fs::path workspace_dir;
    REQUIRE(workspace::OpenOrRegisterWorkspace(root / "trajectories" / "workspaces", identity, 1000,
                                               &created, &workspace_dir)
                .has_value());

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

// ---------------------------------------------------------------------------
// SV-11:manifest 读改写串行化。时间戳单调是锁内并账的一部分;锁本身的
// 跨进程互斥/暴毙/隔离在 integration/workspace 的 racer 册验。
// ---------------------------------------------------------------------------

TEST_CASE("manifest:时间戳单调——now 比盘上账旧时不把账改回去") {
    const fs::path root = TempRoot("monotonic");
    const auto identity = workspace::MakeFallbackIdentity(root / "proj");
    REQUIRE(workspace::OpenOrRegisterWorkspace(root / "workspaces", identity, 5000).has_value());

    // 后写的钟慢(跨进程钟差):last_opened/last_seen 不倒退,first_seen
    // 永不改写,created 以旧账为准。
    auto older = workspace::OpenOrRegisterWorkspace(root / "workspaces", identity, 3000);
    REQUIRE(older.has_value());
    CHECK(older->created_at_ms == 5000);
    CHECK(older->last_opened_at_ms == 5000);
    REQUIRE(older->checkouts.size() == 1);
    CHECK(older->checkouts[0].first_seen_at_ms == 5000);
    CHECK(older->checkouts[0].last_seen_at_ms == 5000);

    // linked worktree 登记后再用旧钟开:同 root 只更新 last_seen,也不倒退。
    workspace::WorkspaceIdentity wt_identity = identity;
    wt_identity.checkout_root = fs::weakly_canonical(root / "proj-wt");
    wt_identity.launch_cwd = wt_identity.checkout_root;
    REQUIRE(workspace::OpenOrRegisterWorkspace(root / "workspaces", wt_identity, 4000).has_value());
    auto again = workspace::OpenOrRegisterWorkspace(root / "workspaces", wt_identity, 3500);
    REQUIRE(again.has_value());
    REQUIRE(again->checkouts.size() == 2);
    CHECK(again->checkouts[1].first_seen_at_ms == 4000);
    CHECK(again->checkouts[1].last_seen_at_ms == 4000);  // 3500 不把 4000 改回去
    CHECK(again->last_opened_at_ms == 5000);             // 全房 last_opened 取最大
}

TEST_CASE("ManifestLock: 同房二取被拒,释放后再取;探测与取锁同判") {
    const fs::path root = TempRoot("lock-basic");
    workspace::ManifestLock first;
    const auto got = workspace::ManifestLock::TryAcquire(root, &first);
    REQUIRE(got.status == workspace::ManifestLock::Status::Acquired);
    CHECK(first.holds());
    CHECK(workspace::ManifestLock::HolderAlive(root));

    // 活持有者不因锁龄被夺:拨旧十分钟后照旧拒。
    REQUIRE(!BackdateMtime(workspace::ManifestLockDir(root)));
    workspace::ManifestLock second;
    const auto refused = workspace::ManifestLock::TryAcquire(root, &second);
    CHECK(refused.status == workspace::ManifestLock::Status::HeldByLiveHolder);
    CHECK(refused.detail.find("pid") != std::string::npos);
    CHECK_FALSE(second.holds());
    CHECK(fs::exists(workspace::ManifestLockDir(root) / "owner"));  // 锁原封不动
    CHECK_FALSE(HasStaleLockEvidence(root));

    first.Release();
    CHECK_FALSE(fs::exists(workspace::ManifestLockDir(root)));
    CHECK_FALSE(workspace::ManifestLock::HolderAlive(root));
    const auto again = workspace::ManifestLock::TryAcquire(root, &second);
    CHECK(again.status == workspace::ManifestLock::Status::Acquired);
}

TEST_CASE("ManifestLock: owner 在但读不懂——明报不动,探测保守按持有") {
    const fs::path root = TempRoot("lock-broken");
    fs::create_directories(workspace::ManifestLockDir(root));
    Write(workspace::ManifestLockDir(root) / "owner", "not-json{{{");
    workspace::ManifestLock taker;
    const auto refused = workspace::ManifestLock::TryAcquire(root, &taker);
    CHECK(refused.status == workspace::ManifestLock::Status::BrokenLock);
    CHECK(refused.detail.find("不是合法 JSON") != std::string::npos);
    CHECK_FALSE(taker.holds());
    CHECK(fs::exists(workspace::ManifestLockDir(root) / "owner"));  // 原样没动
    CHECK(workspace::ManifestLock::HolderAlive(root));
    CHECK_FALSE(HasStaleLockEvidence(root));
}

TEST_CASE("ManifestLock: 无 owner 空锁目录——年轻按在建拒,老了隔离留证") {
    const fs::path root = TempRoot("lock-legacy");
    fs::create_directories(workspace::ManifestLockDir(root));
    // 年轻的空锁目录:视作对手"占目录与写 owner 之间"的在建窗口,保守拒。
    workspace::ManifestLock taker;
    const auto young = workspace::ManifestLock::TryAcquire(root, &taker);
    CHECK(young.status == workspace::ManifestLock::Status::HeldByLiveHolder);
    CHECK(young.detail.find("正在建立") != std::string::npos);
    CHECK_FALSE(taker.holds());
    CHECK(fs::exists(workspace::ManifestLockDir(root)));

    // 老的(旧格式残留):整目录隔离留证,再占新锁。
    REQUIRE(!BackdateMtime(workspace::ManifestLockDir(root)));
    const auto aged = workspace::ManifestLock::TryAcquire(root, &taker);
    CHECK(aged.status == workspace::ManifestLock::Status::Acquired);
    CHECK(aged.detail.find("隔离留证") != std::string::npos);
    CHECK(HasStaleLockEvidence(root));
}
