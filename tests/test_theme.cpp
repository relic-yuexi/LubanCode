// cli::Theme:三套内置色板(dark/light/plain)要能选对、要能区分开;
// ComputeColorsEnabled 是纯函数,把"是不是真控制台 / VT 处理开没开 /
// 强制着色"这三个布尔值组合的真值表测全;ResolveTheme 在 enable_colors=false
// 时不管传的主题名是什么,一律退化成 plain(空串)。
// 全部走纯函数,不碰真实控制台句柄(DetectConsoleCapability 本身依赖真实
// 控制台状态,这里只测它总能返回、不崩,不断言具体值——CI/测试环境跑在
// 什么样的控制台/管道下不可控)。

#include <doctest/doctest.h>

#include "cli/theme.hpp"

using namespace lubancode::cli;

TEST_CASE("BuiltinTheme: dark 是默认主题,着色字段非空") {
    const Theme t = BuiltinTheme("dark");
    CHECK_FALSE(t.banner.empty());
    CHECK_FALSE(t.prompt.empty());
    CHECK_FALSE(t.tool_line.empty());
    CHECK_FALSE(t.confirm.empty());
    CHECK_FALSE(t.error.empty());
    CHECK_FALSE(t.stats.empty());
    CHECK_FALSE(t.spinner.empty());
    CHECK_FALSE(t.reset.empty());
}

TEST_CASE("BuiltinTheme: light 主题着色字段非空,且跟 dark 不完全一样") {
    const Theme dark = BuiltinTheme("dark");
    const Theme light = BuiltinTheme("light");
    CHECK_FALSE(light.banner.empty());
    CHECK_FALSE(light.prompt.empty());
    // 两套主题不能是同一份配色(不然分两套毫无意义)。
    CHECK((dark.banner != light.banner || dark.prompt != light.prompt || dark.stats != light.stats));
}

TEST_CASE("BuiltinTheme: plain 主题所有字段都是空串") {
    const Theme t = BuiltinTheme("plain");
    CHECK(t.banner.empty());
    CHECK(t.prompt.empty());
    CHECK(t.tool_line.empty());
    CHECK(t.confirm.empty());
    CHECK(t.error.empty());
    CHECK(t.stats.empty());
    CHECK(t.spinner.empty());
    CHECK(t.reset.empty());
}

TEST_CASE("BuiltinTheme: 不认得的名字兜底成 dark") {
    const Theme t = BuiltinTheme("这个主题名字肯定不存在");
    const Theme dark = BuiltinTheme("dark");
    CHECK(t.banner == dark.banner);
    CHECK(t.prompt == dark.prompt);
}

TEST_CASE("ComputeColorsEnabled: 真值表") {
    // force_color 为真,不管别的条件,一律开。
    CHECK(ComputeColorsEnabled(/*is_console=*/false, /*vt_ok=*/false, /*force=*/true));
    CHECK(ComputeColorsEnabled(/*is_console=*/false, /*vt_ok=*/true, /*force=*/true));
    CHECK(ComputeColorsEnabled(/*is_console=*/true, /*vt_ok=*/false, /*force=*/true));

    // force_color 为假:必须真控制台 + VT 处理都成立才开。
    CHECK(ComputeColorsEnabled(/*is_console=*/true, /*vt_ok=*/true, /*force=*/false));
    CHECK_FALSE(ComputeColorsEnabled(/*is_console=*/true, /*vt_ok=*/false, /*force=*/false));
    CHECK_FALSE(ComputeColorsEnabled(/*is_console=*/false, /*vt_ok=*/true, /*force=*/false));
    CHECK_FALSE(ComputeColorsEnabled(/*is_console=*/false, /*vt_ok=*/false, /*force=*/false));
}

TEST_CASE("ResolveTheme: enable_colors=false 时不管主题名是什么,一律退化成 plain") {
    const Theme t1 = ResolveTheme("dark", /*enable_colors=*/false);
    const Theme t2 = ResolveTheme("light", /*enable_colors=*/false);
    CHECK(t1.banner.empty());
    CHECK(t1.prompt.empty());
    CHECK(t2.banner.empty());
    CHECK(t2.prompt.empty());
}

TEST_CASE("ResolveTheme: enable_colors=true 时按名字选对应主题") {
    const Theme dark = ResolveTheme("dark", /*enable_colors=*/true);
    const Theme light = ResolveTheme("light", /*enable_colors=*/true);
    CHECK(dark.banner == BuiltinTheme("dark").banner);
    CHECK(light.banner == BuiltinTheme("light").banner);
}

TEST_CASE("DetectConsoleCapability: 能正常调用返回,不崩") {
    const ConsoleCapability cap = DetectConsoleCapability();
    // 测试跑在什么样的宿主(真终端/管道/CI)下不可控,这里只验证调用安全、
    // 字段能读到,不断言具体的 true/false。
    CHECK((cap.is_console == true || cap.is_console == false));
    CHECK((cap.colors_enabled == true || cap.colors_enabled == false));
}
