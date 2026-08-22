// M7:插件工具测试。
//
// PluginTool 这部分分两路:
//   1. 进程内假 tool_def——不经 DLL,函数指针直接指向测试自己的函数,验证
//      名字前缀、schema 解析、"结果拷贝后立刻 free_result"这套跨堆规矩;
//   2. 真 DLL——tests/CMakeLists 把 examples/plugins/hello_plugin 和
//      fixtures/bad_version_plugin.c 各编成一个 DLL 放在同一个目录
//      (LUBANCODE_TEST_PLUGIN_DIR),用 PluginHost 真加载:hello 挂上、
//      bad_version 因 api_version 不合被跳过并出警告。
//
// LuaTool 全走 LoadFromScript(不用落盘),覆盖:正常表、缺字段、execute
// 抛错、输入 JSON->lua 表转换(嵌套/数组/类型)、中文往返、schema 两种
// 写法;目录扫描(好文件 + 坏文件)另开临时目录测。

#include <doctest/doctest.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "platform/dynamic_library.hpp"
#include "tools/lua_tool.hpp"
#include "tools/plugin_loader.hpp"
#include "tools/registry.hpp"

using namespace lubancode;

// ---------------------------------------------------------------------------
// PluginTool:进程内假 tool_def
// ---------------------------------------------------------------------------

namespace {

std::atomic<int> g_free_calls{0};
std::string g_last_input;

luban_tool_result FakeExecute(const char* input_json) {
    g_last_input = input_json != nullptr ? input_json : "";
    luban_tool_result r;
    char* buffer = static_cast<char*>(std::malloc(16));
    std::memcpy(buffer, "ok:结果", sizeof("ok:结果"));
    r.content = buffer;
    r.is_error = 0;
    return r;
}

luban_tool_result FakeExecuteError(const char* input_json) {
    (void)input_json;
    luban_tool_result r;
    char* buffer = static_cast<char*>(std::malloc(32));
    std::memcpy(buffer, "出错了", sizeof("出错了"));
    r.content = buffer;
    r.is_error = 1;
    return r;
}

void FakeFree(luban_tool_result* result) {
    g_free_calls.fetch_add(1);
    if (result != nullptr && result->content != nullptr) {
        std::free(const_cast<char*>(result->content));
        result->content = nullptr;
    }
}

}  // namespace

TEST_CASE("PluginTool: 名字前缀 plugin__<dll名>__<工具名>,needs_confirm 恒真") {
    luban_tool_def def{};
    def.name = "reverse_text";
    def.description = "倒序";
    def.input_schema_json = R"({"type":"object","properties":{"text":{"type":"string"}}})";
    def.execute = FakeExecute;
    def.free_result = FakeFree;

    tools::PluginTool tool("hello_plugin", &def);
    CHECK(tool.name() == "plugin__hello_plugin__reverse_text");
    CHECK(tool.needs_confirm());
    CHECK(tool.description() == "[plugin:hello_plugin] 倒序");
    CHECK(tool.input_schema()["properties"].contains("text"));
}

TEST_CASE("PluginTool: 结果拷成 std::string 后立刻调 free_result,入参按 JSON 文本传入") {
    luban_tool_def def{};
    def.name = "t";
    def.description = "";
    def.input_schema_json = nullptr;  // 没给 schema,退化成 {"type":"object"}
    def.execute = FakeExecute;
    def.free_result = FakeFree;

    tools::PluginTool tool("p", &def);
    CHECK(tool.input_schema() == nlohmann::json{{"type", "object"}});

    g_free_calls = 0;
    const auto result = tool.execute(nlohmann::json{{"text", "你好"}});
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "ok:结果");
    CHECK(g_free_calls.load() == 1);  // 拷完立刻交还插件释放,一次不多一次不少
    // 入参是序列化好的 JSON 文本(UTF-8 原样,不转义)
    CHECK(nlohmann::json::parse(g_last_input) == nlohmann::json{{"text", "你好"}});
}

TEST_CASE("PluginTool: is_error 原样透传") {
    luban_tool_def def{};
    def.name = "t";
    def.execute = FakeExecuteError;
    def.free_result = FakeFree;

    tools::PluginTool tool("p", &def);
    g_free_calls = 0;
    const auto result = tool.execute(nlohmann::json::object());
    CHECK(result.is_error);
    CHECK(result.content == "出错了");
    CHECK(g_free_calls.load() == 1);
}

TEST_CASE("PluginTool: 插件给的 schema 不是合法 JSON 时退化成宽对象 schema") {
    luban_tool_def def{};
    def.name = "t";
    def.input_schema_json = "{这不是 JSON";
    def.execute = FakeExecute;
    def.free_result = FakeFree;
    tools::PluginTool tool("p", &def);
    CHECK(tool.input_schema() == nlohmann::json{{"type", "object"}});
}

// ---------------------------------------------------------------------------
// PluginHost:真库加载(plugins 单第 5 步起三平台;库由 tests/CMakeLists
// 按当前平台编好——Windows 出 hello_plugin.dll,Linux 出 libhello_plugin.so,
// macOS 出 libhello_plugin.dylib,bad_version 同理)
// ---------------------------------------------------------------------------

#ifdef LUBANCODE_TEST_PLUGIN_DIR

TEST_CASE("PluginHost: 真加载示例库,abi_tag 不合的跳过并出警告") {
    tools::PluginHost host;
    const auto warnings = host.LoadDirectory(LUBANCODE_TEST_PLUGIN_DIR);

    // bad_version_plugin 被跳过,警告里点名 + 提到 abi_tag。
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("bad_version_plugin") != std::string::npos);
    CHECK(warnings[0].find("abi_tag") != std::string::npos);

    // hello_plugin 挂上了(ABI v2)。stem 是文件名去扩展名(POSIX 带 lib
    // 前缀);v2 工具名前缀走 plugin_id,那是真正的身份。
    REQUIRE(host.plugins().size() == 1);
    CHECK(host.plugins()[0].stem.find("hello_plugin") != std::string::npos);
    CHECK_FALSE(host.plugins()[0].legacy_v1);
    CHECK(host.plugins()[0].abi_tag == LUBAN_PLUGIN_ABI_V2);
    CHECK(host.plugins()[0].plugin_id == "hello_plugin");
    CHECK(host.plugins()[0].plugin_version == "1.1.0");

    // ToolRuntime 给 main/sub 两张表各装 wrapper 时会扫两遍同一目录。模块
    // 只该载一份；坏版本那枚仍可各报一次警告。
    const auto repeat_warnings = host.LoadDirectory(LUBANCODE_TEST_PLUGIN_DIR);
    REQUIRE(repeat_warnings.size() == 1);
    CHECK(host.plugins().size() == 1);

    std::vector<std::string> wrap_warnings;
    auto wrapped = host.WrapTools(wrap_warnings);
    CHECK(wrap_warnings.empty());
    REQUIRE(wrapped.size() == 1);
    REQUIRE(wrapped[0].tools.size() == 1);

    tools::ToolRegistry registry;
    for (auto& tool : wrapped[0].tools) {
        registry.Register(std::move(tool));
    }
    tools::Tool* tool = registry.Find("plugin__hello_plugin__reverse_text");
    REQUIRE(tool != nullptr);
    CHECK(tool->needs_confirm());

    // 真跨 DLL 执行:ASCII 倒序 + 中文按字符倒序(UTF-8 安全)。
    auto result = tool->execute(nlohmann::json{{"text", "hello"}});
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "olleh");

    result = tool->execute(nlohmann::json{{"text", "你好ab"}});
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "ba好你");

    // 缺 text 字段报 is_error,不崩。
    result = tool->execute(nlohmann::json{{"other", 1}});
    CHECK(result.is_error);
}

TEST_CASE("PluginHost: 坏库(垃圾字节)打警告跳过,不崩") {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode_test_bad_lib";
    std::filesystem::create_directories(dir);
    {
        // 当前平台认什么扩展名就投什么:Windows .dll / Linux .so / macOS .dylib。
        std::ofstream out(dir / ("junk" + std::string(lubancode::platform::DynamicLibraryExtension())),
                          std::ios::binary);
        out << "这压根不是一个合法的动态库文件";
    }
    tools::PluginHost host;
    const auto warnings = host.LoadDirectory(dir);
    CHECK(host.plugins().empty());
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("junk") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

#endif  // LUBANCODE_TEST_PLUGIN_DIR

TEST_CASE("PluginHost: 目录不存在时静默返回,不算错") {
    tools::PluginHost host;
    // 注意路径写 ASCII——窄串路径经系统 ACP(GBK)转宽,UTF-8 中文会转炸。
    const auto warnings = host.LoadDirectory("Z:/no_such_dir_for_lubancode/plugins");
    CHECK(warnings.empty());
    CHECK(host.plugins().empty());
}

// ---------------------------------------------------------------------------
// LuaTool
// ---------------------------------------------------------------------------

TEST_CASE("LuaTool: 正常表——名字前缀、描述、schema(JSON 字符串)、执行") {
    const std::string script = R"lua(
        return {
            name = "greet",
            description = "打招呼",
            input_schema = '{"type":"object","properties":{"who":{"type":"string"}}}',
            execute = function(input)
                return "你好, " .. input.who
            end,
        }
    )lua";
    auto loaded = tools::LuaTool::LoadFromScript(script, "greeter");
    REQUIRE(loaded.has_value());
    auto& tool = **loaded;
    CHECK(tool.name() == "plugin__greeter__greet");
    CHECK(tool.description() == "[plugin:greeter] 打招呼");
    CHECK(tool.needs_confirm());
    CHECK(tool.input_schema()["properties"].contains("who"));

    const auto result = tool.execute(nlohmann::json{{"who", "世界"}});
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "你好, 世界");  // 中文进 lua 再出来,字节原样往返
}

TEST_CASE("LuaTool: input_schema 用 lua 表写也认(转成 JSON)") {
    const std::string script = R"lua(
        return {
            name = "t",
            input_schema = {
                type = "object",
                properties = { n = { type = "number" } },
                required = { "n" },
            },
            execute = function(input) return "x" end,
        }
    )lua";
    auto loaded = tools::LuaTool::LoadFromScript(script, "s");
    REQUIRE(loaded.has_value());
    const nlohmann::json schema = (*loaded)->input_schema();
    CHECK(schema["type"] == "object");
    CHECK(schema["properties"]["n"]["type"] == "number");
    CHECK(schema["required"] == nlohmann::json::array({"n"}));  // 连续整数键判成数组
}

TEST_CASE("LuaTool: 缺字段各有各的报错") {
    // 缺 name
    auto r1 = tools::LuaTool::LoadFromScript("return { execute = function() return 'x' end }", "s");
    REQUIRE_FALSE(r1.has_value());
    CHECK(r1.error().find("name") != std::string::npos);

    // 缺 execute
    auto r2 = tools::LuaTool::LoadFromScript("return { name = 't' }", "s");
    REQUIRE_FALSE(r2.has_value());
    CHECK(r2.error().find("execute") != std::string::npos);

    // 返回值不是表
    auto r3 = tools::LuaTool::LoadFromScript("return 42", "s");
    REQUIRE_FALSE(r3.has_value());
    CHECK(r3.error().find("不是表") != std::string::npos);

    // 语法错误
    auto r4 = tools::LuaTool::LoadFromScript("return {{{", "s");
    REQUIRE_FALSE(r4.has_value());
    CHECK(r4.error().find("编译失败") != std::string::npos);

    // input_schema 是坏 JSON 字符串
    auto r5 = tools::LuaTool::LoadFromScript(
        "return { name='t', input_schema='{oops', execute=function() return 'x' end }", "s");
    REQUIRE_FALSE(r5.has_value());
    CHECK(r5.error().find("input_schema") != std::string::npos);
}

TEST_CASE("LuaTool: execute 里 error() 被 pcall 接住,报 is_error 不崩") {
    auto loaded = tools::LuaTool::LoadFromScript(
        "return { name='boom', execute=function(input) error('炸了') end }", "s");
    REQUIRE(loaded.has_value());
    const auto result = (*loaded)->execute(nlohmann::json::object());
    CHECK(result.is_error);
    CHECK(result.content.find("炸了") != std::string::npos);
}

TEST_CASE("LuaTool: execute 忘了 return 报 is_error") {
    auto loaded =
        tools::LuaTool::LoadFromScript("return { name='t', execute=function(input) end }", "s");
    REQUIRE(loaded.has_value());
    const auto result = (*loaded)->execute(nlohmann::json::object());
    CHECK(result.is_error);
}

TEST_CASE("LuaTool: 入参 JSON 转 lua 表——嵌套对象/数组/整数/浮点/布尔都对得上") {
    // execute 把拿到的表原样 return,LuaTool 再把表转回 JSON——一来一回全等,
    // 就证明两个方向的转换都没丢东西(除了 null:lua 表里存不住 nil,豁免)。
    auto loaded = tools::LuaTool::LoadFromScript(
        "return { name='echo', execute=function(input) return input end }", "s");
    REQUIRE(loaded.has_value());

    const nlohmann::json input = {
        {"text", "中文往返测试"},
        {"count", 42},
        {"ratio", 0.5},
        {"flag", true},
        {"nested", {{"list", {1, 2, 3}}, {"names", {"甲", "乙"}}}},
    };
    const auto result = (*loaded)->execute(input);
    REQUIRE_FALSE(result.is_error);
    CHECK(nlohmann::json::parse(result.content) == input);
}

TEST_CASE("LuaTool: 返回数字/布尔也能字符串化") {
    auto loaded = tools::LuaTool::LoadFromScript(
        "return { name='n', execute=function(input) return input.n + 1 end }", "s");
    REQUIRE(loaded.has_value());
    const auto result = (*loaded)->execute(nlohmann::json{{"n", 41}});
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "42");
}

TEST_CASE("LoadLuaPlugins: 好文件挂上,坏文件警告跳过,目录不存在返回空") {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode_test_lua_plugins";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream good(dir / "good.lua", std::ios::binary);
        good << "return { name='ok', description='好的', execute=function(input) return 'fine' end }";
        std::ofstream bad(dir / "bad.lua", std::ios::binary);
        bad << "return {{{ 这是坏语法";
    }

    auto result = tools::LoadLuaPlugins(dir);
    REQUIRE(result.tools.size() == 1);
    CHECK(result.tools[0]->name() == "plugin__good__ok");
    CHECK(result.tools[0]->stem() == "good");
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("bad.lua") != std::string::npos);

    std::filesystem::remove_all(dir);

    const auto empty = tools::LoadLuaPlugins(dir / "missing");  // ASCII,理由同上(ACP 窄路径)
    CHECK(empty.tools.empty());
    CHECK(empty.warnings.empty());
}

TEST_CASE("LuaTool: 两个工具各自独立 lua_State,全局变量互不串门") {
    auto a = tools::LuaTool::LoadFromScript(
        "G = '甲'; return { name='a', execute=function(_) return G end }", "a");
    auto b = tools::LuaTool::LoadFromScript(
        "return { name='b', execute=function(_) return tostring(G) end }", "b");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK((*a)->execute(nlohmann::json::object()).content == "甲");
    CHECK((*b)->execute(nlohmann::json::object()).content == "nil");  // 隔离:看不到别人的 G
}

TEST_CASE("LuaTool: 同一 state 被多线程调用时串行,计数不重不漏") {
    auto loaded = tools::LuaTool::LoadFromScript(
        R"lua(
            local counter = 0
            return {
                name = 'counter',
                execute = function(_)
                    local before = counter
                    for i = 1, 20000 do local square = i * i end
                    counter = before + 1
                    return counter
                end,
            }
        )lua",
        "counter");
    REQUIRE(loaded.has_value());

    constexpr int kThreads = 8;
    std::vector<std::string> outputs(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&tool = **loaded, &outputs, i] {
            outputs[i] = tool.execute(nlohmann::json::object()).content;
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    const std::set<std::string> unique(outputs.begin(), outputs.end());
    CHECK(unique.size() == kThreads);
    for (int i = 1; i <= kThreads; ++i) {
        CHECK(unique.contains(std::to_string(i)));
    }
}
