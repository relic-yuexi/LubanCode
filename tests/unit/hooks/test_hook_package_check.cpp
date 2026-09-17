// LuaHook 单 P1-D(§8.2):hook 包校验与试跑引擎(hook validate/test 的芯)。
//   静态档:清单 schema/entry 越界/Lua 语法+handler 对账/依赖计划/能力词表;
//   fake 档:真实 Lua runtime 跑 fixtures(fake HTTP/工具),断言候选、效果、
//   next 次数、结局与错码;约束注入(取消/重复 next)证明限制生效;
//   报告四档分账与退出码(0 全过/1 有 fail/2 包读不到)。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/hook_package_check.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const char* tag) {
        path = std::filesystem::temp_directory_path() / ("lubancode-hook-check-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file << content;
    REQUIRE_FALSE(file.bad());
}

const char* kCleanManifest = R"json({
  "schemaVersion": 1,
  "id": "prompt-clean",
  "version": "0.1.0",
  "entry": "main.lua",
  "hooks": [
    {"hookPoint": "PreUser", "name": "prompt.normalize", "handler": "normalize",
     "priority": 100, "match": {"origin": "human", "purpose": "interactive"}}
  ]
})json";

const char* kCleanLua = R"lua(
local function trim(s)
    return (s:gsub("^%s*(.-)%s*$", "%1"))
end
return {
  normalize = function(ctx, input, next)
    if type(input.prompt) ~= "string" then
      return next(input)
    end
    local candidate = {}
    for key, value in pairs(input) do
      candidate[key] = value
    end
    candidate.prompt = trim(input.prompt)
    return next(candidate)
  end,
}
)lua";

HookCheckReport Check(const std::filesystem::path& dir, bool run_fixtures = true) {
    HookCheckOptions options;
    options.run_fixtures = run_fixtures;
    options.fs_scratch_root = dir / "scratch";
    return RunHookPackageCheck(dir, options);
}

const HookCheckReport::Check* FindCheck(const HookCheckReport& report, const std::string& item) {
    for (const auto& check : report.checks) {
        if (check.item == item) {
            return &check;
        }
    }
    return nullptr;
}

const HookCheckReport::Fixture* FindFixture(const HookCheckReport& report, const std::string& name) {
    for (const auto& fixture : report.fixtures) {
        if (fixture.name == name) {
            return &fixture;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("静态档:好包四项全过,计划可读,退出码 0") {
    TempDir dir("static-good");
    WriteFile(dir.path / "hook.json", kCleanManifest);
    WriteFile(dir.path / "main.lua", kCleanLua);
    const HookCheckReport report = Check(dir.path, /*run_fixtures=*/false);

    REQUIRE(report.static_pass);
    REQUIRE(report.checks.size() == 4);
    CHECK(FindCheck(report, "manifest")->pass);
    CHECK(FindCheck(report, "lua")->pass);
    CHECK(FindCheck(report, "capabilities")->pass);
    CHECK(FindCheck(report, "plan")->pass);
    CHECK(report.plan.is_object());
    CHECK(report.plan.contains("registryRevision"));
    CHECK(report.exit_code() == 0);
    // 不跑 fixtures 时未验项明列。
    bool saw_real = false;
    for (const std::string& item : report.unverified) {
        if (item.rfind("真实集成", 0) == 0) saw_real = true;
    }
    CHECK(saw_real);
}

TEST_CASE("静态档:坏形状逐项拒——不是 hook 包/坏 JSON/坏 schema/语法错/缺 handler") {
    // 连目录都没有:退 2,不炸。
    {
        const HookCheckReport report = Check(std::filesystem::temp_directory_path() / "no-such-hook-pkg");
        CHECK(report.exit_code() == 2);
        CHECK_FALSE(report.static_pass);
    }
    TempDir dir("static-bad");
    // hook.json 不是 JSON。
    WriteFile(dir.path / "hook.json", "{ 不是 JSON");
    {
        const HookCheckReport report = Check(dir.path, false);
        CHECK(report.exit_code() == 2);
    }
    // schema 不合:schemaVersion 2。
    WriteFile(dir.path / "hook.json", nlohmann::json{{"schemaVersion", 2}, {"id", "x"}, {"entry", "main.lua"},
                                                     {"hooks", nlohmann::json::array()}}
                                       .dump());
    WriteFile(dir.path / "main.lua", kCleanLua);
    {
        const HookCheckReport report = Check(dir.path, false);
        CHECK_FALSE(report.static_pass);
        CHECK(report.exit_code() == 1);
        CHECK_FALSE(FindCheck(report, "manifest")->pass);
        // 静态挂了,后面的检查不跑(不给半份结论)。
        CHECK(FindCheck(report, "lua") == nullptr);
    }
    // 语法错。
    WriteFile(dir.path / "hook.json", kCleanManifest);
    WriteFile(dir.path / "main.lua", "return { normalize = function( --- 缺尾");
    {
        const HookCheckReport report = Check(dir.path, false);
        CHECK_FALSE(FindCheck(report, "lua")->pass);
        CHECK(report.exit_code() == 1);
    }
    // handler 对账失败:清单要 normalize,脚本只给别的名字。
    WriteFile(dir.path / "main.lua", "return { other = function(ctx, input, next) return next(input) end }");
    {
        const HookCheckReport report = Check(dir.path, false);
        CHECK_FALSE(FindCheck(report, "lua")->pass);
        CHECK(report.exit_code() == 1);
    }
}

TEST_CASE("静态档:能力词表与依赖计划——拼错的 capability/缺依赖都拒") {
    TempDir dir("static-plan");
    // 拼错的能力申请。
    {
        const nlohmann::json manifest = nlohmann::json::parse(kCleanManifest);
        manifest["hooks"][0]["capabilities"] = nlohmann::json::array({"htp"});
        WriteFile(dir.path / "hook.json", manifest.dump());
        WriteFile(dir.path / "main.lua", kCleanLua);
        const HookCheckReport report = Check(dir.path, false);
        CHECK_FALSE(FindCheck(report, "capabilities")->pass);
        CHECK(report.exit_code() == 1);
    }
    // 缺依赖:after 指向不存在的逻辑键。
    {
        const nlohmann::json manifest = nlohmann::json::parse(kCleanManifest);
        manifest["hooks"][0]["after"] = nlohmann::json::array({"PreUser/ghost.step"});
        WriteFile(dir.path / "hook.json", manifest.dump());
        const HookCheckReport report = Check(dir.path, false);
        CHECK_FALSE(FindCheck(report, "plan")->pass);
        CHECK(report.exit_code() == 1);
    }
    // entry 越界。
    {
        const nlohmann::json manifest = nlohmann::json::parse(kCleanManifest);
        manifest["entry"] = "../escape.lua";
        WriteFile(dir.path / "hook.json", manifest.dump());
        const HookCheckReport report = Check(dir.path, false);
        CHECK_FALSE(FindCheck(report, "manifest")->pass);
        CHECK(report.exit_code() == 1);  // 清单在但形状不合规:静态 fail,不是包读不到
    }
}

TEST_CASE("fake 档:改写/拒绝/取消/重复 next 四路 fixture,断言与退出码") {
    TempDir dir("fixtures");
    WriteFile(dir.path / "hook.json", kCleanManifest);
    WriteFile(dir.path / "main.lua", kCleanLua);

    // 1) 改写:首尾空白被洗,链尾恰一次。
    WriteFile(dir.path / "fixtures" / "clean.json", R"json({
      "name": "clean",
      "trigger": {"input": {"prompt": "  你好  "}, "origin": "human", "purpose": "interactive"},
      "expect": {"kind": "completed", "prompt": "你好", "terminalRuns": 1,
                 "records": {"PreUser/prompt.normalize": {"outcome": "completed", "nextCalls": 1}}}
    })json");
    {
        const HookCheckReport report = Check(dir.path);
        REQUIRE(report.static_pass);
        REQUIRE(report.fixtures.size() == 1);
        CHECK(report.fixtures[0].ran);
        CHECK(report.fixtures[0].pass);
        CHECK(report.exit_code() == 0);
    }

    // 2) 匹配未命中:purpose 对不上 -> skipped_no_match,不改写。
    WriteFile(dir.path / "fixtures" / "nomatch.json", R"json({
      "name": "nomatch",
      "trigger": {"input": {"prompt": "  旁问  "}, "purpose": "btw"},
      "expect": {"kind": "completed", "prompt": "  旁问  ",
                 "records": {"PreUser/prompt.normalize": {"outcome": "skipped_no_match"}}}
    })json");
    {
        const HookCheckReport report = Check(dir.path);
        REQUIRE(FindFixture(report, "nomatch") != nullptr);
        CHECK(FindFixture(report, "nomatch")->pass);
    }

    // 3) 取消注入:预置取消旗 -> dispatch 整体取消,改写不采用。
    WriteFile(dir.path / "fixtures" / "cancel.json", R"json({
      "name": "cancel",
      "trigger": {"input": {"prompt": "  被打断的  "}, "origin": "human", "purpose": "interactive"},
      "cancel": true,
      "expect": {"kind": "failed", "errorCode": "hook.dispatch.cancelled",
                 "records": {"PreUser/prompt.normalize": {"outcome": "skipped_cancelled"}}}
    })json");
    {
        const HookCheckReport report = Check(dir.path);
        REQUIRE(FindFixture(report, "cancel") != nullptr);
        CHECK(FindFixture(report, "cancel")->pass);
    }

    // 4) 断言挂了要报得出:期望值故意写错,detail 给差异,退出码 1。
    WriteFile(dir.path / "fixtures" / "wrong.json", R"json({
      "name": "wrong",
      "trigger": {"input": {"prompt": "  你好  "}, "origin": "human", "purpose": "interactive"},
      "expect": {"prompt": "不该是这个值"}
    })json");
    {
        const HookCheckReport report = Check(dir.path);
        const HookCheckReport::Fixture* fixture = FindFixture(report, "wrong");
        REQUIRE(fixture != nullptr);
        CHECK(fixture->ran);
        CHECK_FALSE(fixture->pass);
        CHECK(fixture->detail.find("prompt") != std::string::npos);
        CHECK(report.exit_code() == 1);
    }
}

TEST_CASE("fake 档:重复 next 的约束由 fixture 真跑出来(hook.next.already_consumed)") {
    TempDir dir("double-next");
    WriteFile(dir.path / "hook.json", kCleanManifest);
    WriteFile(dir.path / "main.lua", R"lua(
return {
  normalize = function(ctx, input, next)
    local first = next(input)
    next(input)  -- 协议违规:第二次 next
    return first
  end,
}
)lua");
    WriteFile(dir.path / "fixtures" / "double.json", R"json({
      "name": "double",
      "trigger": {"input": {"prompt": "原文"}},
      "expect": {"kind": "failed", "errorCode": "hook.next.already_consumed",
                 "records": {"PreUser/prompt.normalize": {"nextCalls": 2}}}
    })json");
    const HookCheckReport report = Check(dir.path);
    REQUIRE(report.static_pass);  // 语法/对账没问题——行为错要跑了才知道
    const HookCheckReport::Fixture* fixture = FindFixture(report, "double");
    REQUIRE(fixture != nullptr);
    CHECK(fixture->ran);
    CHECK(fixture->pass);  // 断言的是"第二次 next 被拒",约束生效即过
}

TEST_CASE("fake 档:HTTP 召回 + 工具桥走 fake adapter,context.append 断言到位") {
    TempDir dir("recall");
    WriteFile(dir.path / "hook.json", R"json({
      "schemaVersion": 1,
      "id": "knowledge-recall",
      "entry": "main.lua",
      "hooks": [{"hookPoint": "PostUser", "name": "knowledge.recall", "handler": "recall",
                 "capabilities": ["http", "tools", "context"]}]
    })json");
    WriteFile(dir.path / "main.lua", R"lua(
return {
  recall = function(ctx, input, next)
    local resp, err = luban.http.request({method = "GET", url = "https://kb.local/notes"})
    if resp == nil then
      return ctx.deny("recall_failed", "知识库召回失败: " .. tostring(err.code))
    end
    local result = luban.tools.call("kb.search", {query = "分代热换"})
    if result == nil then
      return ctx.deny("tool_failed", "工具桥失败")
    end
    luban.context.append("召回材料: " .. resp.body .. " / 检索: " .. result.content)
    return next(input)
  end,
}
)lua");
    WriteFile(dir.path / "fixtures" / "recall.json", R"json({
      "name": "recall",
      "trigger": {"input": {"prompt": "查一下热换的设计"}, "origin": "human"},
      "http": [{"status": 200, "body": "热换归分代热换装单"}],
      "tools": [{"name": "kb.search", "content": "三段材料"}],
      "expect": {"kind": "completed",
                 "contextAppends": ["召回材料: 热换归分代热换装单 / 检索: 三段材料"],
                 "records": {"PostUser/knowledge.recall": {"outcome": "completed", "nextConsumed": true}}}
    })json");
    // HTTP 失败分支:canned 耗尽 -> network_failed -> 业务 deny。
    WriteFile(dir.path / "fixtures" / "down.json", R"json({
      "name": "down",
      "trigger": {"input": {"prompt": "再查一次"}, "origin": "human"},
      "expect": {"kind": "denied", "denyCode": "recall_failed"}
    })json");
    const HookCheckReport report = Check(dir.path);
    REQUIRE(report.static_pass);
    REQUIRE(report.fixtures.size() == 2);
    CHECK(FindFixture(report, "recall")->pass);
    CHECK(FindFixture(report, "down")->pass);
    CHECK(report.exit_code() == 0);
    // 申请了 http/tools 的包,报告明列真实服务未验。
    bool saw_http_unverified = false;
    for (const std::string& item : report.unverified) {
        if (item.find("真实 HTTP") != std::string::npos) saw_http_unverified = true;
    }
    CHECK(saw_http_unverified);
}

TEST_CASE("报告:JSON 形状四档分账,fixturesPass 不拿零用例冒充过") {
    TempDir dir("report");
    WriteFile(dir.path / "hook.json", kCleanManifest);
    WriteFile(dir.path / "main.lua", kCleanLua);
    {
        const HookCheckReport report = Check(dir.path, /*run_fixtures=*/true);
        CHECK(report.fixtures.empty());
        CHECK_FALSE(report.fixtures_pass());
        const nlohmann::json json = report.ToJson();
        CHECK(json.at("staticPass") == true);
        CHECK(json.at("fixturesPass") == false);
        CHECK(json.at("fixtures").is_array());
        CHECK(json.at("unverified").size() >= 1);
    }
    {
        const HookCheckReport report = Check(dir.path, /*run_fixtures=*/false);
        const nlohmann::json json = report.ToJson();
        CHECK(json.at("checks").size() == 4);
        CHECK(json.at("exitCode") == 0);
    }
}
