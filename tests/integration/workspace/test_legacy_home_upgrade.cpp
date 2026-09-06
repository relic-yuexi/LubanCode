// Workspace 收官验收·旧数据升级册(单子 §一第 7 条的 in-process 半边;
// 真二进制 E2E 在 tests/manual/workspace_e2e_probe.py)。
//
// 迁移器已整件退场(a069d06e,无老用户),升级语义随之定死:
//   - 旧根(~/.lubancode 下 sessions/、projects/、trajectories/ 三处)
//     零读零写——新账一个字节不碰旧树,旧树一个文件不多不少;
//   - 新根 workspaces/ 首次开仓原子写,workspace.json v2;
//   - v1 session.json 搬错家进新根 = 坏档:索引标 misplaced_v1,
//     resume 找不到可恢复场(不猜、不静默降级);
//   - 旧 projects/ 里的记忆不迁移:新场记忆空着开张,旧树原样保留。
// 旧数据全部用 tests/fixtures/workspace/ 的脱敏夹具造,不碰真主目录。
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "memory/project_memory.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/session_index.hpp"
#include "workspace/identity.hpp"
#include "workspace/index.hpp"  // 账本制:key 反查房门

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    static const auto run_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    ("lubancode-ws-upgrade-" + std::to_string(run_id % 100000) + "-" + name +
                     "-" + std::to_string(++sequence));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
    return path;
}

void CopyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);  // fs::copy 不补缺父目录
    fs::copy(from, to, fs::copy_options::recursive, ec);
    REQUIRE_FALSE(ec);
}

// 旧主目录:照夹具映射表铺三处旧根。
fs::path PlantLegacyHome(const std::string& name) {
    const fs::path home = TempRoot(name);
    const fs::path fixtures = fs::path(LUBANCODE_TEST_FIXTURES_DIR) / "workspace";
    // 旧会话平铺档。
    CopyTree(fixtures / "legacy", home / "sessions");
    // 旧项目记忆树(任挑一个 key 摆着,代表旧存量)。
    CopyTree(fixtures / "memory" / "project", home / "projects" / "old-key" / "memory");
    // 旧全局记忆(user 层路径不动,这层还有效)。
    CopyTree(fixtures / "memory" / "user", home / "memory" / "user");
    // 旧 memory job 队列。
    CopyTree(fixtures / "memory-jobs", home / "memory-jobs");
    return home;
}

// 一棵树的文件数与总字节(旧树"一字不动"的对账口径)。
std::pair<int, std::uintmax_t> TreeAccount(const fs::path& root) {
    int files = 0;
    std::uintmax_t bytes = 0;
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return {0, 0};
    }
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file(ec)) {
            ++files;
            bytes += entry.file_size(ec);
        }
    }
    return {files, bytes};
}

}  // namespace

TEST_CASE("升级: 旧根零读零写,新根全新开张") {
    const fs::path home = PlantLegacyHome("legacy-home");
    const fs::path repo = TempRoot("legacy-repo");
    fs::create_directories(repo / ".git");

    const auto before_sessions = TreeAccount(home / "sessions");
    const auto before_projects = TreeAccount(home / "projects");
    REQUIRE(before_sessions.first > 0);
    REQUIRE(before_projects.first > 0);

    // 升级后的第一跑:同主目录开账 + 记忆。
    {
        runtime::TrajectorySessionLedger::Options options;
        options.workspaces_root = home / "workspaces";
        options.workspace_root = repo;
        options.launch_cwd = repo.generic_string();
        options.lubancode_version = "upgrade-test";
        auto ledger = runtime::TrajectorySessionLedger::Open(std::move(options));
        REQUIRE(ledger.has_value());
        CHECK(fs::exists(ledger->session_dir() / "main.jsonl"));

        memory::Options memory_options;
        memory_options.global_allowed = true;
        memory_options.enabled = true;
        auto identity = memory::ResolveProjectIdentity(repo, home);
        REQUIRE(identity.has_value());
        auto store = std::make_shared<memory::ProjectMemory>(std::move(*identity), home,
                                                             memory_options);
        memory::SaveRequest request;
        request.kind = memory::MemoryKind::Fact;
        request.id = "fact.new";
        request.title = "新事实";
        request.summary = "新事实";
        request.content = "升级后新写的项目事实。";
        request.paths = {"README.md"};
        request.confidence = "verified";
        REQUIRE(store->EnqueueSave(request).has_value());
        REQUIRE(memory::RunPendingMemoryJobs(home).has_value());
        std::error_code ec;
        CHECK(fs::exists(store->memory_dir() / "facts" / "new.md", ec));
    }

    // 旧树一字不动:文件数与字节数对账。
    CHECK(TreeAccount(home / "sessions") == before_sessions);
    CHECK(TreeAccount(home / "projects") == before_projects);
    // 新根开出来了,且只有新账。
    std::error_code ec;
    CHECK(fs::exists(home / "workspaces", ec));
    int legacy_keys = 0;
    for (const auto& entry : fs::directory_iterator(home / "workspaces", ec)) {
        const std::string key = entry.path().filename().generic_string();
        if (key.find("old-key") != std::string::npos) {
            ++legacy_keys;
        }
    }
    CHECK(legacy_keys == 0);  // 旧 projects/<key> 不并账、不搬房
    // 旧 job 队列还躺在原地(旧 pending job 不被新场消化)。
    CHECK(fs::exists(home / "memory-jobs" / "pending", ec));
}

TEST_CASE("升级: 旧全局记忆仍按 user 层读——路径不动的那层不丢") {
    const fs::path home = PlantLegacyHome("user-layer");
    const fs::path repo = TempRoot("user-layer-repo");
    fs::create_directories(repo / ".git");

    memory::Options options;
    options.global_allowed = true;
    options.enabled = true;
    options.user_enabled = true;
    auto identity = memory::ResolveProjectIdentity(repo, home);
    REQUIRE(identity.has_value());
    auto store = std::make_shared<memory::ProjectMemory>(std::move(*identity), home, options);
    // 夹具里 user 层带 preference/feedback 主题:管理读口列得出。
    const auto managed = store->ListGlobalEntriesForManagement();
    CHECK_FALSE(managed.empty());
    for (const auto& entry : managed) {
        CHECK(entry.scope.level == "user");
    }
}

TEST_CASE("升级: v1 session.json 搬错家进新根 = 坏档,索引点名,不冒充可恢复") {
    const fs::path home = TempRoot("misplaced-v1");
    const fs::path repo = TempRoot("misplaced-repo");
    fs::create_directories(repo / ".git");
    const auto identity = workspace::ResolveWorkspaceIdentity(repo, {}).value();

    // 先正常开一场(好账),再手植一场 v1 manifest 的"旧档搬错家"。
    {
        runtime::TrajectorySessionLedger::Options options;
        options.workspaces_root = home / "workspaces";
        options.workspace_root = repo;
        options.lubancode_version = "upgrade-test";
        auto ledger = runtime::TrajectorySessionLedger::Open(std::move(options));
        REQUIRE(ledger.has_value());
    }
    // 账本制:房门按 key 反查(目录名是门牌,不是 key)。
    const fs::path sessions = *workspace::index::ResolveDirByWorkspaceKey(home / "workspaces",
                                                                          identity.workspace_key) /
                              "sessions";
    const fs::path misplaced = sessions / "20200101-000000-V1OLD0";
    fs::create_directories(misplaced);
    {
        std::ofstream file(misplaced / "session.json", std::ios::binary | std::ios::trunc);
        // v1 形状(schema_version:1);workspace_key/status 等 FromJson 必填键
        // 给全,保证读侧真能解出 schema_version 再判"搬错家"。
        file << R"json({"schema_version": 1, "workspace_key": ")json"
             << identity.workspace_key << R"json(", "session_id": "20200101-000000-V1OLD0",
                        "launch_cwd": "C:/old", "main_run_id": "main-0001",
                        "status": "closed"})json";
        std::ofstream main(misplaced / "main.jsonl", std::ios::binary | std::ios::trunc);
        main << "";  // 空账:v1 判坏只看 manifest 的 schema_version
    }

    trajectory::SessionIndexQuery query;
    query.current_workspace_key = identity.workspace_key;
    const auto page = trajectory::QueryWorkspaceSessions(home / "workspaces", query);
    REQUIRE(page.entries.size() == 2);
    int misplaced_flagged = 0;
    int healthy = 0;
    for (const auto& entry : page.entries) {
        if (entry.session_id == "20200101-000000-V1OLD0") {
            REQUIRE(entry.misplaced_v1);
            ++misplaced_flagged;
        } else {
            CHECK_FALSE(entry.misplaced_v1);
            ++healthy;
        }
    }
    CHECK(misplaced_flagged == 1);
    CHECK(healthy == 1);
}
