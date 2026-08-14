// tools/isolation + 三道闸的执行点(0.27.x):范围栈的压/弹与嵌套、
// PathBlockedByIsolation 的禁写根判定、write_file/edit_file 的文件闸、
// run_command 的 cwd 闸与 git 改道闸(被拦的调用在起进程之前就回错误,
// 不碰真 shell)。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "tools/edit_file.hpp"
#include "tools/isolation.hpp"
#include "tools/run_command.hpp"
#include "tools/write_file.hpp"

using namespace lubancode;

namespace {

std::filesystem::path Utf8Path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 临时仓库:主树 + 自家房区,测试里现摆。
struct TempRepo {
    std::filesystem::path root;
    std::filesystem::path room;

    TempRepo() {
        auto base = std::filesystem::temp_directory_path() /
                    ("lubancode_iso_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        root = base / "repo";
        room = root / ".lubancode" / "worktrees" / "fix-1";
        std::filesystem::create_directories(room);
        std::filesystem::create_directories(root / "src");
        std::ofstream(root / "src" / "main.cpp") << "int main(){}\n";
    }
    ~TempRepo() {
        std::error_code ec;
        std::filesystem::remove_all(root.parent_path(), ec);
    }

    tools::IsolationScope Scope() const {
        return tools::IsolationScope{"fix-1", PathToUtf8(room), PathToUtf8(root)};
    }
};

}  // namespace

TEST_CASE("IsolationGuard:压栈/弹栈/嵌套,内层盖外层") {
    CHECK(tools::IsolationGuard::Current() == nullptr);
    const tools::IsolationScope outer{"outer", "D:/a/room", "D:/a"};
    {
        tools::ScopedIsolation guard(outer);
        REQUIRE(tools::IsolationGuard::Current() != nullptr);
        CHECK(tools::IsolationGuard::Current()->name == "outer");
        {
            const tools::IsolationScope inner{"inner", "D:/b/room", "D:/b"};
            tools::ScopedIsolation inner_guard(inner);
            CHECK(tools::IsolationGuard::Current()->name == "inner");
        }
        CHECK(tools::IsolationGuard::Current()->name == "outer");
    }
    CHECK(tools::IsolationGuard::Current() == nullptr);
}

TEST_CASE("PathBlockedByIsolation:主树内拦,房区与主树外放行") {
    const TempRepo repo;
    const tools::IsolationScope scope = repo.Scope();

    CHECK(tools::PathBlockedByIsolation(PathToUtf8(repo.root / "src" / "main.cpp"), scope));
    CHECK(tools::PathBlockedByIsolation(PathToUtf8(repo.root / "README.md"), scope));
    // 相对路径以房为基准,落在房内 → 放行
    CHECK_FALSE(tools::PathBlockedByIsolation("src/room_file.cpp", scope));
    // 房区之内:放行
    CHECK_FALSE(tools::PathBlockedByIsolation(PathToUtf8(repo.room / "x.cpp"), scope));
    CHECK_FALSE(tools::PathBlockedByIsolation(PathToUtf8(repo.room), scope));
    // 主树之外:放行
    CHECK_FALSE(tools::PathBlockedByIsolation("D:/elsewhere/x.cpp", scope));
    CHECK_FALSE(tools::PathBlockedByIsolation("D:/a-but-not-a", scope));
}

TEST_CASE("文件闸:write_file/edit_file 写主 checkout 被拦,写房内放行") {
    const TempRepo repo;
    tools::ScopedIsolation guard(repo.Scope());

    tools::WriteFileTool write;
    const auto blocked = write.execute(nlohmann::json{
        {"path", PathToUtf8(repo.root / "src" / "blocked.cpp")}, {"content", "x"}});
    CHECK(blocked.is_error);
    CHECK(blocked.content.find("[隔离]") != std::string::npos);
    CHECK(blocked.content.find("fix-1") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(repo.root / "src" / "blocked.cpp"));

    const auto allowed = write.execute(
        nlohmann::json{{"path", PathToUtf8(repo.room / "new.cpp")}, {"content", "int x;\n"}});
    CHECK_FALSE(allowed.is_error);
    CHECK(std::filesystem::exists(repo.room / "new.cpp"));

    tools::EditFileTool edit;
    const auto edit_blocked = edit.execute(nlohmann::json{
        {"path", PathToUtf8(repo.root / "src" / "main.cpp")}, {"old_string", "int"}, {"new_string", "long"}});
    CHECK(edit_blocked.is_error);
    CHECK(edit_blocked.content.find("[隔离]") != std::string::npos);
}

TEST_CASE("文件闸:不隔离时照旧放行") {
    const TempRepo repo;
    CHECK(tools::IsolationGuard::Current() == nullptr);
    tools::WriteFileTool write;
    const auto result = write.execute(
        nlohmann::json{{"path", PathToUtf8(repo.root / "src" / "free.cpp")}, {"content", "x"}});
    CHECK_FALSE(result.is_error);
}

TEST_CASE("cwd 闸与 git 改道闸:run_command 在起进程前就拦") {
    const TempRepo repo;
    tools::ScopedIsolation guard(repo.Scope());
    tools::RunCommandTool run;

    // cwd 指主树
    const auto cwd_blocked = run.execute(
        nlohmann::json{{"command", "echo hi"}, {"cwd", PathToUtf8(repo.root)}});
    CHECK(cwd_blocked.is_error);
    CHECK(cwd_blocked.content.find("[隔离]") != std::string::npos);

    // git -C 指主树
    const auto git_blocked = run.execute(
        nlohmann::json{{"command", "git -C \"" + PathToUtf8(repo.root) + "\" status"}});
    CHECK(git_blocked.is_error);
    CHECK(git_blocked.content.find("[隔离]") != std::string::npos);

    // GIT_DIR 赋值
    const auto env_blocked = run.execute(
        nlohmann::json{{"command", "$env:GIT_DIR='D:/nowhere/.git'; git status"}});
    CHECK(env_blocked.is_error);
}
