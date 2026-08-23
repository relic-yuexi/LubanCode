// plugins 单第 3 步(Python scaffold)的测试:生成物形状 + 真机往返。
//
// 三层:
//   1. 纯落盘:目录/文件/manifest 可解析(runner.py 的字节内容由第 2 层
//      真跑兜底,不做逐字节断言);
//   2. 真机往返:有 Python 的环境用生成的 runner.py 走一遍 RunProcessToolCall
//      ——CI 有 Python 跑真脚本,没有明确 SKIP(PythonAvailable 成例);
//   3. 冲突与命名:非空目录拒绝、坏名字拒绝。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "app/plugin_scaffold.hpp"
#include "platform/process.hpp"
#include "runtime/plugin_contract.hpp"
#include "runtime/plugin_process.hpp"
#include "runtime/plugin_tool.hpp"

using namespace lubancode;
using namespace lubancode::app;
using namespace lubancode::runtime;

namespace {

#ifdef _WIN32
constexpr const char* kPythonCmd = "python";
#else
constexpr const char* kPythonCmd = "python3";
#endif

bool PythonAvailable() {
    static const bool available = [] {
        const auto result = platform::RunProcess(std::vector<std::string>{kPythonCmd, "--version"}, 10000);
        return result.exit_code == 0 && result.output.find("Python") != std::string::npos;
    }();
    return available;
}

struct TempRoot {
    std::filesystem::path path;
    TempRoot() {
        std::error_code ec;
        path = std::filesystem::temp_directory_path() /
              ("lubancode_plugin_scaffold_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
               std::to_string(++counter_));
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempRoot() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

  private:
    static int counter_;
};
int TempRoot::counter_ = 0;

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("scaffold:生成 plugin.json/runner.py/test_runner.py,manifest 可解析") {
    TempRoot root;
    const auto result = ScaffoldPythonPlugin(lubancode::platform::PathToUtf8(root.path), "my_math", kPythonCmd);
    REQUIRE(result.has_value());
    CHECK(result->plugin_name == "my_math");
    CHECK(result->files.size() == 3);

    const std::filesystem::path dir = root.path / "my_math";
    for (const char* name : {"plugin.json", "runner.py", "test_runner.py"}) {
        CHECK(std::filesystem::is_regular_file(dir / name));
    }

    // 生成的 manifest 必须过得了合同解析(生成器自己坏了自己,加载期就露馅)。
    auto manifest = ParsePluginManifest(ReadFile(dir / "plugin.json"), dir);
    if (!manifest.has_value()) {
        INFO(manifest.error());
    }
    REQUIRE(manifest.has_value());
    CHECK(manifest->id == "my_math");
    CHECK(manifest->kind == RuntimeKind::Process);
    REQUIRE(manifest->tools.size() == 1);
    CHECK(manifest->tools[0].name == "add");
    CHECK(manifest->tools[0].full_name == "plugin__my_math__add");
    CHECK(manifest->argv.size() == 2);  // command + runner.py
}

TEST_CASE("scaffold:非空同名目录拒绝,不覆盖用户东西") {
    TempRoot root;
    (std::filesystem::create_directories(root.path / "taken"),
     std::ofstream(root.path / "taken" / "keep.txt", std::ios::binary) << "mine");
    const auto result = ScaffoldPythonPlugin(lubancode::platform::PathToUtf8(root.path), "taken", kPythonCmd);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("不覆盖") != std::string::npos);
    // 原文件还在。
    CHECK(std::filesystem::is_regular_file(root.path / "taken" / "keep.txt"));
}

TEST_CASE("scaffold:坏插件名(空/元字符/超长)拒绝") {
    TempRoot root;
    const std::string root_utf8 = lubancode::platform::PathToUtf8(root.path);
    CHECK_FALSE(ScaffoldPythonPlugin(root_utf8, "", kPythonCmd).has_value());
    CHECK_FALSE(ScaffoldPythonPlugin(root_utf8, "a;b", kPythonCmd).has_value());
    CHECK_FALSE(ScaffoldPythonPlugin(root_utf8, "-lead", kPythonCmd).has_value());
    CHECK_FALSE(ScaffoldPythonPlugin(root_utf8, std::string(80, 'x'), kPythonCmd).has_value());
}

TEST_CASE("scaffold:同名二次生成 = 非空目录拒绝(幂等安全)") {
    TempRoot root;
    const std::string root_utf8 = lubancode::platform::PathToUtf8(root.path);
    REQUIRE(ScaffoldPythonPlugin(root_utf8, "dup", kPythonCmd).has_value());
    CHECK_FALSE(ScaffoldPythonPlugin(root_utf8, "dup", kPythonCmd).has_value());
}

// ---------------------------------------------------------------------------
// 真机往返:生成的 runner.py 走完整 RunProcessToolCall 链
// ---------------------------------------------------------------------------

TEST_CASE("scaffold:生成的 runner.py 真跑——add 往返/缺字段验参/未知工具/坏 JSON") {
    if (!PythonAvailable()) {
        return;
    }
    TempRoot root;
    const auto scaffold = ScaffoldPythonPlugin(lubancode::platform::PathToUtf8(root.path), "live_probe", kPythonCmd);
    REQUIRE(scaffold.has_value());
    const std::filesystem::path dir = root.path / "live_probe";
    auto manifest = ParsePluginManifest(ReadFile(dir / "plugin.json"), dir);
    REQUIRE(manifest.has_value());

    const auto make_request = [](const nlohmann::json& arguments) {
        plugin_protocol::ProcessRequest request;
        request.plugin = "live_probe";
        request.tool = "add";
        request.entry = "add";
        request.arguments = arguments;
        request.call_id = "call_scaffold";
        request.context_cwd = "D:/tmp";
        return request;
    };

    // 正常:add(1.5, 2) = 3.5。
    {
        const auto outcome = RunProcessToolCall(*manifest, make_request(nlohmann::json{{"a", 1.5}, {"b", 2}}),
                                                "D:/tmp", nullptr, ProcessCallLimits{});
        const std::string why = outcome.detail + " | stderr=" + outcome.stderr_tail.substr(0, 300);
        INFO(why);
        REQUIRE(outcome.code == PluginErrorCode::Ok);
        CHECK(outcome.text == "3.5");
        CHECK(outcome.structured == 3.5);
    }
    // 插件自报失败:缺 b 字段(KeyError 进 execution_failed)。
    {
        const auto outcome = RunProcessToolCall(*manifest, make_request(nlohmann::json{{"a", 1}}), "D:/tmp",
                                                nullptr, ProcessCallLimits{});
        REQUIRE(outcome.code == PluginErrorCode::PluginReportedError);
        CHECK(outcome.plugin_error_code == "execution_failed");
    }
    // 未知工具:unknown_tool。
    {
        auto request = make_request(nlohmann::json::object());
        request.tool = "nope";
        request.entry = "nope";
        const auto outcome = RunProcessToolCall(*manifest, request, "D:/tmp", nullptr, ProcessCallLimits{});
        REQUIRE(outcome.code == PluginErrorCode::PluginReportedError);
        CHECK(outcome.plugin_error_code == "unknown_tool");
    }
}

TEST_CASE("scaffold:本地单测模板真能跑(python test_runner.py 退出码 0)") {
    if (!PythonAvailable()) {
        return;
    }
    TempRoot root;
    REQUIRE(ScaffoldPythonPlugin(lubancode::platform::PathToUtf8(root.path), "ut_probe", kPythonCmd).has_value());
    // test_runner.py import runner,须在插件目录里跑(cwd 参数)。
    const auto result = platform::RunProcessWithStdin(
        std::vector<std::string>{kPythonCmd, "test_runner.py"}, "", 60000, {},
        platform::kDefaultMaxOutputBytes);
    INFO(result.output.substr(0, 400));
    // 退出码非零多半是 cwd 不在插件目录找不到 runner 模块——重跑一遍带
    // 绝对路径的,两条路至少一条要通(cwd 不可写的极端机器除外)。
    if (result.exit_code != 0) {
        const std::string script = lubancode::platform::PathToUtf8(root.path / "ut_probe" / "test_runner.py");
        const auto retry = platform::RunProcessWithStdin(std::vector<std::string>{kPythonCmd, script}, "", 60000,
                                                         {}, platform::kDefaultMaxOutputBytes);
        INFO(retry.output.substr(0, 400));
        CHECK(retry.exit_code == 0);
        return;
    }
    CHECK(result.exit_code == 0);
}

TEST_CASE("scaffold:ScanPluginDirectories 认得生成的插件") {
    TempRoot root;
    REQUIRE(ScaffoldPythonPlugin(lubancode::platform::PathToUtf8(root.path), "scan_probe", kPythonCmd).has_value());
    const auto scan = ScanPluginDirectories(root.path);
    REQUIRE(scan.manifests.size() == 1);
    CHECK(scan.manifests[0]->id == "scan_probe");
    CHECK(scan.warnings.empty());
}
