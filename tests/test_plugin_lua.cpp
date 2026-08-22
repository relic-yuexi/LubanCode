// plugins 单第 4 步(收现有 Lua)的测试:profile 分级、指令预算、内存帽、
// 取消链、EmbeddedLuaRuntime 的台账与幂等。legacy 用法零变化的部分
// (脚本格式/工具名/扫描排序)在 test_plugins.cpp 的 LuaTool 段,这里不
// 重复,只钉新墙。
//
// 注意预算与帽都给得很小:单测要的是"墙真的会落",不是"数值合理"。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "runtime/plugin_lua.hpp"
#include "tools/lua_tool.hpp"

using namespace lubancode;
using namespace lubancode::runtime;
using namespace lubancode::tools;

namespace {

std::string RunScript(const std::string& body, const LuaProfile& profile) {
    const std::string script = "return { name='t', execute=function(input) " + body + " end }";
    auto loaded = LuaTool::LoadFromScript(script, "probe", profile);
    if (!loaded.has_value()) {
        return "LOAD-ERROR: " + loaded.error();
    }
    return (*loaded)->execute(nlohmann::json::object()).content;
}

}  // namespace

// ---------------------------------------------------------------------------
// pure 画像:门关了;trusted 画像:门开着
// ---------------------------------------------------------------------------

TEST_CASE("pure 缺省:os.execute/os.exit/io/package.loadlib 都拿不到") {
    CHECK(RunScript("return type(os.execute)", LuaProfile::PureDefault()) == "nil");
    CHECK(RunScript("return type(os.exit)", LuaProfile::PureDefault()) == "nil");
    CHECK(RunScript("return type(io)", LuaProfile::PureDefault()) == "nil");
    CHECK(RunScript("return type(package.loadlib)", LuaProfile::PureDefault()) == "nil");
    // 没关的照常用:string/table/math/os.clock 都在。
    CHECK(RunScript("return type(string.gmatch)", LuaProfile::PureDefault()) == "function");
    CHECK(RunScript("return type(os.clock)", LuaProfile::PureDefault()) == "function");
    CHECK(RunScript("return type(os.time)", LuaProfile::PureDefault()) == "function");
}

TEST_CASE("trusted 全开:io/os.execute 照旧(legacy 行为)") {
    CHECK(RunScript("return type(io.open)", LuaProfile::TrustedDefault()) == "function");
    CHECK(RunScript("return type(os.execute)", LuaProfile::TrustedDefault()) == "function");
}

TEST_CASE("pure 里用 io 报错不崩,报 is_error") {
    const std::string script =
        "return { name='boom', execute=function(input) local f = io.open('x') return 'no' end }";
    auto loaded = LuaTool::LoadFromScript(script, "p", LuaProfile::PureDefault());
    REQUIRE(loaded.has_value());
    const auto result = (*loaded)->execute(nlohmann::json::object());
    CHECK(result.is_error);
    CHECK(result.content.find("attempt to index") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 指令预算:死循环落锤
// ---------------------------------------------------------------------------

TEST_CASE("指令预算:死循环在预算内被 luaL_error 掐断,宿主不吊死") {
    LuaProfile profile = LuaProfile::PureDefault();
    profile.instruction_budget = 2'000'000;  // 2M 条 ≈ 几十毫秒
    const auto started = std::chrono::steady_clock::now();
    const std::string out = RunScript("while true do end return 'x'", profile);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
            .count();
    CHECK(out.find("cpu 指令预算耗尽") != std::string::npos);
    CHECK(elapsed < 5000);  // 有界落锤(正常在几十 ms;CI 慢机器给宽限)
}

TEST_CASE("预算 0 = 不设:循环照跑(由调用方自己把关)") {
    LuaProfile profile = LuaProfile::PureDefault();
    profile.instruction_budget = 0;
    // 小循环跑完就退,证明 hook 没装也不会坏。
    CHECK(RunScript("local n = 0 for i = 1, 10000 do n = n + 1 end return tostring(n)", profile) == "10000");
}

// ---------------------------------------------------------------------------
// 内存帽:吃内存的脚本按 OOM 报错,宿主堆不破
// ---------------------------------------------------------------------------

TEST_CASE("内存帽:狂吃内存的脚本被 allocator 掐断") {
    LuaProfile profile = LuaProfile::PureDefault();
    profile.memory_cap_bytes = 8 * 1024 * 1024;  // 8MB
    const std::string out = RunScript(
        "local chunks = {} for i = 1, 1000 do chunks[i] = string.rep('x', 1024 * 1024) end return 'ate it'",
        profile);
    CHECK(out.find("not enough memory") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 取消链:ESC 旗子置位后 hook 里掐断
// ---------------------------------------------------------------------------

TEST_CASE("取消链:cancel 置位后死循环被掐断,分型是取消不是预算") {
    LuaProfile profile = LuaProfile::PureDefault();
    profile.instruction_budget = 500'000'000;  // 预算放很大,先到的得是取消
    const std::string script = "return { name='t', execute=function(input) while true do end end }";
    auto loaded = LuaTool::LoadFromScript(script, "cancel_probe", profile);
    REQUIRE(loaded.has_value());

    std::atomic<bool> cancel{false};
    (*loaded)->SetCancel(&cancel);
    std::thread canceller([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        cancel.store(true);
    });
    const auto started = std::chrono::steady_clock::now();
    const auto result = (*loaded)->execute(nlohmann::json::object());
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
            .count();
    canceller.join();
    CHECK(result.is_error);
    CHECK(result.content.find("用户取消") != std::string::npos);
    CHECK(elapsed < 5000);
}

// ---------------------------------------------------------------------------
// EmbeddedLuaRuntime:台账、幂等、adapter 转发
// ---------------------------------------------------------------------------

TEST_CASE("EmbeddedLuaRuntime:扫描一次、台账对账、MakeAdapters 可用") {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode_test_lua_runtime";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    {
        std::ofstream good(dir / "alpha.lua", std::ios::binary);
        good << "return { name='one', execute=function(input) return 'A' end }";
        std::ofstream bad(dir / "zbad.lua", std::ios::binary);
        bad << "return {{{";
    }

    EmbeddedLuaRuntime runtime;
    const auto warnings = runtime.LoadDirectory(dir);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("zbad.lua") != std::string::npos);
    REQUIRE(runtime.records().size() == 1);
    CHECK(runtime.records()[0].id == "alpha");
    CHECK(runtime.records()[0].tool_name == "plugin__alpha__one");

    // 幂等:第二遍不重扫、不翻倍。
    const auto again = runtime.LoadDirectory(dir);
    CHECK(again.empty());
    CHECK(runtime.records().size() == 1);

    // adapter 转发执行。
    auto adapters = runtime.MakeAdapters();
    REQUIRE(adapters.size() == 1);
    CHECK(adapters[0]->name() == "plugin__alpha__one");
    CHECK(adapters[0]->needs_confirm());
    const auto result = adapters[0]->execute(nlohmann::json::object());
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "A");

    std::filesystem::remove_all(dir, ec);
}
