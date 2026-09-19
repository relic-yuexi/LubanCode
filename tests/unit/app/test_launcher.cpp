// 固定启动器布局识别(GitHubRelease自动更新单 P1,§六)的纯函数用例:
// 只打 ClassifyExecutionLayout,不真起进程(转发链路的真机验证另记)。
#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/launcher.hpp"

namespace fs = std::filesystem;
namespace launcher = lubancode::app::launcher;

namespace {

#ifdef _WIN32
constexpr const char* kExeName = "lubancode.exe";
#else
constexpr const char* kExeName = "lubancode";
#endif

fs::path MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    const fs::path base = fs::temp_directory_path() /
                          ("luban-launcher-test-" + tag + "-" +
                           std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

void WriteFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

}  // namespace

TEST_CASE("ClassifyExecutionLayout: 裸 exe 是 Standalone") {
    const fs::path base = MakeTempDir("standalone");
    WriteFile(base / kExeName, "binary");
    const auto info = launcher::ClassifyExecutionLayout(base / kExeName);
    CHECK(info.layout == launcher::Layout::Standalone);
    CHECK(info.install_root.empty());
}

TEST_CASE("ClassifyExecutionLayout: 平铺安装认 skills/manifest/updater") {
    const fs::path base = MakeTempDir("flat");
    WriteFile(base / kExeName, "binary");
    fs::create_directories(base / "skills");
    auto info = launcher::ClassifyExecutionLayout(base / kExeName);
    CHECK(info.layout == launcher::Layout::FlatInstall);
    CHECK(info.install_root == base);

    fs::remove_all(base / "skills");
    WriteFile(base / "manifest.json", "{}");
    info = launcher::ClassifyExecutionLayout(base / kExeName);
    CHECK(info.layout == launcher::Layout::FlatInstall);
}

TEST_CASE("ClassifyExecutionLayout: versions/<v>/ 里是 VersionedEntry") {
    const fs::path base = MakeTempDir("versioned");
    const fs::path exe = base / "versions" / "1.0.0-deadbeef" / kExeName;
    WriteFile(exe, "binary");
    const auto info = launcher::ClassifyExecutionLayout(exe);
    CHECK(info.layout == launcher::Layout::VersionedEntry);
    CHECK(info.install_root == base);
}

TEST_CASE("ClassifyExecutionLayout: 根位启动器按 current.json 转发") {
    const fs::path base = MakeTempDir("launcher");
    WriteFile(base / kExeName, "launcher copy");
    fs::create_directories(base / "versions" / "2.0.0-cafe1234");
    WriteFile(base / "current.json",
              R"({"schema":1,"current":"2.0.0-cafe1234","previous":null})");
    const auto info = launcher::ClassifyExecutionLayout(base / kExeName);
    REQUIRE(info.layout == launcher::Layout::RootLauncher);
    CHECK(info.install_root == base);
    CHECK(info.current == "2.0.0-cafe1234");
    CHECK(info.current_exe == base / "versions" / "2.0.0-cafe1234" / kExeName);
}

TEST_CASE("ClassifyExecutionLayout: 坏指针不当启动器") {
    const fs::path base = MakeTempDir("badptr");
    WriteFile(base / kExeName, "launcher copy");
    fs::create_directories(base / "versions" / "2.0.0-cafe1234");
    fs::create_directories(base / "skills");

    // schema 不认:退回平铺/独立判定,不猜
    WriteFile(base / "current.json", R"({"schema":9,"current":"2.0.0-cafe1234"})");
    CHECK(launcher::ClassifyExecutionLayout(base / kExeName).layout ==
          launcher::Layout::FlatInstall);

    // 目录名带路径形状(穿越/绝对):不信
    WriteFile(base / "current.json",
              R"({"schema":1,"current":"../../evil"})");
    CHECK(launcher::ClassifyExecutionLayout(base / kExeName).layout ==
          launcher::Layout::FlatInstall);

    // 空指针
    WriteFile(base / "current.json", R"({"schema":1,"current":""})");
    CHECK(launcher::ClassifyExecutionLayout(base / kExeName).layout ==
          launcher::Layout::FlatInstall);
}
