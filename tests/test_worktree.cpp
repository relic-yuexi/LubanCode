#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "cli/worktree.hpp"

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
