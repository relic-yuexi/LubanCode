// P0-1:WorkspaceIdentityResolver 的裁决矩阵与 key 同异账。
//
// 单子 §十一 11.1 身份矩阵逐条落:Git 根/子目录同 key、linked worktree 共
// workspace、独立 clone 不共享、同名不同路径不撞、Windows 盘符大小写斜杠
// 归一、嵌套仓最近者胜、marker/config/cwd fallback 三级递退。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "memory/project_memory.hpp"  // ResolveProjectIdentity:两把钥匙统一对账
#include "workspace/identity.hpp"
#include "workspace/storage_contracts.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempRoot(const std::string& name) {
    const fs::path root = fs::temp_directory_path() / ("lubancode-ws-identity-" + name);
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

// 造一间最简 Git 仓(.git 目录即 common git dir)。
void MakeRepo(const fs::path& repo) {
    fs::create_directories(repo / ".git");
}

// 造一间挂在 main 仓名下的 linked worktree(.git 文件 + commondir 引用)。
void MakeLinkedWorktree(const fs::path& main_repo, const fs::path& worktree, const std::string& name) {
    const fs::path git_dir = main_repo / ".git" / "worktrees" / name;
    fs::create_directories(git_dir);
    fs::create_directories(worktree);
    Write(worktree / ".git", "gitdir: " + git_dir.generic_string() + "\n");
    Write(git_dir / "commondir", "../..\n");
}

}  // namespace

TEST_CASE("身份:Git 根、子目录、深层目录同一把钥匙") {
    const fs::path root = TempRoot("git-same-key");
    const fs::path repo = root / "demo-repo";
    MakeRepo(repo);
    fs::create_directories(repo / "src" / "deeply" / "nested");

    const auto from_root = workspace::ResolveWorkspaceIdentity(repo, root / "home");
    const auto from_sub = workspace::ResolveWorkspaceIdentity(repo / "src", root / "home");
    const auto from_deep = workspace::ResolveWorkspaceIdentity(repo / "src" / "deeply" / "nested",
                                                               root / "home");
    REQUIRE(from_root.has_value());
    REQUIRE(from_sub.has_value());
    REQUIRE(from_deep.has_value());
    CHECK(from_root->workspace_key == from_sub->workspace_key);
    CHECK(from_root->workspace_key == from_deep->workspace_key);
    // key 逐字相同,且身份字段同源。
    CHECK(from_root->identity_kind == std::string(workspace::contracts::kIdentityKindGitCommon));
    CHECK(from_root->git_common_dir == fs::weakly_canonical(repo / ".git"));
    CHECK(from_root->checkout_root == fs::weakly_canonical(repo));
    CHECK(from_deep->checkout_root == fs::weakly_canonical(repo));  // checkout 根不跟 cwd 走
    CHECK(from_deep->launch_cwd == fs::weakly_canonical(repo / "src" / "deeply" / "nested"));
}

TEST_CASE("身份:主树与 linked worktree 共 workspace,checkout 各记各的") {
    const fs::path root = TempRoot("worktree");
    const fs::path repo = root / "demo-repo";
    MakeRepo(repo);
    const fs::path worktree = root / "wt-feature";
    MakeLinkedWorktree(repo, worktree, "wt-feature");

    const auto main_identity = workspace::ResolveWorkspaceIdentity(repo, root / "home");
    const auto wt_identity = workspace::ResolveWorkspaceIdentity(worktree / "src", root / "home");
    REQUIRE(main_identity.has_value());
    REQUIRE(wt_identity.has_value());
    CHECK(main_identity->workspace_key == wt_identity->workspace_key);
    CHECK(main_identity->identity_root == wt_identity->identity_root);
    CHECK(wt_identity->checkout_root == fs::weakly_canonical(worktree));
    CHECK(main_identity->checkout_root != wt_identity->checkout_root);
}

TEST_CASE("身份:两份独立 clone 不共享,同名目录也不撞") {
    const fs::path root = TempRoot("clones");
    MakeRepo(root / "clone-a" / "demo");
    MakeRepo(root / "clone-b" / "demo");

    const auto a = workspace::ResolveWorkspaceIdentity(root / "clone-a" / "demo", root / "home");
    const auto b = workspace::ResolveWorkspaceIdentity(root / "clone-b" / "demo", root / "home");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->workspace_key != b->workspace_key);
    CHECK(a->display_name == b->display_name);  // basename 只给人看,不参与唯一性

    const auto a_root = workspace::ResolveWorkspaceIdentity(root / "clone-a" / "demo", root / "home2");
    CHECK(a_root->workspace_key == a->workspace_key);  // home 不参与 key
}

TEST_CASE("身份:Windows 盘符/大小写/斜杠归一;POSIX 大小写敏感") {
    const fs::path root = TempRoot("case");
    const auto identity = workspace::MakeFallbackIdentity(fs::path("D:/Work/Demo"));
    const std::string key = identity.workspace_key;
    CHECK(!key.empty());
    CHECK(key.size() == std::string("Demo-").size() + 16);
#ifdef _WIN32
    CHECK(workspace::MakeFallbackIdentity(fs::path("d:/work/demo/")).workspace_key == key);
    CHECK(workspace::MakeFallbackIdentity(fs::path("D:\\work\\demo")).workspace_key == key);
#else
    CHECK(workspace::MakeFallbackIdentity(fs::path("D:/Work/Demo/")).workspace_key == key);
    CHECK(workspace::MakeFallbackIdentity(fs::path("D:/work/demo")).workspace_key != key);
#endif
#ifdef _WIN32
    CHECK(workspace::NormalizeIdentityPathText(fs::path("D:\\a\\/b\\\\")) == "d:/a/b");
#else
    // POSIX 保留大小写,只归一尾斜杠。
    CHECK(workspace::NormalizeIdentityPathText(fs::path("/Tmp/Repo/")) == "/Tmp/Repo");
    CHECK(workspace::NormalizeIdentityPathText(fs::path("/tmp/repo/")) == "/tmp/repo");
#endif
}

TEST_CASE("身份:嵌套仓最近边界胜,不被外层吞掉") {
    const fs::path root = TempRoot("nested");
    const fs::path outer = root / "outer";
    const fs::path inner = outer / "vendor" / "inner";
    MakeRepo(outer);
    MakeRepo(inner);

    const auto from_inner = workspace::ResolveWorkspaceIdentity(inner / "src", root / "home");
    const auto from_outer = workspace::ResolveWorkspaceIdentity(outer / "docs", root / "home");
    REQUIRE(from_inner.has_value());
    REQUIRE(from_outer.has_value());
    CHECK(from_inner->git_common_dir == fs::weakly_canonical(inner / ".git"));
    CHECK(from_inner->workspace_key != from_outer->workspace_key);
    // submodule 形状(.git 文件指去父仓 modules/,无 commondir):身份独立。
    const fs::path sub = outer / "submodule-checkout";
    const fs::path sub_gitdir = outer / ".git" / "modules" / "sub";
    fs::create_directories(sub_gitdir);
    fs::create_directories(sub);
    Write(sub / ".git", "gitdir: " + sub_gitdir.generic_string() + "\n");
    const auto sub_identity = workspace::ResolveWorkspaceIdentity(sub, root / "home");
    REQUIRE(sub_identity.has_value());
    CHECK(sub_identity->workspace_key != from_outer->workspace_key);
    CHECK(sub_identity->git_common_dir == fs::weakly_canonical(sub_gitdir));
}

TEST_CASE("身份:非 Git 三级递退——marker、config、cwd fallback") {
    const fs::path root = TempRoot("fallback");
    const fs::path marker_dir = root / "marked" / "proj";
    fs::create_directories(marker_dir / "src");
    Write(marker_dir / ".lubancode" / "workspace.json",
          "{\"schema\": \"lubancode.workspace.marker\", \"workspace_id\": \"team-monorepo-42\"}");

    const auto marker = workspace::ResolveWorkspaceIdentity(marker_dir / "src", root / "home");
    REQUIRE(marker.has_value());
    CHECK(marker->identity_kind == std::string(workspace::contracts::kIdentityKindExplicitMarker));
    CHECK(marker->project_root == fs::weakly_canonical(marker_dir));
    // marker 的 seed 是声明 id:同 id 的两处目录同 key(团队显式并账语义)。
    const fs::path marker_twin = root / "elsewhere" / "other-name";
    fs::create_directories(marker_twin);
    Write(marker_twin / ".lubancode" / "workspace.json",
          "{\"workspace_id\": \"team-monorepo-42\"}");
    const auto twin = workspace::ResolveWorkspaceIdentity(marker_twin, root / "home");
    REQUIRE(twin.has_value());
    CHECK(twin->workspace_key == marker->workspace_key);

    // config 级:没有 marker,取最近 .lubancode/config.json 所在目录。
    const fs::path config_dir = root / "plain" / "proj";
    fs::create_directories(config_dir / "src" / "deep");
    Write(config_dir / ".lubancode" / "config.json", "{\"memory\": {\"enabled\": true}}");
    const auto config = workspace::ResolveWorkspaceIdentity(config_dir / "src" / "deep",
                                                            root / "home");
    REQUIRE(config.has_value());
    CHECK(config->identity_kind == std::string(workspace::contracts::kIdentityKindConfigRoot));
    CHECK(config->project_root == fs::weakly_canonical(config_dir));

    // 四处皆无:cwd fallback。祖先链上那层带 .lubancode/config.json 的目录
    // 是"用户主目录"——传了 home 就在它这儿止步,不把它吸成大项目(§4.2
    // 第 5 级);没传 home 才认它为 config 边界(向后兼容的旧口径)。
    const fs::path fake_user = root / "userhome";
    fs::create_directories(fake_user / ".lubancode");
    Write(fake_user / ".lubancode" / "config.json", "{}");
    const fs::path nowhere = fake_user / "deep" / "proj";
    fs::create_directories(nowhere);
    const auto fallback =
        workspace::ResolveWorkspaceIdentity(nowhere, fake_user / ".lubancode");
    REQUIRE(fallback.has_value());
    CHECK(fallback->identity_kind == std::string(workspace::contracts::kIdentityKindCwdFallback));
    CHECK(fallback->project_root == fs::weakly_canonical(nowhere));
    // 不递 home:同一层 .lubancode/config.json 按最近 config 边界认。
    const auto no_home = workspace::ResolveWorkspaceIdentity(nowhere, {});
    REQUIRE(no_home.has_value());
    CHECK(no_home->identity_kind == std::string(workspace::contracts::kIdentityKindConfigRoot));
    CHECK(no_home->project_root == fs::weakly_canonical(fake_user));
}

TEST_CASE("身份:坏 marker 不开门,滑到下一级") {
    const fs::path root = TempRoot("bad-marker");
    const fs::path proj = root / "proj";
    fs::create_directories(proj / "src");
    Write(proj / ".lubancode" / "workspace.json", "not-json-at-all");
    Write(proj / ".lubancode" / "config.json", "{}");

    const auto identity = workspace::ResolveWorkspaceIdentity(proj / "src", root / "home");
    REQUIRE(identity.has_value());
    CHECK(identity->identity_kind == std::string(workspace::contracts::kIdentityKindConfigRoot));

    CHECK_FALSE(workspace::ReadMarkerWorkspaceId(proj / ".lubancode" / "workspace.json").has_value());
}

TEST_CASE("身份:Git 优先于更近的 marker/config(§4.2 级联顺序)") {
    const fs::path root = TempRoot("git-first");
    const fs::path repo = root / "repo";
    MakeRepo(repo);
    Write(repo / ".lubancode" / "workspace.json", "{\"workspace_id\": \"inside-repo-id\"}");

    const auto identity = workspace::ResolveWorkspaceIdentity(repo / "src", root / "home");
    REQUIRE(identity.has_value());
    CHECK(identity->identity_kind == std::string(workspace::contracts::kIdentityKindGitCommon));
}

TEST_CASE("身份:key 形状与统一算法的稳定样例") {
    // seed 前缀三选一;display 名清洗走 SafeName 语义(非法字节折 '-')。
    const std::string key = workspace::ComputeWorkspaceKeyFromSeed("git:/x/y/.git", "demo repo");
    // 显示名 "demo repo" 的空格折成 '-':前缀 demo-repo 连 hash 前 16 位。
    CHECK(key.substr(0, 10) == "demo-repo-");
    CHECK(key.size() == 10 + 16);
    // 同 seed 同名逐字稳定(跨进程跨平台同口径)。
    CHECK(workspace::ComputeWorkspaceKeyFromSeed("git:/x/y/.git", "demo repo") == key);
    CHECK(workspace::ComputeWorkspaceKeyFromSeed("marker:team-42", "demo repo") != key);
    // 显示名空串兜 "project"。
    CHECK(workspace::ComputeWorkspaceKeyFromSeed("path:/a/b", "").substr(0, 8) == "project-");
}

TEST_CASE("身份:两把旧钥匙已统一——memory 侧 ProjectIdentity 同 key 同根") {
    // P0-1 验收线:同一 cwd 从任一入口所得 key 逐字相同。memory 的
    // ResolveProjectIdentity 已是 workspace resolver 的薄适配,这里把两把
    // 钥匙摆在一起逐字对(trajectory 侧 key 由 SessionManager 吃同一
    // identity 出,见 test_workspace_switch 册)。
    const fs::path root = TempRoot("unified");
    MakeRepo(root / "demo-repo");
    MakeLinkedWorktree(root / "demo-repo", root / "demo-repo-wt", "wt");

    const auto identity = workspace::ResolveWorkspaceIdentity(root / "demo-repo" / "src", {}).value();
    const auto memory_identity =
        memory::ResolveProjectIdentity(root / "demo-repo" / "src", root / "home").value();
    CHECK(memory_identity.key == identity.workspace_key);
    CHECK(memory_identity.project_root == identity.project_root);
    CHECK(memory_identity.common_root == identity.identity_root);
    CHECK(memory_identity.git);

    // linked worktree:memory 侧也共 key(旧算法修过的裂口,新算法天然不裂)。
    const auto wt_memory = memory::ResolveProjectIdentity(root / "demo-repo-wt", root / "home").value();
    CHECK(wt_memory.key == memory_identity.key);
    CHECK(wt_memory.project_dir == memory_identity.project_dir);  // 换钥匙不换房:projects/<key>/
}
