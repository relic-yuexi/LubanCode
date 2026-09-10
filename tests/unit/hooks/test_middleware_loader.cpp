// hook 包目录发现(LuaHook 单 P0-B,§三):清单只读、entry 越界整包拒绝、
// 一包失败不连累邻包、来源层级由发现位置定(清单不自报)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "hooks/middleware.hpp"
#include "hooks/middleware_loader.hpp"
#include "runtime/plugin_lua_host.hpp"

using namespace lubancode;
using namespace lubancode::hooks::middleware;

namespace {

struct LoaderHarness {
    std::filesystem::path root;

    explicit LoaderHarness(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-hookload-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }

    std::filesystem::path MakePackage(const std::string& name, const std::string& hook_json,
                                      const std::string& entry_name = "main.lua",
                                      const std::string& script = "return {}") {
        const std::filesystem::path package = root / name;
        std::filesystem::create_directories(package);
        // 清单里 entry 字段由调用方自己写对/写错(测越界用)。
        {
            std::ofstream file(package / "hook.json", std::ios::binary);
            file << hook_json;
        }
        if (!entry_name.empty()) {
            std::ofstream file(package / entry_name, std::ios::binary);
            file << script;
        }
        return package;
    }

    std::string ManifestWithEntry(const std::string& entry) {
        return R"({
          "schemaVersion": 1,
          "id": "prompt-normalize",
          "version": "1.0.0",
          "entry": ")" + entry + R"(",
          "hooks": [
            {"hookPoint": "PreUser", "name": "prompt.normalize", "handler": "normalize"}
          ]
        })";
    }
};

}  // namespace

TEST_CASE("目录发现:有 hook.json 的包入池,没清单的目录跳过") {
    LoaderHarness harness("discover");
    harness.MakePackage("with-manifest", harness.ManifestWithEntry("main.lua"));
    std::filesystem::create_directories(harness.root / "just-fixtures");
    {
        std::ofstream file(harness.root / "just-fixtures" / "input.json", std::ios::binary);
        file << "{}";
    }

    MiddlewarePool pool;
    const auto report = LoadHookPackages(pool, harness.root, SourceLayer::User, "user test");
    CHECK(report.packages_loaded == 1);
    CHECK(report.errors.empty());
    CHECK(pool.candidate_count() == 1);
}

TEST_CASE("root 不存在:零包零错(没配 hooks 的常态)") {
    MiddlewarePool pool;
    const auto report = LoadHookPackages(pool, std::filesystem::temp_directory_path() / "no-such-hook-root-xyz",
                                         SourceLayer::User, "user test");
    CHECK(report.packages_loaded == 0);
    CHECK(report.errors.empty());
}

TEST_CASE("entry 越界与绝对路径:整包拒绝,错误带包名") {
    LoaderHarness harness("escape");
    harness.MakePackage("traversal", harness.ManifestWithEntry("../../outside.lua"));
    // is_absolute() 两平台语义不同:Windows 认盘符根(C:/x),POSIX 只认
    // 前导斜杠(/x)——各喂各平台的绝对路径样本,C:/x 在 POSIX 只是相对
    // 路径,断言就翻车(linux/macos CI 红过)。
#if defined(_WIN32)
    harness.MakePackage("absolute", harness.ManifestWithEntry("C:/windows/system32/evil.lua"));
#else
    harness.MakePackage("absolute", harness.ManifestWithEntry("/etc/evil.lua"));
#endif
    // 邻包照装:一包失败不连累别家。
    harness.MakePackage("healthy", harness.ManifestWithEntry("main.lua"));

    MiddlewarePool pool;
    const auto report = LoadHookPackages(pool, harness.root, SourceLayer::Project, "project test");
    CHECK(report.packages_loaded == 1);
    REQUIRE(report.errors.size() == 2);
    // 目录名稳定序:absolute < traversal。
    CHECK(report.errors[0].find("[absolute]") != std::string::npos);
    CHECK(report.errors[0].find("绝对路径") != std::string::npos);
    CHECK(report.errors[1].find("[traversal]") != std::string::npos);
    CHECK(report.errors[1].find("越出包根") != std::string::npos);
}

TEST_CASE("清单解析失败:整包拒绝,不带半个定义进池") {
    LoaderHarness harness("badmanifest");
    {
        const std::filesystem::path package = harness.root / "broken";
        std::filesystem::create_directories(package);
        std::ofstream file(package / "hook.json", std::ios::binary);
        file << "{ not valid json";
    }
    harness.MakePackage("bad-point", R"({
        "schemaVersion": 1, "id": "x", "entry": "main.lua",
        "hooks": [{"hookPoint": "NopePoint", "name": "x", "handler": "run"}]
    })");

    MiddlewarePool pool;
    const auto report = LoadHookPackages(pool, harness.root, SourceLayer::User, "user test");
    CHECK(report.packages_loaded == 0);
    CHECK(report.errors.size() == 2);
    CHECK(pool.candidate_count() == 0);
}

TEST_CASE("单包直装:清单 + 脚本入池,来源层级按发现位置") {
    LoaderHarness harness("single");
    const auto package = harness.MakePackage("pkg", harness.ManifestWithEntry("main.lua"),
                                             "main.lua",
                                             "return { normalize = function(ctx, input, next)\n"
                                             "  return next(input)\n"
                                             "end }");
    MiddlewarePool::Options options;
    options.lua_factory = [](const LuaHandlerSpec& spec, const HandlerLimits& limits) {
        return lubancode::runtime::MakeLuaHookHandler(spec, limits);
    };
    MiddlewarePool pool(std::move(options));
    auto loaded = LoadHookPackage(pool, package, SourceLayer::Project, "project test");
    REQUIRE(loaded.has_value());
    CHECK(pool.candidate_count() == 1);
    // 候选定义的层级/实现引用按清单形状落定;发布把 lua 声明物化成 Handler。
    auto published = pool.Publish();
    REQUIRE(published.has_value());
    const auto& selected = (*published)->Selected(HookPoint::PreUser);
    REQUIRE(selected.size() == 1);
    CHECK(selected[0]->layer == SourceLayer::Project);
    CHECK(selected[0]->is_lua);
    CHECK(selected[0]->implementation_ref.find("hooks/prompt-normalize#normalize") != std::string::npos);
}
