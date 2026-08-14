#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cli/worktree.hpp"
#include "platform/process.hpp"

using namespace lubancode;

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

std::filesystem::path Utf8Path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

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
    CHECK(std::filesystem::current_path() == cli::WorktreePath(repo, "fix-1"));
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
    CHECK(std::filesystem::current_path() == repo);
    CHECK(std::filesystem::exists(cli::WorktreePath(repo, "fix-1")));
    REQUIRE(hook_log.size() == 2);
    CHECK(hook_log[1] == "exit");

    // 再进,exit remove(干净即删)
    REQUIRE(session.Enter("fix-1", "head").code == cli::WorktreeResultCode::Created);
    const auto removed = session.Exit("remove");
    CHECK(removed.code == cli::WorktreeResultCode::Removed);
    CHECK_FALSE(std::filesystem::exists(cli::WorktreePath(repo, "fix-1")));
    CHECK(std::filesystem::current_path() == repo);
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
    CHECK(std::filesystem::current_path() == outside);

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
    CHECK(std::filesystem::current_path() == repo);  // 没被搬进去

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

#ifdef _WIN32
TEST_CASE("SafeRemoveTree: junction 只删链接,不追删指向的目录") {
    const auto base = std::filesystem::temp_directory_path() /
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
