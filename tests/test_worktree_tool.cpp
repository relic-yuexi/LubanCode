// tools::WorktreeTool(模型侧 worktree 工具薄壳,0.27.x):入参校验、
// 结果文本排版、两道硬确认(进园外的房、脏房强删)的回调通道——git 编排
// 都在 cli::WorktreeSession(拿假 runner,test_worktree.cpp 已钉),这里
// 只钉薄壳自己的行为。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <optional>
#include <string>

#include "cli/worktree.hpp"
#include "tools/worktree_tool.hpp"

using namespace lubancode;

namespace {

std::filesystem::path Utf8Path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

struct CwdGuard {
    std::filesystem::path saved;
    CwdGuard() : saved(std::filesystem::current_path()) {}
    ~CwdGuard() {
        std::error_code ec;
        std::filesystem::current_path(saved, ec);
    }
};

struct TempRepo {
    std::filesystem::path repo;
    std::filesystem::path outside;
    bool dirty = false;

    TempRepo() {
        const auto base = std::filesystem::temp_directory_path() /
                          ("lubancode_wttool_" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        repo = base / "repo";
        std::filesystem::create_directories(repo / ".git");
        outside = base / "user-room";
        std::filesystem::create_directories(outside);
        const std::filesystem::path admin = repo / ".git" / "worktrees" / "user-room";
        std::filesystem::create_directories(admin);
        std::ofstream(outside / ".git") << "gitdir: " << PathToUtf8(admin) << "\n";
        std::ofstream(admin / "gitdir") << PathToUtf8(outside) << "\n";
    }
    ~TempRepo() {
        std::error_code ec;
        std::filesystem::remove_all(repo.parent_path(), ec);
    }

    cli::GitRunner Runner() {
        return [this](const cli::GitCommand& command) -> cli::GitCommandResult {
            const std::string first = command.args.front();
            if (first == "rev-parse" && command.args.size() > 1 && command.args[1] == "--show-toplevel") {
                return {0, PathToUtf8(repo) + "\n", {}};
            }
            if (first == "worktree" && command.args.size() > 1 && command.args[1] == "list") {
                return {0, "worktree " + PathToUtf8(repo) + "\nbranch refs/heads/main\nworktree " +
                              PathToUtf8(outside) + "\nbranch refs/heads/user-room\n",
                        {}};
            }
            if (first == "symbolic-ref") {
                return {0, "user-room\n", {}};
            }
            if (first == "status") {
                return dirty ? cli::GitCommandResult{0, " M x\n", {}} : cli::GitCommandResult{0, "", {}};
            }
            return {0, "", {}};
        };
    }
};

}  // namespace


// macOS 的 /var 是 /private/var 的符号链接:临时目录路径带着 /var 进去,
// chdir 后 current_path() 出来已是 /private/var,裸等值必翻。断言两边都
// 过一遍 canonical 再比(路径不存在时原样退回,给"拒进"类负例留活路)。
inline std::filesystem::path NormalizedPath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::canonical(path, ec);
    return ec ? path : canonical;
}

TEST_CASE("worktree 工具:名字与 schema 形状") {
    TempRepo temp;
    cli::WorktreeSession session(temp.Runner());
    tools::WorktreeTool tool(session, /*confirm=*/{}, /*on_session_moved=*/{});
    CHECK(tool.name() == "worktree");
    CHECK_FALSE(tool.needs_confirm());

    const nlohmann::json schema = tool.input_schema();
    CHECK(schema["required"] == nlohmann::json::array({"action"}));
    CHECK(schema["properties"]["action"]["enum"] ==
          nlohmann::json::array({"enter", "status", "list", "exit"}));
    CHECK(schema["properties"]["base"]["enum"] == nlohmann::json::array({"fresh", "head"}));
    CHECK(schema["properties"]["mode"]["enum"] == nlohmann::json::array({"keep", "remove"}));
}

TEST_CASE("worktree 工具:入参校验") {
    TempRepo temp;
    cli::WorktreeSession session(temp.Runner());
    tools::WorktreeTool tool(session, /*confirm=*/{}, /*on_session_moved=*/{});

    CHECK(tool.execute(nlohmann::json{{"command", "x"}}).is_error);  // 缺 action
    CHECK(tool.execute(nlohmann::json{{"action", "fly"}}).is_error);
    CHECK(tool.execute(nlohmann::json{{"action", "enter"}, {"base", "origin/main"}}).is_error);
    CHECK(tool.execute(nlohmann::json{{"action", "exit"}, {"mode", "destroy"}}).is_error);
    CHECK(tool.execute(nlohmann::json{{"action", "enter"}, {"name", "bad/name"}}).is_error);
}

TEST_CASE("worktree 工具:status/list 文本可读") {
    TempRepo temp;
    cli::WorktreeSession session(temp.Runner());
    tools::WorktreeTool tool(session, /*confirm=*/{}, /*on_session_moved=*/{});

    const auto idle = tool.execute(nlohmann::json{{"action", "status"}});
    CHECK_FALSE(idle.is_error);
    CHECK(idle.content.find("不在任何 worktree") != std::string::npos);

    CwdGuard guard;
    std::filesystem::current_path(temp.repo);
    const auto list = tool.execute(nlohmann::json{{"action", "list"}});
    CHECK_FALSE(list.is_error);
    CHECK(list.content.find("user-room") != std::string::npos);
    CHECK(list.content.find("分支 user-room") != std::string::npos);
}

TEST_CASE("worktree 工具:进园外的房,confirm 通道说了算") {
    CwdGuard guard;
    TempRepo temp;
    cli::WorktreeSession session(temp.Runner());
    int moved = 0;
    tools::WorktreeTool tool(
        session, /*confirm=*/[](const std::string& question) -> std::optional<bool> {
            REQUIRE(question.find("user-room") != std::string::npos);
            return false;  // 用户摇头
        },
        [&moved]() { ++moved; });

    std::filesystem::current_path(temp.repo);
    const auto refused = tool.execute(nlohmann::json{{"action", "enter"}, {"name", "user-room"}});
    CHECK(refused.is_error);
    CHECK(refused.content.find("需要用户确认") != std::string::npos);
    CHECK_FALSE(session.active());
    CHECK(moved == 0);
    CHECK(NormalizedPath(std::filesystem::current_path()) == NormalizedPath(temp.repo));  // 没搬

    // 用户点头:进得去,房名上状态,搬目录回调触发
    tools::WorktreeTool agree_tool(
        session, /*confirm=*/[](const std::string&) -> std::optional<bool> { return true; },
        [&moved]() { ++moved; });
    const auto entered = agree_tool.execute(nlohmann::json{{"action", "enter"}, {"name", "user-room"}});
    CHECK_FALSE(entered.is_error);
    CHECK(entered.content.find("已住进 worktree 房") != std::string::npos);
    CHECK(session.active_name() == "user-room");
    CHECK(moved == 1);
    CHECK(NormalizedPath(std::filesystem::current_path()) == NormalizedPath(temp.outside));

    // 脏房 exit remove:confirm 点头才删;摇头则房原样保留
    temp.dirty = true;
    tools::WorktreeTool deny_tool(
        session, /*confirm=*/[](const std::string&) -> std::optional<bool> { return false; },
        [&moved]() { ++moved; });
    const auto kept_dirty = deny_tool.execute(nlohmann::json{{"action", "exit"}, {"mode", "remove"}});
    CHECK(kept_dirty.is_error);
    CHECK(kept_dirty.content.find("已被拒绝") != std::string::npos);
    CHECK(std::filesystem::exists(temp.outside));
    CHECK(NormalizedPath(std::filesystem::current_path()) == NormalizedPath(temp.outside));  // 拒绝后不出房

    tools::WorktreeTool force_tool(
        session, /*confirm=*/[](const std::string&) -> std::optional<bool> { return true; }, [&moved]() { ++moved; });
    const auto removed = force_tool.execute(nlohmann::json{{"action", "exit"}, {"mode", "remove"}});
    CHECK_FALSE(removed.is_error);
    CHECK_FALSE(std::filesystem::exists(temp.outside));
    CHECK(moved == 2);
}

TEST_CASE("worktree 工具:没人可问(管道模式)时硬确认一律拒") {
    CwdGuard guard;
    TempRepo temp;
    cli::WorktreeSession session(temp.Runner());
    tools::WorktreeTool tool(session, /*confirm=*/{}, /*on_session_moved=*/{});
    std::filesystem::current_path(temp.repo);

    const auto refused = tool.execute(nlohmann::json{{"action", "enter"}, {"name", "user-room"}});
    CHECK(refused.is_error);
    CHECK(refused.content.find("需要用户确认") != std::string::npos);
    CHECK_FALSE(session.active());
}
