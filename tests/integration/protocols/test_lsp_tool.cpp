// lsp 工具测试,分三块:
//   1) 纯函数格式化:definition(Location/LocationLink/null)、references
//      (截断)、symbols(层级/平铺)、diagnostics(nullopt/空/多条)。
//   2) 路由与参数校验:mode 不认得、缺 line/character、扩展名没配置、文件
//      读不到——全都不起任何服务器进程就快速失败。
//   3) 真夹具全链路:config 指 python 夹具(tests/fixtures/lsp_test_server.py),
//      真调 lsp 工具四个 mode 验证输出;外加懒启动/闲置关停/重启
//      (SetIdleMillisForTest 把阈值收窄到毫秒)和"找不到命令指路安装"。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "lsp/manager.hpp"
#include "tools/lsp_tool.hpp"

using namespace lubancode;

#ifndef LUBANCODE_TEST_FIXTURES_DIR
#define LUBANCODE_TEST_FIXTURES_DIR "."
#endif

namespace {

// 造一份指到 Python 夹具的 lsp 配置(语言名 python,管 .py)。真夹具用
// 哪个 python:Windows 装的是 python.exe,Linux/macOS 发行版惯例是
// python3(裸 python 常常不存在)。
std::map<std::string, config::LspServerConfig> FixtureConfigs() {
    config::LspServerConfig server;
#ifdef _WIN32
    server.command = "python";
#else
    server.command = "python3";
#endif
    server.args = {std::string(LUBANCODE_TEST_FIXTURES_DIR) + "/lsp_test_server.py"};
    server.extensions = {".py"};
    std::map<std::string, config::LspServerConfig> configs;
    configs.emplace("python", std::move(server));
    return configs;
}

// 临时目录里写一个已知内容的 .py 文件,返回 UTF-8 路径。
std::string WriteSampleFile(const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "import os\n"          // 0 基第 0 行
         << "\n"                    // 1
         << "def top_func():\n"    // 2(夹具 definition 指到这行)
         << "    inner = 1\n"      // 3
         << "    top_func()\n";    // 4
    file.close();
    return path.string();
}

}  // namespace

// ---------------------------------------------------------------------------
// 1) 纯函数格式化
// ---------------------------------------------------------------------------

TEST_CASE("FormatLspDefinition: Location 数组 -> 文件:行:列(1 基)+ 该行文本") {
    const nlohmann::json result = nlohmann::json::array(
        {{{"uri", "file:///D:/proj/a.py"},
          {"range", {{"start", {{"line", 2}, {"character", 4}}}, {"end", {{"line", 2}, {"character", 12}}}}}}});
    const auto reader = [](const std::string& path, int line) -> std::optional<std::string> {
        CHECK(line == 2);
        CHECK(path.find("a.py") != std::string::npos);
        return "def top_func():";
    };
    const std::string text = tools::FormatLspDefinition(result, reader);
    CHECK(text.find(":3:5") != std::string::npos);
    CHECK(text.find("def top_func():") != std::string::npos);
}

TEST_CASE("FormatLspDefinition: LocationLink 数组取 targetSelectionRange") {
    const nlohmann::json result = nlohmann::json::array(
        {{{"targetUri", "file:///D:/b.cpp"},
          {"targetRange", {{"start", {{"line", 10}, {"character", 0}}}, {"end", {{"line", 20}, {"character", 0}}}}},
          {"targetSelectionRange",
           {{"start", {{"line", 11}, {"character", 6}}}, {"end", {{"line", 11}, {"character", 9}}}}}}});
    const std::string text = tools::FormatLspDefinition(result, nullptr);
    CHECK(text.find(":12:7") != std::string::npos);
}

TEST_CASE("FormatLspDefinition: null/空数组 -> 没找到定义") {
    CHECK(tools::FormatLspDefinition(nlohmann::json(nullptr), nullptr) == "没找到定义。");
    CHECK(tools::FormatLspDefinition(nlohmann::json::array(), nullptr) == "没找到定义。");
}

TEST_CASE("FormatLspDefinition: 行文本读不到(reader 给 nullopt)只列位置,不崩") {
    const nlohmann::json result = nlohmann::json::array(
        {{{"uri", "file:///D:/gone.py"},
          {"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", 0}, {"character", 1}}}}}}});
    const auto reader = [](const std::string&, int) -> std::optional<std::string> { return std::nullopt; };
    const std::string text = tools::FormatLspDefinition(result, reader);
    CHECK(text.find(":1:1") != std::string::npos);
}

TEST_CASE("FormatLspReferences: 列表 + 超上限截断注明") {
    nlohmann::json result = nlohmann::json::array();
    for (int i = 0; i < 5; ++i) {
        result.push_back({{"uri", "file:///D:/r.py"},
                           {"range", {{"start", {{"line", i}, {"character", 0}}}, {"end", {{"line", i}, {"character", 1}}}}}});
    }
    const std::string text = tools::FormatLspReferences(result, 3);
    CHECK(text.find("共 5 处引用") != std::string::npos);
    CHECK(text.find("只列前 3 处") != std::string::npos);
    CHECK(text.find(":1:1") != std::string::npos);
    CHECK(text.find(":3:1") != std::string::npos);
    CHECK(text.find(":5:1") == std::string::npos);  // 第 5 处被截掉了
}

TEST_CASE("FormatLspReferences: 空结果 -> 没找到引用") {
    CHECK(tools::FormatLspReferences(nlohmann::json::array()) == "没找到引用。");
    CHECK(tools::FormatLspReferences(nlohmann::json(nullptr)) == "没找到引用。");
}

TEST_CASE("FormatLspSymbols: 层级式 DocumentSymbol 缩进展开,带种类和 1 基行号") {
    const nlohmann::json result = nlohmann::json::array(
        {{{"name", "MyClass"},
          {"kind", 5},
          {"selectionRange", {{"start", {{"line", 4}, {"character", 6}}}, {"end", {{"line", 4}, {"character", 13}}}}},
          {"children",
           nlohmann::json::array(
               {{{"name", "run"},
                 {"kind", 6},
                 {"selectionRange",
                  {{"start", {{"line", 6}, {"character", 8}}}, {"end", {{"line", 6}, {"character", 11}}}}}}})}}});
    const std::string text = tools::FormatLspSymbols(result);
    CHECK(text.find("MyClass [类] 第 5 行") != std::string::npos);
    CHECK(text.find("  - run [方法] 第 7 行") != std::string::npos);
}

TEST_CASE("FormatLspSymbols: 平铺式 SymbolInformation(location.range)也认") {
    const nlohmann::json result = nlohmann::json::array(
        {{{"name", "main"},
          {"kind", 12},
          {"location",
           {{"uri", "file:///D:/m.py"},
            {"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", 2}, {"character", 0}}}}}}}}});
    const std::string text = tools::FormatLspSymbols(result);
    CHECK(text.find("main [函数] 第 1 行") != std::string::npos);
}

TEST_CASE("FormatLspSymbols: 空结果 -> 没找到符号") {
    CHECK(tools::FormatLspSymbols(nlohmann::json::array()) == "没找到符号。");
    CHECK(tools::FormatLspSymbols(nlohmann::json(nullptr)) == "没找到符号。");
}

TEST_CASE("FormatLspDiagnostics: nullopt -> 暂无;空数组 -> 没问题;多条带严重度和 1 基行号") {
    CHECK(tools::FormatLspDiagnostics(std::nullopt).find("暂无诊断") != std::string::npos);
    CHECK(tools::FormatLspDiagnostics(nlohmann::json::array()) == "没有诊断问题。");

    const nlohmann::json diagnostics = nlohmann::json::array(
        {{{"range", {{"start", {{"line", 9}, {"character", 0}}}, {"end", {{"line", 9}, {"character", 5}}}}},
          {"severity", 1},
          {"message", "变量未定义"}},
         {{"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", 0}, {"character", 3}}}}},
          {"severity", 2},
          {"message", "没用上的 import"}}});
    const std::string text = tools::FormatLspDiagnostics(diagnostics);
    CHECK(text.find("共 2 条诊断") != std::string::npos);
    CHECK(text.find("[错误] 第 10 行: 变量未定义") != std::string::npos);
    CHECK(text.find("[警告] 第 1 行: 没用上的 import") != std::string::npos);
}

TEST_CASE("InstallHintForCommand: clangd 指路 winget,不认识的给通用提示") {
    CHECK(lsp::InstallHintForCommand("clangd").find("winget install LLVM.clangd") != std::string::npos);
    CHECK(lsp::InstallHintForCommand("some-server").find("PATH") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 2) 路由与参数校验(不起任何进程)
// ---------------------------------------------------------------------------

TEST_CASE("Manager::LanguageForFile: 按扩展名路由,大小写不敏感,没人管的给 nullopt") {
    lsp::Manager manager(FixtureConfigs(), "D:\\proj");
    CHECK(manager.LanguageForFile("a/b/c.py") == "python");
    CHECK(manager.LanguageForFile("D:\\X\\Y.PY") == "python");
    CHECK_FALSE(manager.LanguageForFile("a.rs").has_value());
    CHECK_FALSE(manager.LanguageForFile("no_extension").has_value());
}

TEST_CASE("LspTool: 参数校验快速失败,服务器保持未启动") {
    lsp::Manager manager(FixtureConfigs(), "D:\\proj");
    tools::LspTool tool(manager);

    // mode 不认得
    auto r1 = tool.execute({{"mode", "rename"}, {"file", "a.py"}});
    CHECK(r1.is_error);
    CHECK(r1.content.find("mode") != std::string::npos);

    // 缺 file
    auto r2 = tool.execute({{"mode", "symbols"}});
    CHECK(r2.is_error);
    CHECK(r2.content.find("file") != std::string::npos);

    // 定位类缺 line/character
    auto r3 = tool.execute({{"mode", "definition"}, {"file", "a.py"}});
    CHECK(r3.is_error);
    CHECK(r3.content.find("line") != std::string::npos);

    // 1 基参数不许 <1
    auto r4 = tool.execute({{"mode", "references"}, {"file", "a.py"}, {"line", 0}, {"character", 1}});
    CHECK(r4.is_error);
    CHECK(r4.content.find("1 基") != std::string::npos);

    // 扩展名没配置
    auto r5 = tool.execute({{"mode", "symbols"}, {"file", "a.rs"}});
    CHECK(r5.is_error);
    CHECK(r5.content.find("lsp") != std::string::npos);

    // 文件读不到
    auto r6 = tool.execute({{"mode", "symbols"}, {"file", "D:\\lubancode_definitely_missing_file.py"}});
    CHECK(r6.is_error);
    CHECK(r6.content.find("读不到") != std::string::npos);

    // 上面这些全都不该起进程。
    const auto statuses = manager.StatusList();
    REQUIRE(statuses.size() == 1);
    CHECK(statuses[0].state == "未启动");
}

TEST_CASE("LspTool: 找不到服务器命令,is_error 指路安装") {
    std::map<std::string, config::LspServerConfig> configs;
    config::LspServerConfig server;
    server.command = "lubancode_no_such_lsp_cmd_xyz";
    server.extensions = {".py"};
    configs.emplace("python", std::move(server));
    lsp::Manager manager(std::move(configs), "D:\\proj");
    tools::LspTool tool(manager);

    const std::string sample = WriteSampleFile("lubancode_lsp_missing_cmd.py");
    auto result = tool.execute({{"mode", "symbols"}, {"file", sample}});
    CHECK(result.is_error);
#ifdef _WIN32
    CHECK(result.content.find("未找到") != std::string::npos);
    CHECK(result.content.find("PATH") != std::string::npos);  // 通用安装指路
#endif
}

// ---------------------------------------------------------------------------
// 3) 真夹具全链路(两平台都跑,StdioTransport 底下是 platform::ChildProcess)
// ---------------------------------------------------------------------------

TEST_CASE("LspTool + 真实 Python 夹具:四个 mode 全链路,懒启动只在首次查询发生") {
    lsp::Manager manager(FixtureConfigs(), std::filesystem::temp_directory_path().string());
    tools::LspTool tool(manager);
    const std::string sample = WriteSampleFile("lubancode_lsp_sample.py");

    // 还没查过,服务器未启动(懒启动)。
    CHECK(manager.StatusList()[0].state == "未启动");

    // definition:夹具指到 0 基第 2 行第 4 列 -> 1 基 3:5,附该行文本。
    auto definition = tool.execute({{"mode", "definition"}, {"file", sample}, {"line", 5}, {"character", 5}});
    REQUIRE_MESSAGE(!definition.is_error, definition.content);
    CHECK(definition.content.find(":3:5") != std::string::npos);
    CHECK(definition.content.find("def top_func():") != std::string::npos);
    CHECK(manager.StatusList()[0].state == "运行中");

    // references:夹具给两处。
    auto references = tool.execute({{"mode", "references"}, {"file", sample}, {"line", 3}, {"character", 5}});
    REQUIRE_MESSAGE(!references.is_error, references.content);
    CHECK(references.content.find("共 2 处引用") != std::string::npos);
    CHECK(references.content.find(":1:1") != std::string::npos);
    CHECK(references.content.find(":5:5") != std::string::npos);

    // symbols:层级式,top_func 带子符号 inner。
    auto symbols = tool.execute({{"mode", "symbols"}, {"file", sample}});
    REQUIRE_MESSAGE(!symbols.is_error, symbols.content);
    CHECK(symbols.content.find("top_func [函数] 第 3 行") != std::string::npos);
    CHECK(symbols.content.find("  - inner [变量] 第 4 行") != std::string::npos);

    // diagnostics:didOpen 后夹具推了一条假警告,读缓存。
    auto diagnostics = tool.execute({{"mode", "diagnostics"}, {"file", sample}});
    REQUIRE_MESSAGE(!diagnostics.is_error, diagnostics.content);
    CHECK(diagnostics.content.find("[警告]") != std::string::npos);
    CHECK(diagnostics.content.find("fixture-warning") != std::string::npos);

    manager.ShutdownAll();
}

TEST_CASE("Manager: 闲置关停 + 下次调用自动重启(阈值收窄到毫秒实测)") {
    lsp::Manager manager(FixtureConfigs(), std::filesystem::temp_directory_path().string());
    manager.SetIdleMillisForTest(30);

    // 首次取用:懒启动成功。
    const auto first = manager.AcquireClient("python");
    REQUIRE_MESSAGE(first.has_value(), first.error());
    CHECK(manager.StatusList()[0].state == "运行中");

    // 睡过闲置阈值,StatusList 顺手收割 -> 已闲置关停。
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK(manager.StatusList()[0].state == "已闲置关停");

    // 再取用:自动重启,又是运行中,拿到的客户端能干活。
    const auto second = manager.AcquireClient("python");
    REQUIRE_MESSAGE(second.has_value(), second.error());
    CHECK(manager.StatusList()[0].state == "运行中");
    CHECK((*second)->Alive());

    manager.ShutdownAll();
    CHECK(manager.StatusList()[0].state == "未启动");
}
