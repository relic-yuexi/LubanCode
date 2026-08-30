// GUI Agent 示例(gui-agent-example)的集成测试:真 manifest、真 runner
// 进程、真协议往返——但不碰真鼠标键盘:
//   - manifest 静态校验:十二件工具、id、env allowlist、network=false
//   - gui_status 真跑:平台口径如实报(Windows=ok;别处=unsupported_platform)
//   - gui_click 走 dry-run + 假窗口 id:校验链路全通,window_not_found
//     在注入前拦住——一只真事件都不发
//   - gui_snapshot 走假窗口 id:快照链路真起进程,window_not_found 兜底
//   - env allowlist 递进:LUBANCODE_GUI_DRY_RUN 经宿主最小集递给子进程
//   - runner 防御层:坏类型参数在脚本侧也被拦(schema 是宿主的合同,
//     脚本自己再守一道)
//   - 文档与代码不两张皮:README 提到的工具名都真在 manifest 与 runner 里
//
// 真桌面 E2E(真点击真输入)不进 ctest:普通开发机会抢用户鼠标,照工单
// 停手线走 examples/packages/gui-agent/plugins/gui-agent-example/scripts/
// manual_e2e.py(视觉路)与 scripts/uia_snapshot_e2e.py(结构路),默认 SKIP。
// 缺 Python 的环境进程类测试整段跳过(manifest 静态校验照跑)。

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "platform/process.hpp"
#include "runtime/plugin_contract.hpp"
#include "runtime/plugin_process.hpp"
#include "runtime/plugin_tool.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

// 示例目录:测试从源码树真读,不复制——示例改坏(工具改名、manifest 坏
// 了、runner 挪位)当场红,文档与代码不两张皮。示例已改造成 Package
// (examples/packages/gui-agent):插件本体在 plugins/,README 在包根,
// SKILL 在 skills/gui-agent/。
std::filesystem::path ExampleDir() {
    return std::filesystem::path(LUBANCODE_TEST_SOURCE_DIR) / "examples" / "packages" / "gui-agent" /
           "plugins" / "gui-agent-example";
}

std::filesystem::path PackageReadmePath() {
    return std::filesystem::path(LUBANCODE_TEST_SOURCE_DIR) / "examples" / "packages" / "gui-agent" /
           "README.md";
}

std::filesystem::path SkillFilePath() {
    return std::filesystem::path(LUBANCODE_TEST_SOURCE_DIR) / "examples" / "packages" / "gui-agent" /
           "skills" / "gui-agent" / "SKILL.md";
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// 与 test_plugin_process.cpp 同款探测:--version 退出码 0 且输出含 Python,
// Windows 的 Microsoft Store 假 shim 正好被挡掉。
bool PythonAvailable(const std::string& command) {
    const auto result = platform::RunProcess(std::vector<std::string>{command, "--version"}, 10000);
    // 只认 Python 3:macOS 的 /usr/bin/python 是 Python 2 桩,"Python 2.7" 也
    // 含 "Python"——旧门放行,py2 跑 py3 代码非零退,REQUIRE(4==13) 就这么
    // 来的。py2 一律按"不可用"跳过本组真机案。
    return result.exit_code == 0 && result.output.find("Python 3") != std::string::npos;
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
    "gui_snapshot", "gui_move_mouse", "gui_click", "gui_scroll",
    "gui_type_text", "gui_key", "gui_set_value", "gui_invoke",
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
TEST_CASE("gui-agent 示例:manifest 十二件工具、权限与 env allowlist 齐全") {
    const auto manifest = LoadExampleManifest();
    REQUIRE(manifest.tools.size() == 12);
    CHECK(manifest.id == "gui-agent-example");
    CHECK(manifest.kind == RuntimeKind::Process);
    CHECK_FALSE(manifest.network_allowed);
    CHECK(manifest.timeout_ms == 20000);

    // 十件工具名逐一对上;完整名前缀同一枚插件 id。
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
    for (const char* module : {"gui_actions.py", "gui_backend.py", "gui_uia.py", "png.py",
                               "test_runner.py"}) {
        CHECK_MESSAGE(std::filesystem::exists(ExampleDir() / module), "示例缺文件: " << module);
    }
}

TEST_CASE("gui-agent 示例:README/SKILL 提的工具名与 manifest 不两张皮") {
    const auto manifest = LoadExampleManifest();
    const std::string runner_source = ReadFile(ExampleDir() / "gui_actions.py");
    const std::string readme = ReadFile(PackageReadmePath());
    const std::string skill = ReadFile(SkillFilePath());
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
TEST_CASE("gui-agent 示例:gui_screenshot 真跑——v2 image 块随响应回,宿主落账成 ImageContent") {
    const auto manifest = LoadExampleManifest();
    if (!PythonAvailable(manifest.argv[0])) {
        return;
    }
#ifdef _WIN32
    // 纯观察链(不注入任何输入):先枚举窗口,再对第一枚可见窗口截图。
    const auto listed = RunProcessToolCall(manifest, MakeRequest("gui_list_windows", nlohmann::json::object()),
                                           "D:/not-a-real-dir", nullptr, ProcessCallLimits{});
    if (listed.code != PluginErrorCode::Ok) {
        return;  // 枚举不到窗口的极端环境(无桌面会话)按跳过收
    }
    const std::string window_id = listed.structured.value("windows", nlohmann::json::array())
                                      .at(0)
                                      .value("window_id", "");
    if (window_id.empty()) {
        return;
    }

    // 走 adapter 全链:宿主验身(魔数/帽)→ 落 artifact → payload 带图。
    auto shared = std::make_shared<const PluginManifest>(std::move(manifest));
    PluginToolAdapter adapter(shared, &shared->tools[0]);
    const std::filesystem::path artifacts =
        std::filesystem::temp_directory_path() / "lubancode_gui_shot_artifacts";
    const auto result = adapter.execute(nlohmann::json{{"target", "window"}, {"window_id", window_id}},
                                        tools::ToolExecutionContext{nullptr, artifacts.generic_string()});
    if (result.is_error) {
        // 目标窗口可能在两步之间关了/最小化了——那是真桌面的正常变数,
        // 不算红;只拒真正落不了账的错。
        const bool desktop_churn = result.error_code == "plugin.window_minimized" ||
                                   result.error_code == "plugin.window_not_found";
        INFO(result.content);
        CHECK(desktop_churn);
        return;
    }
    const auto* image = std::get_if<tools::ImageContent>(&result.payload.content.back());
    REQUIRE(image != nullptr);
    CHECK(image->artifact.stored);
    CHECK(image->artifact.filename.rfind("art-", 0) == 0);
    CHECK(std::filesystem::exists(artifacts / image->artifact.filename));
    CHECK(image->bytes > 0);
#endif
}

TEST_CASE("gui-agent 示例:gui_snapshot 走假窗口 id,window_not_found 兜底") {
    const auto manifest = LoadExampleManifest();
    if (!PythonAvailable(manifest.argv[0])) {
        return;
    }
    // 快照是观察类(不注入输入);假窗口先过窗口现场重查,不到 UIA 那步
    // 就该被拦——链路真起进程、真走协议,一只 COM 调用都不发。
    const auto outcome = RunProcessToolCall(
        manifest, MakeRequest("gui_snapshot", nlohmann::json{{"window_id", "0x0DEADBEEF"}}),
        "D:/not-a-real-dir", nullptr, ProcessCallLimits{});
    REQUIRE(outcome.code == PluginErrorCode::PluginReportedError);
    CHECK(outcome.plugin_error_code == "window_not_found");
}

TEST_CASE("gui-agent 示例:结构路动作走假窗口 id,同样 window_not_found 兜底") {
    const auto manifest = LoadExampleManifest();
    if (!PythonAvailable(manifest.argv[0])) {
        return;
    }
    // set_value/invoke 是写动作但不注入输入;假窗口先过窗口现场重查,
    // 一枚 pattern 调用都不发就拦住。
    const auto set_value = RunProcessToolCall(
        manifest, MakeRequest("gui_set_value", nlohmann::json{{"window_id", "0x0DEADBEEF"},
                                                              {"ref", "e1"},
                                                              {"text", "x"}}),
        "D:/not-a-real-dir", nullptr, ProcessCallLimits{});
    REQUIRE(set_value.code == PluginErrorCode::PluginReportedError);
    CHECK(set_value.plugin_error_code == "window_not_found");

    const auto invoke = RunProcessToolCall(
        manifest, MakeRequest("gui_invoke", nlohmann::json{{"window_id", "0x0DEADBEEF"},
                                                           {"ref", "e1"},
                                                           {"action", "invoke"}}),
        "D:/not-a-real-dir", nullptr, ProcessCallLimits{});
    REQUIRE(invoke.code == PluginErrorCode::PluginReportedError);
    CHECK(invoke.plugin_error_code == "window_not_found");
}

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
    // 协议 v2:回喂能力在 status 里翻真(rich_result=true)。
    CHECK(outcome.structured.contains("rich_result"));
    CHECK(outcome.structured["rich_result"].value("rich_result", false) == true);
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
