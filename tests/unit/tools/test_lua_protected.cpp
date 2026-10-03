#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include "tools/lua_tool.hpp"

namespace {
using lubancode::tools::LuaProfile;
using lubancode::tools::LuaTool;
using Json = nlohmann::json;

LuaProfile Profile(std::size_t memory = 128 * 1024) {
    auto profile = LuaProfile::HookDefault();
    profile.memory_cap_bytes = memory;
    profile.instruction_budget = 2'000'000;
    profile.wall_budget = std::chrono::milliseconds(100);
    profile.allow_print = false;
    return profile;
}

const std::string healthy =
    "local calls=0; return {name='probe',input_schema={type='object'},"
    "execute=function(input) calls=calls+1; return tostring(calls)..':'..input.text end}";
}

TEST_CASE("Lua protected: state and library allocation failures return before host termination") {
    const auto tiny = LuaTool::LoadFromScript(healthy, "tiny", Profile(1));
    REQUIRE_FALSE(tiny.has_value());
    CHECK(tiny.error().find("lua_newstate") != std::string::npos);

    bool saw_library_oom = false;
    for (std::size_t bytes : {std::size_t{8192}, std::size_t{16384}, std::size_t{32768}}) {
        const auto loaded = LuaTool::LoadFromScript(healthy, "small", Profile(bytes));
        if (!loaded && loaded.error().find("脚本开库失败") != std::string::npos) {
            CHECK(loaded.error().find("not enough memory") != std::string::npos);
            saw_library_oom = true;
        }
    }
    CHECK(saw_library_oom);
    auto next = LuaTool::LoadFromScript(healthy, "next", Profile());
    REQUIRE_MESSAGE(next.has_value(), (next ? std::string{} : next.error()));
    const auto result = (*next)->execute(Json{{"text", "alive"}});
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "1:alive");
}

TEST_CASE("Lua protected: oversized input conversion fails then the same VM executes cleanly") {
    auto loaded = LuaTool::LoadFromScript(healthy, "input", Profile());
    REQUIRE_MESSAGE(loaded.has_value(), (loaded ? std::string{} : loaded.error()));
    const auto failed = (*loaded)->execute(Json{{"text", std::string(1024 * 1024, 'x')}});
    CHECK(failed.is_error);
    CHECK(failed.error_code == "plugin.lua_error");
    CHECK(failed.content.find("not enough memory") != std::string::npos);
    const auto next = (*loaded)->execute(Json{{"text", "alive"}});
    CHECK_FALSE(next.is_error);
    CHECK(next.content == "1:alive");  // input never reached execute.
    const auto again = (*loaded)->execute(Json{{"text", "again"}});
    CHECK_FALSE(again.is_error);
    CHECK(again.content == "2:again");
}

TEST_CASE("Lua protected: selected whitelist opens its real libraries and retains old pure trusted sets") {
    const std::string script =
        "return {name='libs',execute=function(input) "
        "return type(require)..':'..type(io)..':'..type(dofile)..':'..type(loadfile)..':'.."
        "type(os.execute)..':'..type(os.remove)..':'..type(coroutine)..':'..type(debug)..':'.."
        "type(string.rep)..':'..type(table.concat)..':'..type(math.floor)..':'.."
        "type(utf8.len)..':'..type(load)..':'..type(os.time)..':'..type(print) end}";
    auto loaded = LuaTool::LoadFromScript(script, "whitelist", Profile());
    REQUIRE_MESSAGE(loaded.has_value(), (loaded ? std::string{} : loaded.error()));
    const auto result = (*loaded)->execute(Json::object());
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "nil:nil:nil:nil:nil:nil:nil:nil:function:function:function:function:function:function:nil");
    auto old_hook = LuaTool::LoadFromScript(script, "hook", LuaProfile::HookDefault());
    REQUIRE(old_hook.has_value());
    CHECK((*old_hook)->execute(Json::object()).content ==
        "nil:nil:nil:nil:nil:nil:nil:nil:function:function:function:function:function:function:function");
    const std::string probe = "return {name='old',execute=function(input) return type(io)..':'..type(os.execute)..':'..type(print) end}";
    auto pure = LuaTool::LoadFromScript(probe, "pure", LuaProfile::PureDefault());
    auto trusted = LuaTool::LoadFromScript(probe, "trusted", LuaProfile::TrustedDefault());
    REQUIRE(pure.has_value());
    REQUIRE(trusted.has_value());
    CHECK((*pure)->execute(Json::object()).content == "nil:nil:function");
    CHECK((*trusted)->execute(Json::object()).content == "table:function:function");
}

TEST_CASE("Lua protected: top level instructions and wall time share the guarded initialization") {
    auto instruction = Profile();
    instruction.wall_budget = std::chrono::milliseconds(5000);
    const auto first = LuaTool::LoadFromScript("while true do end", "instructions", instruction);
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().find("cpu 指令预算耗尽") != std::string::npos);

    auto wall = Profile();
    wall.instruction_budget = 2'000'000'000;
    wall.wall_budget = std::chrono::milliseconds(5);
    const auto start = std::chrono::steady_clock::now();
    const auto second = LuaTool::LoadFromScript("while true do end", "wall", wall);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().find("墙钟预算耗尽") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    const auto next = LuaTool::LoadFromScript(healthy, "healthy", Profile());
    REQUIRE(next.has_value());
    CHECK((*next)->execute(Json{{"text", "alive"}}).content == "1:alive");
}

TEST_CASE("Lua protected: each call clears wall and cancel borrowing before another call") {
    const std::string script =
        "return {name='wall',execute=function(input) if input.loop then while true do end end return 'alive' end}";
    auto profile = Profile();
    profile.instruction_budget = 2'000'000'000;
    profile.wall_budget = std::chrono::milliseconds(5);
    auto loaded = LuaTool::LoadFromScript(script, "wall", profile);
    REQUIRE_MESSAGE(loaded.has_value(), (loaded ? std::string{} : loaded.error()));
    const auto timed = (*loaded)->execute(Json{{"loop", true}});
    CHECK(timed.is_error);
    CHECK(timed.content.find("墙钟预算耗尽") != std::string::npos);
    {
        std::atomic<bool> cancel{true};
        lubancode::tools::ToolExecutionContext context;
        context.cancel = &cancel;
        const auto stopped = (*loaded)->execute(Json::object(), context);
        CHECK(stopped.is_error);
        CHECK(stopped.content.find("用户取消") != std::string::npos);
    }
    const auto next = (*loaded)->execute(Json::object());
    CHECK_FALSE(next.is_error);
    CHECK(next.content == "alive");

    // 真运行取消必须读到 Hook 的「已终止」，不能把 pre-cancel 当此案。
    auto running_profile = Profile();
    running_profile.instruction_budget = 2'000'000'000;
    running_profile.wall_budget = std::chrono::milliseconds(1000);
    auto running = LuaTool::LoadFromScript(script, "cancel", running_profile);
    REQUIRE(running.has_value());
    {
        std::atomic<bool> cancel{false};
        std::jthread canceller([&cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            cancel.store(true);
        });
        lubancode::tools::ToolExecutionContext context;
        context.cancel = &cancel;
        const auto stopped = (*running)->execute(Json{{"loop", true}}, context);
        CHECK(stopped.is_error);
        CHECK(stopped.content.find("用户取消") != std::string::npos);
        CHECK(stopped.content.find("脚本已终止") != std::string::npos);
        CHECK(stopped.content.find("墙钟预算耗尽") == std::string::npos);
    }
    const auto after_cancel = (*running)->execute(Json::object());
    CHECK_FALSE(after_cancel.is_error);
    CHECK(after_cancel.content == "alive");
}

TEST_CASE("Lua protected: definition metamethod errors and rich JSON conversion preserve ordinary results") {
    const auto invalid = LuaTool::LoadFromScript(
        "return setmetatable({}, {__index=function() error('definition fault') end})", "bad", Profile());
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().find("definition fault") != std::string::npos);

    const std::string script =
        "return {name='types',description='same converter',input_schema={type='object'},"
        "execute=function(input) return {text=input.text,negative=input.negative,fraction=input.fraction,"
        "truth=input.truth,array=input.array,nested=input.nested} end}";
    auto loaded = LuaTool::LoadFromScript(script, "types", Profile());
    REQUIRE_MESSAGE(loaded.has_value(), (loaded ? std::string{} : loaded.error()));
    CHECK((*loaded)->name() == "plugin__types__types");
    CHECK((*loaded)->description() == "[plugin:types] same converter");
    CHECK((*loaded)->input_schema().at("type").get<std::string>() == "object");
    const Json input{{"text", "UTF-8 雪"}, {"negative", -17}, {"fraction", 1.25}, {"truth", true},
                     {"array", Json::array({1, "two", false})}, {"nested", Json{{"key", "value"}}}};
    const auto result = (*loaded)->execute(input);
    CHECK_FALSE(result.is_error);
    const bool matches_input = Json::parse(result.content) == input;
    CHECK(matches_input);

    const std::string recovery_script =
        "return {name='recovery',execute=function(input) "
        "if input.mode=='cycle' then local t={} t.self=t return t end "
        "if input.mode=='deep' then local t={} local p=t for i=1,70 do p.child={} p=p.child end return t end "
        "return 'alive' end}";
    auto recovery = LuaTool::LoadFromScript(recovery_script, "recovery", Profile());
    REQUIRE(recovery.has_value());
    for (const auto& mode : {"cycle", "deep"}) {
        const auto failed = (*recovery)->execute(Json{{"mode", mode}});
        CHECK(failed.is_error);
        CHECK(failed.content.find("超过 64 层") != std::string::npos);
        const auto alive = (*recovery)->execute(Json::object());
        CHECK_FALSE(alive.is_error);
        CHECK(alive.content == "alive");
    }
}
