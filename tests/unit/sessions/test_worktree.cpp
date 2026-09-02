#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <string>
#include <vector>

#include "runtime/worktree.hpp"
#include "platform/process.hpp"

using namespace lubancode;


// macOS 的 /var 是 /private/var 的符号链接:临时目录路径带着 /var 进去,
// chdir 后 current_path() 出来已是 /private/var,裸等值必翻。断言两边都
// 过一遍 canonical 再比(路径不存在时原样退回,给"拒进"类负例留活路)。
inline std::filesystem::path NormalizedPath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::canonical(path, ec);
    return ec ? path : canonical;
}

TEST_CASE("ParseWorktreeCommand: new/list/exit 的参数分清") {
    const auto named = cli::ParseWorktreeCommand("new feature_54");
    CHECK(named.action == cli::WorktreeAction::New);
    CHECK(named.name == "feature_54");

    const auto random = cli::ParseWorktreeCommand("new");
    CHECK(random.action == cli::WorktreeAction::New);
    CHECK(random.name.empty());

    CHECK(cli::ParseWorktreeCommand("list").action == cli::WorktreeAction::List);
    const auto leave = cli::ParseWorktreeCommand("exit remove");
    CHECK(leave.action == cli::WorktreeAction::Exit);
    CHECK(leave.exit_mode == "remove");
    CHECK(cli::ParseWorktreeCommand("remove").action == cli::WorktreeAction::Invalid);
}

TEST_CASE("WorktreePath: 固定落在仓库 .lubancode/worktrees 下") {
    const std::filesystem::path repository = std::filesystem::path("D:/repo with space");
    CHECK(cli::WorktreePath(repository, "fix_54") ==
          repository / ".lubancode" / "worktrees" / "fix_54");
}

TEST_CASE("ValidateWorktreeName: 不让路径和 Git ref 特殊字符混进名字") {
    CHECK_FALSE(cli::ValidateWorktreeName("fix-54_2").has_value());
    CHECK(cli::ValidateWorktreeName("../escape").has_value());
    CHECK(cli::ValidateWorktreeName("feature/name").has_value());
    CHECK(cli::ValidateWorktreeName("two words").has_value());
}

TEST_CASE("WorktreeSession: 非 Git 仓库经 mock 给清楚结果") {
    std::vector<cli::GitCommand> commands;
    cli::WorktreeSession session([&commands](const cli::GitCommand& command) {
        commands.push_back(command);
        return cli::GitCommandResult{128, "fatal: not a git repository", std::string()};
    });

    const auto result = session.List();
    CHECK(result.code == cli::WorktreeResultCode::NotRepository);
    REQUIRE(commands.size() == 1);
    CHECK((commands.front().args == std::vector<std::string>{"rev-parse", "--show-toplevel"}));
}

TEST_CASE("CurrentGitBranch: 普通分支取短名") {
    std::vector<cli::GitCommand> commands;
    const std::string branch = cli::CurrentGitBranch("D:/repo", [&commands](const cli::GitCommand& command) {
        commands.push_back(command);
        return cli::GitCommandResult{0, "feature/status-panel\n", {}};
    });
    CHECK(branch == "feature/status-panel");
    REQUIRE(commands.size() == 1);
    CHECK((commands[0].args == std::vector<std::string>{"symbolic-ref", "--quiet", "--short", "HEAD"}));
}

TEST_CASE("CurrentGitBranch: 游离 HEAD 回退短哈希，不在仓库返回空") {
    int calls = 0;
    const std::string detached = cli::CurrentGitBranch("D:/repo", [&calls](const cli::GitCommand&) {
        ++calls;
        return calls == 1 ? cli::GitCommandResult{1, {}, {}}
                          : cli::GitCommandResult{0, "abc1234\n", {}};
    });
    CHECK(detached == "detached@abc1234");

    CHECK(cli::CurrentGitBranch("D:/plain", [](const cli::GitCommand&) {
              return cli::GitCommandResult{128, {}, {}};
          }).empty());
}

// ---------------------------------------------------------------------------
// 0.27.x:模型侧 worktree 工具的房务(cli 层新函数)
// ---------------------------------------------------------------------------

namespace {

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 测试期进程 cwd 的 RAII 守卫:Enter/Exit 会 chdir,收尾必须还原,
// 别把别的测试带沟里。
struct CwdGuard {
    std::filesystem::path saved;
    CwdGuard() : saved(std::filesystem::current_path()) {}
    ~CwdGuard() {
        std::error_code ec;
        std::filesystem::current_path(saved, ec);
    }
};

// 摆一间"验明正身能过"的房:主树 .git/worktrees/<name>/gitdir 反向登记
// + 房内 .git 文件指回去。
void FabricateWorktree(const std::filesystem::path& main_root, const std::string& name) {
    const std::filesystem::path room = cli::WorktreePath(main_root, name);
    std::filesystem::create_directories(room);
    const std::filesystem::path admin = main_root / ".git" / "worktrees" / name;
    std::filesystem::create_directories(admin);
    std::ofstream(room / ".git") << "gitdir: " << PathToUtf8(admin) << "\n";
    std::ofstream(admin / "gitdir") << PathToUtf8(room) << "\n";
}

}  // namespace

TEST_CASE("WorktreeIncludeMatches: glob、目录锚定、** 与文件名匹配") {
    CHECK(cli::WorktreeIncludeMatches(".env", ".env", false));
    CHECK(cli::WorktreeIncludeMatches("*.env", "prod.env", false));
    CHECK_FALSE(cli::WorktreeIncludeMatches("*.env", "prod.env.bak", false));
    // 不带斜杠:任意深度按名字
    CHECK(cli::WorktreeIncludeMatches("secrets.json", "config/secrets.json", false));
    // 带斜杠:仓库根锚定
    CHECK(cli::WorktreeIncludeMatches("config/*.key", "config/api.key", false));
    CHECK_FALSE(cli::WorktreeIncludeMatches("config/*.key", "other/api.key", false));
    CHECK_FALSE(cli::WorktreeIncludeMatches("config/*.key", "deep/config/api.key", false));
    // ** 跨段
    CHECK(cli::WorktreeIncludeMatches("**/*.pem", "a/b/c.pem", false));
    CHECK(cli::WorktreeIncludeMatches("certs/**/*.pem", "certs/x/y.pem", false));
    CHECK_FALSE(cli::WorktreeIncludeMatches("certs/**/*.pem", "other/x/y.pem", false));
    // 目录模式:目录本身与其下内容
    CHECK(cli::WorktreeIncludeMatches("keys/", "keys", true));
    CHECK(cli::WorktreeIncludeMatches("keys/", "keys/id_rsa", false));
    CHECK_FALSE(cli::WorktreeIncludeMatches("keys/", "keys", false));
    // 头 / 锚定
    CHECK(cli::WorktreeIncludeMatches("/local.env", "local.env", false));
    CHECK_FALSE(cli::WorktreeIncludeMatches("/local.env", "sub/local.env", false));
}

TEST_CASE("VerifyWorktreeIdentity: 独立 worktree 过,马甲房拒") {
    const auto base = std::filesystem::temp_directory_path() /
                      ("lubancode_wt_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path main_root = base / "repo";
    std::filesystem::create_directories(main_root / ".git");
    FabricateWorktree(main_root, "ok-room");

    CHECK_FALSE(cli::VerifyWorktreeIdentity(cli::WorktreePath(main_root, "ok-room"), main_root).has_value());

    // 主根本体
    CHECK(cli::VerifyWorktreeIdentity(main_root, main_root).has_value());
    // .git 是目录的普通目录(或嵌套完整仓)
    const std::filesystem::path nested = base / "nested";
    std::filesystem::create_directories(nested / ".git");
    CHECK(cli::VerifyWorktreeIdentity(nested, main_root).has_value());
    // .git 文件指向别家仓库
    const std::filesystem::path stranger = base / "stranger";
    std::filesystem::create_directories(stranger);
    std::ofstream(stranger / ".git") << "gitdir: D:/other/repo/.git/worktrees/x\n";
    CHECK(cli::VerifyWorktreeIdentity(stranger, main_root).has_value());
    // 反向登记对不上(登记指别的房)
    FabricateWorktree(main_root, "cross-room");
    const std::filesystem::path admin = main_root / ".git" / "worktrees" / "cross-room";
    std::ofstream(admin / "gitdir") << PathToUtf8(cli::WorktreePath(main_root, "other-room")) << "\n";
    CHECK(cli::VerifyWorktreeIdentity(cli::WorktreePath(main_root, "cross-room"), main_root).has_value());
    // 裸目录(没 .git)
    const std::filesystem::path bare = base / "bare";
    std::filesystem::create_directories(bare);
    CHECK(cli::VerifyWorktreeIdentity(bare, main_root).has_value());

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST_CASE("ResolveFreshBaseRef: 远端默认分支,补 fetch 5 秒封顶,失败回落 HEAD") {
    std::vector<cli::GitCommand> calls;
    cli::GitRunner runner = [&calls](const cli::GitCommand& command) {
        calls.push_back(command);
        if (command.args.front() == "symbolic-ref") {
            return cli::GitCommandResult{0, "refs/remotes/origin/main\n", {}};
        }
        if (command.args.front() == "fetch") {
            return cli::GitCommandResult{1, {}, "network down"};
        }
        if (command.args.front() == "rev-parse") {
            return cli::GitCommandResult{0, "abc1234\n", {}};
        }
        return cli::GitCommandResult{1, {}, {}};
    };
    const auto base = std::filesystem::temp_directory_path() /
                      ("lubancode_fresh_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(base);  // 无 .git/FETCH_HEAD → 视为陈货,补 fetch
    CHECK(cli::ResolveFreshBaseRef(base, runner) == "origin/main");
    REQUIRE(calls.size() == 3);
    CHECK((calls[0].args == std::vector<std::string>{"symbolic-ref", "--quiet", "refs/remotes/origin/HEAD"}));
    CHECK(calls[1].args.front() == "fetch");
    CHECK(calls[1].timeout_ms == 5000);
    CHECK(calls[2].args.front() == "rev-parse");

    // 远端信息拿不到:一次调用就回落 HEAD
    calls.clear();
    CHECK(cli::ResolveFreshBaseRef(base, [&calls](const cli::GitCommand& command) {
              calls.push_back(command);
              return cli::GitCommandResult{128, {}, "no origin"};
          }) == "HEAD");
    REQUIRE(calls.size() == 1);

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST_CASE("WorktreeSession::Enter/Exit: 建房、进旧房、干净删房、scope 钩子") {
    CwdGuard cwd_guard;
    const auto base = std::filesystem::temp_directory_path() /
                      ("lubancode_enter_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path repo = base / "repo";
    std::filesystem::create_directories(repo / ".git");

    // 假 git:按命令首词给结果。
    const auto runner = [&repo](const cli::GitCommand& command) -> cli::GitCommandResult {
        const std::string first = command.args.front();
        if (first == "rev-parse" && command.args.size() > 1 && command.args[1] == "--show-toplevel") {
            return {0, PathToUtf8(repo) + "\n", {}};
        }
        if (first == "worktree" && command.args.size() > 1 && command.args[1] == "list") {
            return {0, "worktree " + PathToUtf8(repo) + "\nbranch refs/heads/main\n", {}};
        }
        if (first == "symbolic-ref") {
            return {0, "worktree/fix-1\n", {}};
        }
        if (first == "status") {
            return {0, "", {}};
        }
        if (first == "worktree") {
            return {0, "", {}};
        }
        if (first == "branch") {
            return {0, "", {}};
        }
        return {0, "", {}};
    };

    cli::WorktreeSession session(runner);
    std::vector<std::string> hook_log;
    session.SetScopeHook([&hook_log](bool entered, const std::filesystem::path&, const std::filesystem::path&) {
        hook_log.push_back(entered ? "enter" : "exit");
    });

    std::filesystem::current_path(repo);
    FabricateWorktree(repo, "fix-1");  // 预摆一间旧房(已登记、可进)

    // Enter 进已有旧房(base=head,不动基准)
    const auto entered = session.Enter("fix-1", "head");
    REQUIRE(entered.code == cli::WorktreeResultCode::Created);
    CHECK(entered.branch == "worktree/fix-1");
    CHECK(session.active());
    CHECK(session.active_name() == "fix-1");
    CHECK(NormalizedPath(std::filesystem::current_path()) == NormalizedPath(cli::WorktreePath(repo, "fix-1")));
    REQUIRE(hook_log.size() == 1);
    CHECK(hook_log[0] == "enter");

    // status:在房里、干净
    const auto status = session.Status();
    CHECK(status.code == cli::WorktreeResultCode::Listed);
    CHECK(status.detail == "clean");
    CHECK(status.branch == "worktree/fix-1");

    // exit keep:回原目录,房还在
    const auto kept = session.Exit("keep");
    CHECK(kept.code == cli::WorktreeResultCode::Kept);
    CHECK(NormalizedPath(std::filesystem::current_path()) == NormalizedPath(repo));
    CHECK(std::filesystem::exists(cli::WorktreePath(repo, "fix-1")));
    REQUIRE(hook_log.size() == 2);
    CHECK(hook_log[1] == "exit");

    // 再进,exit remove(干净即删)
    REQUIRE(session.Enter("fix-1", "head").code == cli::WorktreeResultCode::Created);
    const auto removed = session.Exit("remove");
    CHECK(removed.code == cli::WorktreeResultCode::Removed);
    CHECK_FALSE(std::filesystem::exists(cli::WorktreePath(repo, "fix-1")));
    CHECK(NormalizedPath(std::filesystem::current_path()) == NormalizedPath(repo));
    REQUIRE(hook_log.size() == 4);
    CHECK(hook_log[3] == "exit");

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST_CASE("WorktreeSession::Enter: 园子外的房先问,确认后才进;脏房 remove 要确认") {
    CwdGuard cwd_guard;
    const auto base = std::filesystem::temp_directory_path() /
                      ("lubancode_outside_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path repo = base / "repo";
    std::filesystem::create_directories(repo / ".git");
    const std::filesystem::path outside = base / "user-room";
    std::filesystem::create_directories(outside);
    const std::filesystem::path outside_admin = repo / ".git" / "worktrees" / "user-room";
    std::filesystem::create_directories(outside_admin);
    std::ofstream(outside / ".git") << "gitdir: " << PathToUtf8(outside_admin) << "\n";
    std::ofstream(outside_admin / "gitdir") << PathToUtf8(outside) << "\n";

    bool dirty = false;
    const auto runner = [&repo, &outside, &dirty](const cli::GitCommand& command) -> cli::GitCommandResult {
        const std::string first = command.args.front();
        if (first == "rev-parse" && command.args.size() > 1 && command.args[1] == "--show-toplevel") {
            return {0, PathToUtf8(repo) + "\n", {}};
        }
        if (first == "worktree" && command.args.size() > 1 && command.args[1] == "list") {
            return {0, "worktree " + PathToUtf8(repo) + "\nbranch refs/heads/main\nworktree " + PathToUtf8(outside) +
                          "\nbranch refs/heads/user-room\n",
                    {}};
        }
        if (first == "symbolic-ref") {
            return {0, "user-room\n", {}};
        }
        if (first == "status") {
            return dirty ? cli::GitCommandResult{0, " M file\n", {}} : cli::GitCommandResult{0, "", {}};
        }
        return {0, "", {}};
    };
    cli::WorktreeSession session(runner);
    std::filesystem::current_path(repo);

    // 名字对上园子外的已有房:先要确认
    const auto ask = session.Enter("user-room", "head");
    CHECK(ask.code == cli::WorktreeResultCode::NeedsUserConfirmation);
    CHECK(ask.path == outside);
    CHECK_FALSE(session.active());
    // 点头重进
    const auto confirmed = session.Enter("user-room", "head", /*confirmed_outside=*/true);
    CHECK(confirmed.code == cli::WorktreeResultCode::Created);
    CHECK(NormalizedPath(std::filesystem::current_path()) == NormalizedPath(outside));

    // 脏房 exit remove:要确认;确认后删房,但别人的分支不动(没 branch 调用)
    dirty = true;
    const auto dirty_exit = session.Exit("remove");
    CHECK(dirty_exit.code == cli::WorktreeResultCode::NeedsRemoveConfirmation);
    CHECK(std::filesystem::exists(outside));  // 拒绝前房还在
    const auto removed = session.ConfirmRemove();
    CHECK(removed.code == cli::WorktreeResultCode::Removed);
    CHECK_FALSE(std::filesystem::exists(outside));

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST_CASE("WorktreeSession::EnterByPath: 马甲房(无登记)拒进") {
    CwdGuard cwd_guard;
    const auto base = std::filesystem::temp_directory_path() /
                      ("lubancode_by-path_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path repo = base / "repo";
    std::filesystem::create_directories(repo / ".git");
    const std::filesystem::path fake = cli::WorktreePath(repo, "fake");
    std::filesystem::create_directories(fake);  // 裸目录,没有 .git 登记
    std::filesystem::current_path(repo);

    cli::WorktreeSession session([&repo](const cli::GitCommand& command) {
        if (command.args.front() == "rev-parse") {
            return cli::GitCommandResult{0, PathToUtf8(repo) + "\n", {}};
        }
        return cli::GitCommandResult{0, "", {}};
    });
    const auto refused = session.EnterByPath(fake);
    CHECK(refused.code == cli::WorktreeResultCode::VerificationFailed);
    CHECK_FALSE(session.active());
    CHECK(NormalizedPath(std::filesystem::current_path()) == NormalizedPath(repo));  // 没被搬进去

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

#ifdef _WIN32
TEST_CASE("SafeRemoveTree: junction 只删链接,不追删指向的目录") {    const auto base = std::filesystem::temp_directory_path() /
                      ("lubancode_junction_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path room = base / "room";
    const std::filesystem::path target = base / "precious";
    std::filesystem::create_directories(room);
    std::filesystem::create_directories(target);
    std::ofstream(target / "keep.txt") << "data\n";

    const std::string cmd = "mklink /J \"" + PathToUtf8(room / "link") + "\" \"" + PathToUtf8(target) + "\"";
    const auto proc = platform::RunShellCommand(cmd, 15000);
    if (proc.exit_code != 0) {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        MESSAGE("mklink /J 失败,exit=", proc.exit_code, " output=", proc.output,
                " spawn_error=", proc.spawn_error);
        REQUIRE(false);
    }

    CHECK_FALSE(cli::SafeRemoveTree(room).has_value());
    CHECK_FALSE(std::filesystem::exists(room));
    // 链接指向的目录安然无恙
    CHECK(std::filesystem::exists(target / "keep.txt"));

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}
#endif

// ---------------------------------------------------------------------------
// 子代理房务:CreateAgentWorktree / FinishAgentWorktree / CleanStaleAgentWorktrees
// (真 git 临时仓库)
// ---------------------------------------------------------------------------

namespace {

platform::ProcessResult RunGitAt(const std::filesystem::path& root, std::vector<std::string> args) {
    std::vector<std::string> argv = {"git", "-C", PathToUtf8(root)};
    argv.insert(argv.end(), std::make_move_iterator(args.begin()), std::make_move_iterator(args.end()));
    return platform::RunProcess(argv, 60000);
}

// 真 git 临时仓库(init + 一次提交),给 worktree add 一个能用的基准。
struct RealGitRepo {
    std::filesystem::path root;

    RealGitRepo() {
        const auto base = std::filesystem::temp_directory_path() /
                          ("lubancode_room_" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        root = base / "repo";
        std::filesystem::create_directories(root);
        RunGitAt(root, {"init", "-q", "-b", "main"});
        RunGitAt(root, {"config", "user.email", "test@example.com"});
        RunGitAt(root, {"config", "user.name", "Test"});
        std::ofstream(root / "seed.txt") << "seed\n";
        RunGitAt(root, {"add", "."});
        RunGitAt(root, {"commit", "-q", "-m", "init"});
    }
    ~RealGitRepo() {
        std::error_code ec;
        std::filesystem::remove_all(root.parent_path(), ec);
    }

    void MakeOld(const std::filesystem::path& path) const {
        std::error_code ec;
        const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(100);
        std::filesystem::last_write_time(path, old, ec);
    }
};

}  // namespace

TEST_CASE("CreateAgentWorktree/FinishAgentWorktree: 建房上锁,收工干净删、有活留") {
    RealGitRepo repo;

    // 建房:agent- 前缀、上锁
    const cli::AgentWorktree room = cli::CreateAgentWorktree(repo.root);
    REQUIRE(room.ok);
    CHECK(room.name.starts_with("agent-"));
    CHECK(room.branch == "worktree/" + room.name);
    CHECK(std::filesystem::exists(room.room_path / "seed.txt"));  // checkout 出来了
    bool locked = false;
    for (const auto& entry : cli::ListWorktrees(repo.root)) {
        // git 输出的路径斜杠/大小写跟本地拼的未必逐字相同,按末级房名对。
        if (PathToUtf8(entry.path.filename()) == room.name) {
            locked = entry.locked;
        }
    }
    CHECK(locked);

    // 干净收工:房与分支都删
    const cli::AgentWorktreeFinish clean_finish =
        cli::FinishAgentWorktree(repo.root, room.room_path, room.branch, room.base_commit);
    CHECK(clean_finish.removed);
    CHECK(clean_finish.note.empty());
    CHECK_FALSE(std::filesystem::exists(room.room_path));
    CHECK(RunGitAt(repo.root, {"branch", "--list", room.branch}).output.empty());

    // 有活收工:解锁留房,note 带路径
    const cli::AgentWorktree dirty = cli::CreateAgentWorktree(repo.root);
    REQUIRE(dirty.ok);
    std::ofstream(dirty.room_path / "change.txt") << "work\n";
    const cli::AgentWorktreeFinish dirty_finish =
        cli::FinishAgentWorktree(repo.root, dirty.room_path, dirty.branch, dirty.base_commit);
    CHECK_FALSE(dirty_finish.removed);
    CHECK(dirty_finish.note.find(PathToUtf8(dirty.room_path)) != std::string::npos);
    CHECK(dirty_finish.note.find(dirty.branch) != std::string::npos);
    CHECK(std::filesystem::exists(dirty.room_path));
    // 收尾:删分支留着没关系,把房删掉免得挡后面的测试
    cli::FinishAgentWorktree(repo.root, dirty.room_path, dirty.branch, dirty.base_commit);  // 二次:房还在但仍有活
}

TEST_CASE("CleanStaleAgentWorktrees: 只清 agent- 陈房,有活/新近/用户的房不碰") {
    RealGitRepo repo;

    // 陈而干净:删
    const cli::AgentWorktree stale_clean = cli::CreateAgentWorktree(repo.root);
    REQUIRE(stale_clean.ok);
    repo.MakeOld(stale_clean.room_path);
    // 干净房直接 Finish 掉分支? 不——清扫要自己处理分支,先手动解锁让清扫动手
    REQUIRE(cli::UnlockWorktree(stale_clean.room_path));

    // 陈而有活:留
    const cli::AgentWorktree stale_dirty = cli::CreateAgentWorktree(repo.root);
    REQUIRE(stale_dirty.ok);
    std::ofstream(stale_dirty.room_path / "wip.txt") << "wip\n";
    repo.MakeOld(stale_dirty.room_path);
    REQUIRE(cli::UnlockWorktree(stale_dirty.room_path));

    // 陈但锁着(被杀会话留下的锁):放锁后删
    const cli::AgentWorktree stale_locked = cli::CreateAgentWorktree(repo.root);
    REQUIRE(stale_locked.ok);
    repo.MakeOld(stale_locked.room_path);

    // 新近的 agent 房:不碰
    const cli::AgentWorktree fresh = cli::CreateAgentWorktree(repo.root);
    REQUIRE(fresh.ok);
    REQUIRE(cli::UnlockWorktree(fresh.room_path));

    // 用户手起的房(worktree/ 前缀但目录名不带 agent-):再陈也不碰
    const std::filesystem::path user_room = repo.root / ".lubancode" / "worktrees" / "user-keep";
    REQUIRE(RunGitAt(repo.root, {"worktree", "add", "-q", "-b", "worktree/user-keep",
                                 PathToUtf8(user_room)})
                .exit_code == 0);
    repo.MakeOld(user_room);

    const cli::StaleAgentWorktreeCleanup report = cli::CleanStaleAgentWorktrees(repo.root, std::chrono::hours(72));
    CHECK(report.removed == 2);  // stale_clean + stale_locked
    CHECK(report.kept_dirty == 1);
    CHECK(report.kept_fresh >= 1);
    CHECK_FALSE(std::filesystem::exists(stale_clean.room_path));
    CHECK_FALSE(std::filesystem::exists(stale_locked.room_path));
    CHECK(std::filesystem::exists(stale_dirty.room_path));  // 有活
    CHECK(std::filesystem::exists(fresh.room_path));        // 新近
    CHECK(std::filesystem::exists(user_room));              // 用户手起
}

// ---------------------------------------------------------------------------
// 基线冻结(派工单 §三):CreateAgentWorktree 的基准=冻结的调用者 HEAD,
// 不解析 origin 默认分支。四场景:本地领先远端、detached HEAD、普通分支、
// 带未推提交——基线都不漂;actual_head 对不上冻结值时拆房报错。
// ---------------------------------------------------------------------------

namespace {

std::string TrimGitOutput(const std::string& text) {
    std::size_t end = text.size();
    while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r' || text[end - 1] == ' ')) {
        --end;
    }
    return text.substr(0, end);
}

}  // namespace

TEST_CASE("CreateAgentWorktree: 基线=冻结的调用者 HEAD——领先远端/detached/普通分支/未推提交都不漂") {
    RealGitRepo repo;
    // 摆一个"落后的远端":origin/main 钉在第一笔提交,本地再进一笔(未推)。
    const std::string first = TrimGitOutput(RunGitAt(repo.root, {"rev-parse", "HEAD"}).output);
    REQUIRE(RunGitAt(repo.root, {"update-ref", "refs/remotes/origin/main", first}).exit_code == 0);
    REQUIRE(RunGitAt(repo.root, {"symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/main"})
                .exit_code == 0);
    std::ofstream(repo.root / "second.txt") << "second\n";
    REQUIRE(RunGitAt(repo.root, {"add", "."}).exit_code == 0);
    REQUIRE(RunGitAt(repo.root, {"commit", "-q", "-m", "second"}).exit_code == 0);
    const std::string head = TrimGitOutput(RunGitAt(repo.root, {"rev-parse", "HEAD"}).output);

    // 场景 1+4:普通分支、本地领先远端、带未推提交——房 HEAD=本地 HEAD,
    // 不是 origin/main;新代码(second.txt)在房里读得到。
    const cli::AgentWorktree ahead = cli::CreateAgentWorktree(repo.root);
    REQUIRE(ahead.ok);
    CHECK(ahead.base_commit == head);
    CHECK(ahead.actual_head == head);
    CHECK(ahead.base_ref == "main");
    CHECK(ahead.actual_head != first);  // 若还按 origin/main 起树,这里就漂了
    CHECK(std::filesystem::exists(ahead.room_path / "second.txt"));
    REQUIRE(cli::FinishAgentWorktree(repo.root, ahead.room_path, ahead.branch, ahead.base_commit).removed);

    // 场景 2:detached HEAD——基线照样冻结得住,base_ref 记 "(detached)"。
    REQUIRE(RunGitAt(repo.root, {"checkout", "-q", "--detach", head}).exit_code == 0);
    const cli::AgentWorktree detached = cli::CreateAgentWorktree(repo.root);
    REQUIRE(detached.ok);
    CHECK(detached.base_commit == head);
    CHECK(detached.actual_head == head);
    CHECK(detached.base_ref == "(detached)");
    REQUIRE(cli::FinishAgentWorktree(repo.root, detached.room_path, detached.branch, detached.base_commit)
                .removed);
    REQUIRE(RunGitAt(repo.root, {"checkout", "-q", "main"}).exit_code == 0);
}

TEST_CASE("CreateAgentWorktree: actual_head 对不上冻结基线时拆房报错,不静默开工") {
    RealGitRepo repo;
    const std::string head = TrimGitOutput(RunGitAt(repo.root, {"rev-parse", "HEAD"}).output);

    // 假 runner:worktree add 报"成功",但房内 rev-parse HEAD 吐另一个提交
    //——CreateAgentWorktree 必须当场拆房,把 base_ref/base_commit/actual_head
    // 三个值全亮出来。其余 git 调用(lock/unlock/prune/branch)一律报成功,
    // 拆房路径才走得完。
    const cli::GitRunner lying = [](const cli::GitCommand& command) -> cli::GitCommandResult {
        if (command.args.size() >= 2 && command.args[0] == "worktree" && command.args[1] == "add") {
            return {0, "", ""};
        }
        if (!command.args.empty() && command.args[0] == "rev-parse") {
            return {0, "0000000000000000000000000000000000000000\n", ""};
        }
        return {0, "", ""};
    };
    const cli::AgentWorktree mismatched = cli::CreateAgentWorktree(repo.root, head, "main", lying);
    CHECK_FALSE(mismatched.ok);
    CHECK(mismatched.error.find("基线对不上") != std::string::npos);
    CHECK(mismatched.error.find(head) != std::string::npos);
    CHECK(mismatched.error.find("0000000000000000000000000000000000000000") != std::string::npos);
    // git 侧没落下任何房(worktree add 是假的,登记不存在)。
    CHECK(cli::ListWorktrees(repo.root).size() == 1);  // 只有主 checkout 自己
}

TEST_CASE("FinishAgentWorktree: 有自有提交的房待主控复核,绝不自动删(派工单 §五)") {
    RealGitRepo repo;
    const cli::AgentWorktree room = cli::CreateAgentWorktree(repo.root);
    REQUIRE(room.ok);
    // 房内提交一笔(干净但有自有提交——子代理回传现场的真实形状)。
    REQUIRE(RunGitAt(room.room_path, {"config", "user.email", "test@example.com"}).exit_code == 0);
    REQUIRE(RunGitAt(room.room_path, {"config", "user.name", "Test"}).exit_code == 0);
    std::ofstream(room.room_path / "work.txt") << "done\n";
    REQUIRE(RunGitAt(room.room_path, {"add", "."}).exit_code == 0);
    REQUIRE(RunGitAt(room.room_path, {"commit", "-q", "-m", "work"}).exit_code == 0);
    const std::string room_head = TrimGitOutput(RunGitAt(room.room_path, {"rev-parse", "HEAD"}).output);

    const cli::AgentWorktreeFinish finish =
        cli::FinishAgentWorktree(repo.root, room.room_path, room.branch, room.base_commit);
    CHECK_FALSE(finish.removed);
    CHECK(finish.awaiting_review);
    CHECK(finish.head_commit == room_head);          // 持久提交引用
    CHECK(std::filesystem::exists(room.room_path));  // 复核现场还在
    CHECK_FALSE(RunGitAt(repo.root, {"branch", "--list", room.branch}).output.empty());  // 分支没被 -D
    CHECK(finish.note.find(room.branch) != std::string::npos);
    CHECK(finish.note.find(room_head) != std::string::npos);
    CHECK(finish.note.find("awaiting_parent_review") != std::string::npos);
    CHECK(finish.note.find("git worktree add") != std::string::npos);  // 一条命令重挂

    // 调用者自己的未推提交不算房的活:从"领先远端的调用者"起树、房内无
    // 新提交时,收工照旧自动清理(不因调用者领先而误保留)。
    const std::string first = room.base_commit;
    REQUIRE(RunGitAt(repo.root, {"update-ref", "refs/remotes/origin/main", first}).exit_code == 0);
    REQUIRE(RunGitAt(repo.root, {"symbolic-ref", "refs/remotes/origin/HEAD", "refs/remotes/origin/main"})
                .exit_code == 0);
    std::ofstream(repo.root / "unpushed.txt") << "u\n";
    REQUIRE(RunGitAt(repo.root, {"add", "."}).exit_code == 0);
    REQUIRE(RunGitAt(repo.root, {"commit", "-q", "-m", "unpushed"}).exit_code == 0);
    const cli::AgentWorktree empty_room = cli::CreateAgentWorktree(repo.root);
    REQUIRE(empty_room.ok);
    const cli::AgentWorktreeFinish empty_finish =
        cli::FinishAgentWorktree(repo.root, empty_room.room_path, empty_room.branch, empty_room.base_commit);
    CHECK(empty_finish.removed);  // 干净且无自有提交:照旧删
    CHECK_FALSE(empty_finish.awaiting_review);
}
