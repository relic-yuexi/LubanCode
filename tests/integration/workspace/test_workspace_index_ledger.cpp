// Workspace 目录账本制·边界册(单子《workspace目录账本制命名与索引》):
// 目录名降级为门牌(路径slug+seed哈希前8),查找走 workspaces/index.json,
// 查账→miss 开房记账→账坏扫房重建。本册逐条钉:
//   1. 异地同名仓库:不同父目录下同名文件夹 → 两间房,门牌必不同;
//   2. 账本删除/写坏(截断/坏 JSON/空)→ 各房 workspace.json 自描述照旧,
//      会话照常恢复,重开即回账;
//   3. 并发开房:同路径并发开一间房;异路径并发各开各房;被挤掉的账目
//      靠门牌确定性 + 重开自愈;
//   4. Windows 大小写折叠同房(敏感性运行时探测);junction 进出同房;
//   5. 门牌生成:slug 变换样例逐字、超长截断仍唯一、Windows 保留名处置、
//      中文原样保留、非法字符 '_' 替换;
//   6. worktree 共账:主树+linked worktree 经 git common dir 同键 → 查账
//      同门牌同房,checkouts 双登记。
// 与 test_worktree_shared_storage.cpp 的分工:那册验收共账整链(session/
// memory/索引),本册钉账本机制本身的边界。
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/blob_store.hpp"
#include "trajectory/directory.hpp"
#include "trajectory/session_index.hpp"
#include "workspace/identity.hpp"
#include "workspace/index.hpp"
#include "workspace/manifest.hpp"
#include "workspace/storage_contracts.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    static int sequence = 0;
    static const auto run_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    ("lubancode-ws-index-" + std::to_string(run_id % 100000) + "-" + name +
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

std::string ReadAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void MakeRepo(const fs::path& repo) { fs::create_directories(repo / ".git"); }

void MakeLinkedWorktree(const fs::path& main_repo, const fs::path& worktree,
                        const std::string& name) {
    const fs::path git_dir = main_repo / ".git" / "worktrees" / name;
    fs::create_directories(git_dir);
    fs::create_directories(worktree);
    Write(worktree / ".git", "gitdir: " + git_dir.generic_string() + "\n");
    Write(git_dir / "commondir", "../..\n");
}

// 开一间房(经统一注册口),回房门。
fs::path OpenRoom(const fs::path& workspaces_root, const fs::path& project) {
    const auto identity = workspace::ResolveWorkspaceIdentity(project, {}).value();
    fs::path dir;
    const auto registered =
        workspace::OpenOrRegisterWorkspace(workspaces_root, identity, 1000, nullptr, &dir);
    REQUIRE(registered.has_value());
    return dir;
}

int RoomCount(const fs::path& workspaces_root) {
    int rooms = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(workspaces_root, ec)) {
        std::error_code dir_ec;
        if (entry.is_directory(dir_ec) && !dir_ec) {
            ++rooms;
        }
    }
    return rooms;
}

nlohmann::json ReadLedger(const fs::path& workspaces_root) {
    const auto parsed =
        nlohmann::json::parse(ReadAll(workspaces_root / "index.json"), nullptr, false);
    REQUIRE_FALSE(parsed.is_discarded());
    return parsed;
}

// 探测 root 所在盘的路径大小写敏感性(与 test_identity_cross_platform 同法:
// 盘说了算,不按平台猜)。
bool ProbeCaseSensitive(const fs::path& root) {
    const fs::path probe_upper = root / "Probe-Dir";
    const fs::path probe_lower = root / "probe-dir";
    std::error_code ec;
    fs::create_directories(probe_upper, ec);
    fs::create_directories(probe_lower, ec);
    const bool same_dir = fs::equivalent(probe_upper, probe_lower, ec);
    return !same_dir || ec;
}

}  // namespace

TEST_CASE("账本制: 异地同名仓库——两间房,门牌必不同,各查各门") {
    const fs::path root = TempRoot("same-name");
    const fs::path workspaces = root / "workspaces";
    const fs::path repo_a = root / "alpha" / "demo-repo";
    const fs::path repo_b = root / "beta" / "demo-repo";
    MakeRepo(repo_a);
    MakeRepo(repo_b);

    const auto identity_a = workspace::ResolveWorkspaceIdentity(repo_a, {}).value();
    const auto identity_b = workspace::ResolveWorkspaceIdentity(repo_b, {}).value();
    REQUIRE(identity_a.workspace_key != identity_b.workspace_key);  // seed 不同(git 根不同)

    const fs::path dir_a = OpenRoom(workspaces, repo_a);
    const fs::path dir_b = OpenRoom(workspaces, repo_b);

    // 两间房:门牌不同(哈希段不同),都不是 workspace_key。
    CHECK(dir_a != dir_b);
    CHECK(dir_a.filename() != fs::path(identity_a.workspace_key));
    CHECK(dir_b.filename() != fs::path(identity_b.workspace_key));
    // 门牌尾巴 = key 尾 16 hex 的前 8(seed 哈希前 8,唯一性保险)。
    const std::string hash8_a =
        identity_a.workspace_key.substr(identity_a.workspace_key.size() - 16, 8);
    CHECK(platform::PathToUtf8(dir_a.filename()).ends_with("-" + hash8_a));
    CHECK(RoomCount(workspaces) == 2);

    // 账本两笔,键是 identity_root 的归一文本(identity 现行机械)。
    const nlohmann::json ledger = ReadLedger(workspaces);
    CHECK(ledger["schema"] == std::string(workspace::contracts::kWorkspaceIndexSchemaName));
    CHECK(ledger["workspaces"].size() == 2);
    CHECK(ledger["workspaces"][workspace::NormalizeIdentityPathText(identity_a.identity_root)]
              ["dir"] == platform::PathToUtf8(dir_a.filename()));
    CHECK(ledger["workspaces"][workspace::NormalizeIdentityPathText(identity_b.identity_root)]
              ["dir"] == platform::PathToUtf8(dir_b.filename()));

    // 反查:各 key 各门,不串房。
    CHECK(*workspace::index::ResolveDirByWorkspaceKey(workspaces, identity_a.workspace_key) ==
          dir_a);
    CHECK(*workspace::index::ResolveDirByWorkspaceKey(workspaces, identity_b.workspace_key) ==
          dir_b);
}

TEST_CASE("账本制: 门牌生成——slug 变换逐字,哈希段取 seed 前 8") {
    // 样例口径(单子 §一):盘符 D: → D--,分隔符与点折 '-',大小写保留。
    CHECK(workspace::index::PathSlug("D:/MinerU/2604.10547v2") == "D--MinerU-2604-10547v2");
    CHECK(workspace::index::PathSlug("d:/lubancode") == "d--lubancode");
    CHECK(workspace::index::PathSlug("/home/user/demo_repo") == "-home-user-demo_repo");
    // 非法字符与控制符 → '_'。
    CHECK(workspace::index::PathSlug("D:/a<b>|c*d?e\"f") == "D--a_b__c_d_e_f");
    CHECK(workspace::index::PathSlug("D:/x\ty") == "D--x_y");
    // 中文/Unicode 原样保留(UTF-8 透传)。
    CHECK(workspace::index::PathSlug("D:/项目/中文仓库") == "D--项目-中文仓库");
    // Windows 保留名:前缀 '_'。
    CHECK(workspace::index::PathSlug("CON") == "_CON");
    CHECK(workspace::index::PathSlug("com3") == "_com3");
    CHECK(workspace::index::PathSlug("D:/normal") != "_D--normal");  // 常规名不误伤

    // 门牌 = slug + '-' + key 尾 16 hex 的前 8(identity 的真形状)。slug 源
    // 是真实绝对路径(POSIX 会把 "D:/x" 当相对串接 cwd,不能用裸样例造
    // identity);源文本与实现同一机械(绝对 + 规范 + 正斜杠 + 去尾),测试
    // 侧镜像这五步。
    const fs::path proj = TempRoot("plaque") / "proj-x";
    workspace::WorkspaceIdentity identity;
    identity.identity_kind = "cwd_fallback";
    identity.project_root = proj;
    identity.checkout_root = proj;
    identity.identity_root = proj;
    identity.launch_cwd = proj;
    identity.workspace_key =
        workspace::ComputeWorkspaceKeyFromSeed("path:" + workspace::NormalizeIdentityPathText(proj),
                                               "proj-x");
    std::string slug_source = platform::PathToUtf8(fs::absolute(proj).lexically_normal());
    for (char& c : slug_source) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (slug_source.size() > 1 && slug_source.back() == '/') {
        slug_source.pop_back();
    }
    const std::string plaque = workspace::index::MakeWorkspaceDirName(identity);
    CHECK(plaque == workspace::index::PathSlug(slug_source) + "-" +
                        identity.workspace_key.substr(identity.workspace_key.size() - 16, 8));
    CHECK(plaque != identity.workspace_key);
    // 纯函数:同 identity 恒同名。
    CHECK(workspace::index::MakeWorkspaceDirName(identity) == plaque);
}

TEST_CASE("账本制: 超长路径截断——slug 守平台帽,哈希段保唯一") {
    // 两条只差末段的长路径:slug 同样截到平台帽,门牌靠哈希段分家。帽分
    // 两档(见 index.cpp kSlugMaxBytes):POSIX 80——无 MAX_PATH 之虞,漂亮
    // 优先;Windows 40——门牌 ≤49,给 session/artifacts 深巢留 MAX_PATH
    //(文件 259/目录 247)预算。
#ifdef _WIN32
    constexpr std::size_t kSlugCap = 40;
#else
    constexpr std::size_t kSlugCap = 80;
#endif
    const std::string long_head(140, 'x');
    workspace::WorkspaceIdentity a;
    a.identity_kind = "cwd_fallback";
    a.project_root = platform::Utf8ToPath("D:/" + long_head + "/alpha");
    a.checkout_root = a.project_root;
    a.identity_root = a.project_root;
    a.launch_cwd = a.project_root;
    a.workspace_key = workspace::ComputeWorkspaceKeyFromSeed("path:a", "alpha");
    workspace::WorkspaceIdentity b = a;
    b.project_root = platform::Utf8ToPath("D:/" + long_head + "/bravo");
    b.checkout_root = b.project_root;
    b.identity_root = b.project_root;
    b.launch_cwd = b.project_root;
    b.workspace_key = workspace::ComputeWorkspaceKeyFromSeed("path:b", "bravo");

    const std::string plaque_a = workspace::index::MakeWorkspaceDirName(a);
    const std::string plaque_b = workspace::index::MakeWorkspaceDirName(b);
    REQUIRE(plaque_a.size() <= kSlugCap + 1 + 8);
    REQUIRE(plaque_b.size() <= kSlugCap + 1 + 8);
    CHECK(plaque_a != plaque_b);  // 截断撞 slug,哈希段拆开
    // 截断不落进 UTF-8 序列中间:字节串合法(连续字节必成对有头)。
    const std::string chinese_tail(60, '\xe4');  // 每字节都是 UTF-8 连续位
    const std::string slug = workspace::index::PathSlug("D:/" + chinese_tail);
    CHECK(slug.size() <= kSlugCap);
    CHECK(slug.find_first_not_of("D-\xe4") == std::string::npos);  // 只剩合法材料
}

TEST_CASE("账本制: 深根长径——门牌守平台预算,巢底三样落得下") {
    // 账本制批 Windows 五红的回归面(2026-09 CI 34046281141):门牌帽没给
    // MAX_PATH 留预算时,深 home + 长项目路径会让 session 巢底三样全数
    // 打不开——workflow run 目录链(create_directories 撞目录 247 顶)、
    // blob 临时文件(_wfsopen 撞文件 259 顶)、node 账文件。本册造一只
    // 深 home + 超帽长路径项目,把三样各落一次;帽回涨或巢再加深,这里
    // 当场翻红。
    //
    // 夹具根要贴着 CI 真实深度(临时根到 workspaces ~69 字符),不比它
    // 更深:产品保证的是门牌 ≤49 + 帽后预算放得下 CI 级深根;根比这更深
    // 爆的是 OS 的 260 硬顶,不是门牌帽的账——那种现场只能靠长路径支持,
    // 不在本册钉的范围。故不用共享 TempRoot(前缀长),另起短根。
    static int budget_sequence = 0;
    static const auto budget_run_id =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path root =
        fs::temp_directory_path() / ("lws-" + std::to_string(budget_run_id % 100000) + "-budget-" +
                                     std::to_string(++budget_sequence));
    std::error_code root_ec;
    fs::remove_all(root, root_ec);
    fs::create_directories(root, root_ec);
    const std::string deep_segment(60, 'd');
    const fs::path repo =
        root / "very-deep-home-tree" / deep_segment / "a-project-with-a-long-name";
    MakeRepo(repo);

    runtime::TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = repo;
    options.launch_cwd = repo.generic_string();
    options.lubancode_version = "index-test";
    auto ledger = runtime::TrajectorySessionLedger::Open(std::move(options));
    REQUIRE(ledger.has_value());

    // 门牌守帽:POSIX ≤89,Windows ≤49(slug 帽 40 + 连字符 + 哈希 8)。
    const auto identity = workspace::ResolveWorkspaceIdentity(repo, {}).value();
    const std::string plaque = workspace::index::MakeWorkspaceDirName(identity);
    constexpr std::size_t kPlaqueCap =
#ifdef _WIN32
        49;
#else
        89;
#endif
    CHECK(plaque.size() <= kPlaqueCap);
    CHECK(plaque.size() > 9);  // 不是光杆哈希:slug 面子还在

    // 巢底三样全落得下(Windows 上任何一样超预算都会当场报错)。
    auto directory = trajectory::TrajectoryDirectory::OpenExisting(ledger->session_dir());
    const auto run_stream = directory.ReserveWorkflowRun("wf-run-0001");
    REQUIRE(run_stream.has_value());
    CHECK(fs::exists(run_stream->parent_path() / "checkpoints"));
    const auto node_stream =
        directory.ReserveWorkflowNodeStream("wf-run-0001", "wf-run-0001-setup-d1-a1");
    REQUIRE(node_stream.has_value());
    trajectory::BlobStore blobs(ledger->session_dir() / "artifacts");
    const std::string body(700, 'x');
    const auto stored = blobs.Store(body, "text/plain", trajectory::Durability::ProcessCrash);
    REQUIRE(stored.has_value());
    const auto read_back = blobs.ReadVerified(*stored);
    REQUIRE(read_back.has_value());
    CHECK(*read_back == body);
}

TEST_CASE("账本制: 账本删除/写坏——扫房自愈,会话照常恢复") {
    const fs::path root = TempRoot("corrupt");
    const fs::path workspaces = root / "workspaces";
    const fs::path repo = root / "repo";
    MakeRepo(repo);
    const auto identity = workspace::ResolveWorkspaceIdentity(repo, {}).value();

    // 开一场真 session(经账本开房)。
    std::string session_id;
    {
        runtime::TrajectorySessionLedger::Options options;
        options.workspaces_root = workspaces;
        options.workspace_root = repo;
        options.launch_cwd = repo.generic_string();
        options.lubancode_version = "index-test";
        auto ledger = runtime::TrajectorySessionLedger::Open(std::move(options));
        REQUIRE(ledger.has_value());
        session_id = ledger->session_id();
    }
    const fs::path room = *workspace::index::ResolveDirByWorkspaceKey(workspaces,
                                                                      identity.workspace_key);
    const fs::path index_path = workspaces / "index.json";
    REQUIRE(fs::exists(index_path));

    for (const auto& [label, poison] : std::vector<std::pair<std::string, std::string>>{
             {"删除", "\x01-delete"},
             {"空文件", ""},
             {"截断 JSON", ReadAll(index_path).substr(0, 13)},
             {"坏 JSON", "not json at all{{{"},
         }) {
        // 投毒。
        if (poison == "\x01-delete") {
            std::error_code ec;
            fs::remove(index_path, ec);
        } else {
            Write(index_path, poison);
        }

        // 房还在,session 索引照常列出(各房 manifest 自描述,不靠账本)。
        trajectory::SessionIndexQuery query;
        query.current_workspace_key = identity.workspace_key;
        const auto page = trajectory::QueryWorkspaceSessions(workspaces, query);
        REQUIRE(page.entries.size() == 1);
        CHECK(page.entries[0].session_id == session_id);

        // 反查房门也照常(manifest 匹配,不靠账本)。
        CHECK(*workspace::index::ResolveDirByWorkspaceKey(workspaces, identity.workspace_key) ==
              room);

        // 重开:账本重建(扫房),同门牌回账。
        const fs::path reopened = OpenRoom(workspaces, repo);
        CHECK(reopened == room);
        const nlohmann::json ledger = ReadLedger(workspaces);
        CHECK(ledger["workspaces"]
                     [workspace::NormalizeIdentityPathText(identity.identity_root)]["dir"] ==
              platform::PathToUtf8(room.filename()));
        MESSAGE("账本" << label << "后自愈通过");
    }
}

TEST_CASE("账本制: 并发开房——同路径一间房,异路径各开各,丢账自愈") {
    const fs::path root = TempRoot("concurrent");
    const fs::path workspaces = root / "workspaces";

    // 8 条不同路径并发 miss:各生成各的门牌,最终账本收齐 8 笔。
    std::vector<fs::path> projects;
    for (int i = 0; i < 8; ++i) {
        projects.push_back(root / ("repo-" + std::to_string(i)));
        MakeRepo(projects.back());
    }
    std::vector<std::thread> openers;
    for (const fs::path& project : projects) {
        openers.emplace_back([&workspaces, &project] {
            std::error_code ec;
            fs::create_directories(workspaces, ec);  // 首开前根可能还没建
            const auto identity = workspace::ResolveWorkspaceIdentity(project, {}).value();
            fs::path dir;
            const auto registered =
                workspace::OpenOrRegisterWorkspace(workspaces, identity, 1000, nullptr, &dir);
            REQUIRE(registered.has_value());
        });
    }
    for (auto& opener : openers) {
        opener.join();
    }
    CHECK(RoomCount(workspaces) == 8);
    // 账本解得开、收得齐(读-改-写可能丢笔,丢了的下一段验自愈)。
    for (const fs::path& project : projects) {
        const auto identity = workspace::ResolveWorkspaceIdentity(project, {}).value();
        const std::string key = workspace::NormalizeIdentityPathText(identity.identity_root);
        if (ReadLedger(workspaces)["workspaces"].contains(key)) {
            continue;
        }
        // 丢账(并发后写挤掉前写):重开同路径,门牌确定性开回原房,回账。
        const fs::path reopened = OpenRoom(workspaces, project);
        CHECK(ReadLedger(workspaces)["workspaces"][key]["dir"] ==
              platform::PathToUtf8(reopened.filename()));
    }

    // 同一路径 8 只手并发:门牌纯函数,恒开同一间房,绝无二房。
    const fs::path same = root / "same-repo";
    MakeRepo(same);
    std::vector<std::thread> same_openers;
    for (int i = 0; i < 8; ++i) {
        same_openers.emplace_back([&workspaces, &same] {
            const auto identity = workspace::ResolveWorkspaceIdentity(same, {}).value();
            fs::path dir;
            const auto registered =
                workspace::OpenOrRegisterWorkspace(workspaces, identity, 2000, nullptr, &dir);
            REQUIRE(registered.has_value());
        });
    }
    for (auto& opener : same_openers) {
        opener.join();
    }
    CHECK(RoomCount(workspaces) == 9);
}

#ifdef _WIN32
TEST_CASE("账本制: Windows junction 进出——查账同门同房") {
    const fs::path root = TempRoot("junction");
    const fs::path workspaces = root / "workspaces";
    const fs::path repo = root / "real-repo";
    const fs::path junction = root / "junction-repo";
    MakeRepo(repo);
    const std::string command = "cmd /c mklink /J \"" + junction.string() + "\" \"" +
                                repo.string() + "\" >NUL 2>&1";
    if (std::system(command.c_str()) != 0 || !fs::exists(junction)) {
        MESSAGE("SKIP: junction 造不出,本机此腿留 CI");
        return;
    }
    const fs::path direct = OpenRoom(workspaces, repo);
    const fs::path via_junction = OpenRoom(workspaces, junction);
    CHECK(direct == via_junction);  // 解链后同 identity_root,查账同门
    CHECK(RoomCount(workspaces) == 1);
}
#endif

TEST_CASE("账本制: 大小写折叠——不敏感盘同键同房,敏感盘两房") {
    const fs::path root = TempRoot("case");
    const fs::path workspaces = root / "workspaces";
    const bool case_sensitive = ProbeCaseSensitive(root);
    const fs::path upper = root / "Demo-Repo";
    const fs::path lower = root / "demo-repo";
    MakeRepo(upper);
    if (case_sensitive) {
        MakeRepo(lower);
    }
    const fs::path dir_upper = OpenRoom(workspaces, upper);
    const fs::path dir_lower = OpenRoom(workspaces, lower);
    if (case_sensitive) {
        CHECK(dir_upper != dir_lower);
        CHECK(RoomCount(workspaces) == 2);
    } else {
        CHECK(dir_upper == dir_lower);  // Windows 折叠:同键同门同房
        CHECK(RoomCount(workspaces) == 1);
    }
}

TEST_CASE("账本制: worktree 共账——同键同门牌同房,账本一笔") {
    const fs::path root = TempRoot("worktree");
    const fs::path workspaces = root / "workspaces";
    const fs::path repo = root / "demo-repo";
    const fs::path worktree = root / "demo-repo-wt";
    MakeRepo(repo);
    MakeLinkedWorktree(repo, worktree, "wt");

    const auto main_identity = workspace::ResolveWorkspaceIdentity(repo, {}).value();
    const auto wt_identity = workspace::ResolveWorkspaceIdentity(worktree, {}).value();
    REQUIRE(main_identity.workspace_key == wt_identity.workspace_key);
    REQUIRE(main_identity.git_common_dir == wt_identity.git_common_dir);

    const fs::path main_room = OpenRoom(workspaces, repo);
    const fs::path wt_room = OpenRoom(workspaces, worktree);
    CHECK(main_room == wt_room);  // git common dir 同键 → 查账同门
    CHECK(RoomCount(workspaces) == 1);

    // 账本恰好一笔(键 = common dir 归一),checkouts 双登记在 manifest。
    const nlohmann::json ledger = ReadLedger(workspaces);
    CHECK(ledger["workspaces"].size() == 1);
    const auto manifest = workspace::ReadWorkspaceManifest(main_room);
    REQUIRE(manifest.status == workspace::ManifestRead::Status::Ok);
    REQUIRE(manifest.manifest.checkouts.size() == 2);

    // 账本坏后重建:worktree 房照旧一笔,不裂成两间。
    Write(workspaces / "index.json", "{torn");
    const fs::path healed = OpenRoom(workspaces, worktree);
    CHECK(healed == main_room);
    CHECK(RoomCount(workspaces) == 1);
}
