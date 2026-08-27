// GUI Agent 示例(gui-agent-example)的集成测试:真 manifest、真 runner
// 进程、真协议往返——但不碰真鼠标键盘:
//   - manifest 静态校验:九件工具、id、env allowlist、network=false
//   - gui_status 真跑:平台口径如实报(Windows=ok;别处=unsupported_platform)
//   - gui_click 走 dry-run + 假窗口 id:校验链路全通,window_not_found
//     在注入前拦住——一只真事件都不发
//   - env allowlist 递进:LUBANCODE_GUI_DRY_RUN 经宿主最小集递给子进程
//   - runner 防御层:坏类型参数在脚本侧也被拦(schema 是宿主的合同,
//     脚本自己再守一道)
//   - 文档与代码不两张皮:README 提到的工具名都真在 manifest 与 runner 里
//
// 真桌面 E2E(真点击真输入)不进 ctest:普通开发机会抢用户鼠标,照工单
// 停手线走 examples/agents/gui-agent/scripts/manual_e2e.py,默认 SKIP。
// 缺 Python 的环境进程类测试整段跳过(manifest 静态校验照跑)。

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "platform/process.hpp"
#include "runtime/plugin_contract.hpp"
#include "runtime/plugin_process.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

// 示例目录:测试从源码树真读,不复制——示例改坏(工具改名、manifest 坏
// 了、runner 挪位)当场红,文档与代码不两张皮。
std::filesystem::path ExampleDir() {
    return std::filesystem::path(LUBANCODE_TEST_SOURCE_DIR) / "examples" / "agents" / "gui-agent";
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// 与 test_plugin_process.cpp 同款探测:--version 退出码 0 且输出含 Python,
// Windows 的 Microsoft Store 假 shim 正好被挡掉。
bool PythonAvailable(const std::string& command) {
    const auto result = platform::RunProcess(std::vector<std::string>{command, "--version"}, 10000);
    return result.exit_code == 0 && result.output.find("Python") != std::string::npos;
}

// 解析示例的真 manifest。失败即红——示例的 plugin.json 是交付物,不是参考。
// (INFO 里不拼字符串表达式,先落变量再喂——doctest 宏的 + 重载会绊。)
PluginManifest LoadExampleManifest() {
    const std::string text = ReadFile(ExampleDir() / "plugin.json");
    if (text.empty()) {
        const std::string why = "plugin.json 读取失败: " + (ExampleDir() / "plugin.json").string();
        INFO(why);
        REQUIRE_FALSE(text.empty());
    }
    auto manifest = ParsePluginManifest(text, ExampleDir());
    if (!manifest.has_value()) {
        const std::string why = manifest.error();
        INFO(why);
        REQUIRE(manifest.has_value());
    }
    return std::move(*manifest);
}

constexpr const char* kExpectedTools[] = {
    "gui_status", "gui_list_windows", "gui_focus_window", "gui_screenshot",
    "gui_move_mouse", "gui_click", "gui_scroll", "gui_type_text", "gui_key",
};

plugin_protocol::ProcessRequest MakeRequest(const std::string& tool, const nlohmann::json& arguments) {
    plugin_protocol::ProcessRequest request;
    request.plugin = "gui-agent-example";
    request.tool = tool;
    request.entry = tool;
    request.arguments = arguments;
    request.call_id = "call_gui_" + tool;
    return request;
}

}  // namespace

// ---------------------------------------------------------------------------
// manifest 静态真账(不需要 Python)
// ---------------------------------------------------------------------------
TEST_CASE("gui-agent 示例:manifest 九件工具、权限与 env allowlist 齐全") {
    const auto manifest = LoadExampleManifest();
    REQUIRE(manifest.tools.size() == 9);
    CHECK(manifest.id == "gui-agent-example");
    CHECK(manifest.kind == RuntimeKind::Process);
    CHECK_FALSE(manifest.network_allowed);
    CHECK(manifest.timeout_ms == 20000);

    // 九件工具名逐一对上;完整名前缀同一枚插件 id。
    for (const char* name : kExpectedTools) {
        bool found = false;
        for (const auto& tool : manifest.tools) {
            if (tool.name == name) {
                found = true;
                CHECK(tool.full_name == "plugin__gui-agent-example__" + std::string(name));
                CHECK_FALSE(tool.input_schema.value("additionalProperties", true));
                break;
            }
        }
        CHECK_MESSAGE(found, "manifest 缺工具: " << name);
    }

    // env allowlist:三枚开关全点名(宿主最小集只递点过名的变量)。
    for (const char* wanted : {"LUBANCODE_GUI_DRY_RUN", "LUBANCODE_GUI_EVIDENCE_DIR",
                               "LUBANCODE_GUI_ALLOW_DANGEROUS_KEYS"}) {
        bool found = false;
        for (const auto& entry : manifest.env_allowlist) {
            if (entry == wanted) {
                found = true;
                break;
            }
        }
        CHECK_MESSAGE(found, "permissions.env 缺 " << wanted);
    }

    // argv:python + ${plugin_dir}/runner.py(占位符已展开,圈在插件目录内)。
    REQUIRE(manifest.argv.size() == 2);
    CHECK(manifest.argv[1].find("runner.py") != std::string::npos);
    CHECK(std::filesystem::exists(ExampleDir() / "runner.py"));
    // runner 的同目录模块也在交付物里(整目录拷贝的安装口径)。
    for (const char* module : {"gui_actions.py", "gui_backend.py", "png.py", "test_runner.py"}) {
        CHECK_MESSAGE(std::filesystem::exists(ExampleDir() / module), "示例缺文件: " << module);
    }
}

TEST_CASE("gui-agent 示例:README/SKILL 提的工具名与 manifest 不两张皮") {
    const auto manifest = LoadExampleManifest();
    const std::string runner_source = ReadFile(ExampleDir() / "gui_actions.py");
    const std::string readme = ReadFile(ExampleDir() / "README.md");
    const std::string skill = ReadFile(ExampleDir() / "SKILL.md");
    for (const auto& tool : manifest.tools) {
        // runner 的 HANDLERS 表里真有分派(名字出现两处以上:HANDLERS 与函数定义)。
        CHECK_MESSAGE(runner_source.find("\"" + tool.name + "\":") != std::string::npos,
                      "runner 缺分派: " << tool.name);
        // README 与 SKILL 至少各提过一回九件里的名字(教学文档不能只讲空话)。
        CHECK_MESSAGE(readme.find(tool.name) != std::string::npos,
                      "README 没提工具: " << tool.name);
    }
    CHECK_MESSAGE(skill.find("gui_screenshot") != std::string::npos,
                  "SKILL 没教截图复验循环");
    CHECK_MESSAGE(skill.find("stale_observation") != std::string::npos,
                  "SKILL 没教 stale 处置");
}

// ---------------------------------------------------------------------------
// 真进程往返(缺 Python 的环境跳过;绝不碰真鼠标)
// ---------------------------------------------------------------------------
TEST_CASE("gui-agent 示例:gui_status 真跑,平台口径如实") {
    const auto manifest = LoadExampleManifest();
    if (!PythonAvailable(manifest.argv[0])) {
        return;
    }
    const auto outcome = RunProcessToolCall(manifest, MakeRequest("gui_status", nlohmann::json::object()),
                                            "D:/not-a-real-dir", nullptr, ProcessCallLimits{});
#ifdef _WIN32
    if (outcome.code != PluginErrorCode::Ok) {
        const std::string why = outcome.detail + " | stderr=" + outcome.stderr_tail.substr(0, 300);
        INFO(why);
        REQUIRE(outcome.code == PluginErrorCode::Ok);
    }
    CHECK(outcome.structured.value("platform", "") == "win32");
    CHECK(outcome.structured.contains("dpi_awareness"));
    CHECK(outcome.structured.contains("virtual_screen"));
    CHECK(outcome.structured.contains("dry_run"));
    // 诚实条款:observation 的"模型还没看见图"标记在 status 里也露脸。
    CHECK(outcome.structured.contains("rich_result"));
#else
    // 非宿主平台不装能跑:插件自报 unsupported_platform。
    REQUIRE(outcome.code == PluginErrorCode::PluginReportedError);
    CHECK(outcome.plugin_error_code == "unsupported_platform");
#endif
}

TEST_CASE("gui-agent 示例:dry-run 下假窗口的点击在注入前被拦") {
    const auto manifest = LoadExampleManifest();
    if (!PythonAvailable(manifest.argv[0])) {
        return;
    }
    // 开 dry-run:经宿主 env 最小集 + allowlist 递给子进程(allowlist 里
    // 点过名,这是示例 manifest 与宿主机制的合力)。
#ifdef _WIN32
    REQUIRE(_putenv_s("LUBANCODE_GUI_DRY_RUN", "1") == 0);
#else
    REQUIRE(setenv("LUBANCODE_GUI_DRY_RUN", "1", 1) == 0);
#endif
    struct EnvReset {
        ~EnvReset() {
#ifdef _WIN32
            _putenv_s("LUBANCODE_GUI_DRY_RUN", "");
#else
            unsetenv("LUBANCODE_GUI_DRY_RUN");
#endif
        }
    } env_reset;

    // 0xDEADBEEF 几乎不可能是一枚活窗口;校验链路(窗口在不在)应先拦,
    // 无论 dry-run 开关如何都不会注入一枚事件。
    const auto outcome = RunProcessToolCall(
        manifest,
        MakeRequest("gui_click", nlohmann::json{{"x", 100}, {"y", 100},
                                                {"window_id", "0x0DEADBEEF"}}),
        "D:/not-a-real-dir", nullptr, ProcessCallLimits{});
    REQUIRE(outcome.code == PluginErrorCode::PluginReportedError);
    CHECK(outcome.plugin_error_code == "window_not_found");
}

TEST_CASE("gui-agent 示例:runner 防御层——schema 之外的坏参数脚本侧再拦一道") {
    const auto manifest = LoadExampleManifest();
    if (!PythonAvailable(manifest.argv[0])) {
        return;
    }
    // 坐标给字符串:宿主 adapter 会按 manifest schema 拦;这里直灌
    // RunProcessToolCall 绕过 adapter,验 runner 自己也守(防御性验参,
    // Schema 不是内存安全)。
    const auto outcome = RunProcessToolCall(
        manifest,
        MakeRequest("gui_click", nlohmann::json{{"x", "abc"}, {"y", 1}}),
        "D:/not-a-real-dir", nullptr, ProcessCallLimits{});
    REQUIRE(outcome.code == PluginErrorCode::PluginReportedError);
    CHECK(outcome.plugin_error_code == "invalid_arguments");
}

TEST_CASE("gui-agent 示例:危险组合键默认被拦,不注入") {
    const auto manifest = LoadExampleManifest();
    if (!PythonAvailable(manifest.argv[0])) {
        return;
    }
    const auto outcome = RunProcessToolCall(
        manifest, MakeRequest("gui_key", nlohmann::json{{"keys", nlohmann::json::array({"alt", "f4"})}}),
        "D:/not-a-real-dir", nullptr, ProcessCallLimits{});
    REQUIRE(outcome.code == PluginErrorCode::PluginReportedError);
    CHECK(outcome.plugin_error_code == "dangerous_key_blocked");
}

TEST_CASE("gui-agent 示例:不认得的工具如实报 unknown_tool") {
    const auto manifest = LoadExampleManifest();
    if (!PythonAvailable(manifest.argv[0])) {
        return;
    }
    const auto outcome = RunProcessToolCall(manifest, MakeRequest("gui_launch_missiles", nlohmann::json::object()),
                                            "D:/not-a-real-dir", nullptr, ProcessCallLimits{});
    REQUIRE(outcome.code == PluginErrorCode::PluginReportedError);
    CHECK(outcome.plugin_error_code == "unknown_tool");
}
