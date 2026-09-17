// LuaHook 单 P1-D:官方示例包的合同测试——examples/hooks/ 下每只有
// hook.json 的包过一遍 validate+test(静态 + fixtures fake 档),全绿才
// 算样例可跑(§8.2 "文档样例与 fixture 共用源或互相校验")。另钉
// replace-estimator 的计划面:同名替换后内置退出执行计划(overridden)。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/hook_package_check.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

std::vector<std::filesystem::path> ExampleHookPackages() {
    std::vector<std::filesystem::path> packages;
    const std::filesystem::path root = std::filesystem::path(LUBANCODE_TEST_SOURCE_DIR) / "examples" / "hooks";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        std::error_code has_ec;
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "hook.json", has_ec)) {
            packages.push_back(entry.path());
        }
    }
    std::sort(packages.begin(), packages.end());
    return packages;
}

}  // namespace

TEST_CASE("官方 hook 示例:每包静态过 + fixtures fake 档全绿") {
    const auto packages = ExampleHookPackages();
    REQUIRE(packages.size() >= 6);
    for (const std::filesystem::path& package : packages) {
        CAPTURE(package.string());
        HookCheckOptions options;
        options.run_fixtures = true;
        options.fs_scratch_root = std::filesystem::temp_directory_path();
        const HookCheckReport report = RunHookPackageCheck(package, options);
        CHECK_MESSAGE(report.static_pass,
                      package.filename().string() + " 静态未过: " +
                          (report.checks.empty() ? "(无检查项)"
                                                 : report.checks.back().item + " " + report.checks.back().detail));
        CHECK_MESSAGE(report.fixtures_pass(), package.filename().string() + " fixtures 未全绿");
        for (const auto& fixture : report.fixtures) {
            CHECK_MESSAGE(fixture.ran && fixture.pass,
                          package.filename().string() + "/" + fixture.name + ": " + fixture.detail);
        }
        CHECK(report.exit_code() == 0);
    }
}

TEST_CASE("replace-estimator 示例:同名替换后内置退出执行计划") {
    const std::filesystem::path package =
        std::filesystem::path(LUBANCODE_TEST_SOURCE_DIR) / "examples" / "hooks" / "replace-estimator";
    HookCheckOptions options;
    options.run_fixtures = false;
    const HookCheckReport report = RunHookPackageCheck(package, options);
    REQUIRE(report.static_pass);
    REQUIRE(report.plan.contains("points"));
    // 获选项:用户 Lua(user 层),同键。
    bool lua_selected = false;
    for (const auto& def : report.plan.at("points").value("PreRequest", nlohmann::json::array())) {
        if (def.value("key", std::string()) == "PreRequest/context.token_estimate") {
            lua_selected = def.value("handlerKind", std::string()) == "lua" &&
                           def.value("source", std::string()) == "user";
        }
    }
    CHECK(lua_selected);
    // 内置被挤下:保留定义来源,不进执行计划。
    bool builtin_overridden = false;
    for (const auto& def : report.plan.value("overridden", nlohmann::json::array())) {
        if (def.value("key", std::string()) == "PreRequest/context.token_estimate" &&
            def.value("source", std::string()) == "builtin") {
            builtin_overridden = true;
        }
    }
    CHECK(builtin_overridden);
}
