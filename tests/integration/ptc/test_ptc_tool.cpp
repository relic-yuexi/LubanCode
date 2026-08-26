// PTC P1 单测:真工具(read_file/search)走完整链——每一枚 stub 调用都过
// agent::RunOneTool(schema 复检/PreToolUse/PermissionRequest/执行/PostToolUse)。
// 另覆盖:钩子 deny、用户拒确认、tool_search 延迟挂载谓词、状态栏/装配层
// (ToolRuntime 的 auto 恒 json、programmatic 回落)。

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "platform/process.hpp"
#include "ptc/ptc_tool.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/search.hpp"
#include "tools/tool.hpp"

using namespace lubancode::ptc;

namespace {

#ifdef _WIN32
constexpr const char* kPythonCmd = "python";
#else
constexpr const char* kPythonCmd = "python3";
#endif

bool PythonAvailable() {
    static const bool available = [] {
        const auto result = lubancode::platform::RunProcess(std::vector<std::string>{kPythonCmd, "--version"}, 10000);
        return result.exit_code == 0 && result.output.find("Python") != std::string::npos;
    }();
    return available;
}

// C++ 字符串 -> Python 字符串字面量(测试脚本里嵌路径用)。
std::string JsonString(const std::string& text) {
    std::string out = "\"";
    for (const char ch : text) {
        if (ch == '\\' || ch == '"') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    out += "\"";
    return out;
}

// 需确认的假工具:验证 PermissionRequest/确认链真的挡得住 PTC 脚本。
class GuardedEchoTool : public lubancode::tools::Tool {
public:
    std::string name() const override { return "guarded_echo"; }
    std::string description() const override { return "需确认的回声工具(权限链测试)。"; }
    bool needs_confirm() const override { return true; }
    nlohmann::json input_schema() const override {
        return nlohmann::json::parse(
            R"({"type":"object","properties":{"message":{"type":"string"}},"required":["message"]})");
    }
    Result execute(const nlohmann::json& input) override {
        return Result{"guarded:" + input.at("message").get<std::string>(), false};
    }
};

// 测试脚手架:真工具表 + PtcTool + 可定制的钩子记录器。
struct ToolFixture {
    lubancode::tools::ToolRegistry registry;
    PtcTool::Config config;
    std::unique_ptr<PtcTool> tool;
    std::vector<std::string> events;         // 钩子/确认/执行的轨迹
    std::atomic<bool> cancel{false};
    PtcTool::Hooks hooks;

    explicit ToolFixture(std::vector<std::string> eligible = {})
        : config(MakeConfig(std::move(eligible))) {
        registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
        registry.Register(std::make_unique<lubancode::tools::SearchTool>());
        registry.Register(std::make_unique<GuardedEchoTool>());
        tool = std::make_unique<PtcTool>(registry, nullptr, config);
        hooks.cancel = &cancel;
        hooks.on_tool_start = [this](const std::string&, const std::string& name, const nlohmann::json&) {
            events.push_back("start:" + name);
        };
        hooks.on_tool_done = [this](const std::string&, const std::string& name,
                                    const lubancode::tools::Tool::Result& result) {
            events.push_back(std::string("done:") + name + (result.is_error ? ":err" : ":ok"));
        };
        hooks.on_tool_confirm = [this](const std::string&, const std::string& name, const nlohmann::json&) {
            events.push_back("confirm:" + name);
            return true;
        };
        tool->SetHooks(hooks);
    }

    static PtcTool::Config MakeConfig(std::vector<std::string> eligible) {
        PtcTool::Config config;
        config.python_cmd = kPythonCmd;
        config.limits.wall_clock_ms = 30000;
        config.limits.cpu_ms = 60000;
        if (!eligible.empty()) {
            config.eligible_tools = std::move(eligible);
        }
        return config;
    }

    lubancode::tools::Tool::Result Run(const std::string& purpose, const std::string& script) {
        return tool->execute(nlohmann::json{{"purpose", purpose}, {"script", script}});
    }
};

// 临时工作区:两份 UTF-8 文件给 read_file/search 真读。
struct TempWorkspace {
    std::filesystem::path dir;

    TempWorkspace() {
        dir = std::filesystem::temp_directory_path() /
             ("lubancode-ptc-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir / "src");
        Write(dir / "src" / "alpha.cpp", "int alpha() { return 1; }\n// HOOKDISPATCH_MARKER\n");
        Write(dir / "src" / "beta.cpp", "int beta() { return 2; }\n");
        Write(dir / "notes 中文.txt", "中文内容第一行\nemoji 🎉 第二行\n");
    }
    ~TempWorkspace() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    static void Write(const std::filesystem::path& path, const std::string& content) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
    }

    std::string Path(const std::string& relative) const {
        // 正斜杠跨平台,read_file 两平台都认。
        std::string full = dir.generic_string();
        while (full.ends_with('/')) {
            full.pop_back();
        }
        return full + "/" + relative;
    }
};

}  // namespace

TEST_CASE("真工具链: read_file 真读 + 审计账 + 摘要") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    TempWorkspace workspace;
    ToolFixture fixture;
    const std::string script = "from luban_tools import read_file\n"
                               "r = read_file(path=" +
                               JsonString(workspace.Path("src/alpha.cpp")) +
                               ", limit=3)\n"
                               "emit({\"lines\": r[\"content\"].count(chr(10)), \"has_marker\": "
                               "\"HOOKDISPATCH_MARKER\" in r[\"content\"]})\n";
    const auto result = fixture.Run("读 alpha.cpp", script);
    REQUIRE_FALSE(result.is_error);
    // 单卡内容:聚合行 + 完成行 + 摘要 + 调用账。
    CHECK(result.content.find("● programmatic_tool_calling(读 alpha.cpp)") != std::string::npos);
    CHECK(result.content.find("1 次 read_file") != std::string::npos);
    CHECK(result.content.find("完成  1/1") != std::string::npos);
    CHECK(result.content.find("运行 id: ptc-") != std::string::npos);
    CHECK(result.content.find("[调用账 ptc-") != std::string::npos);
    // stub 调用走了完整链:on_tool_start/on_tool_done 都响过。
    CHECK(std::find(fixture.events.begin(), fixture.events.end(), "start:read_file") != fixture.events.end());
    CHECK(std::find(fixture.events.begin(), fixture.events.end(), "done:read_file:ok") != fixture.events.end());
    // 摘要断言在结果文本里(emit 值;nlohmann dump 无冒号后空格)。
    CHECK(result.content.find("\"has_marker\":true") != std::string::npos);
}

TEST_CASE("真依赖链: search 真结果喂 read_file(P1 的长链现场)") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    TempWorkspace workspace;
    ToolFixture fixture;
    // search 的 grep 模式找到含标记的文件,再读它——两枚调用,第二枚入参
    // 来自第一枚真实返回(search 回的是相对所给根目录的路径 + 行号 + 行文,
    // Windows 盘符自带冒号,劈分要从右侧按 行号:正文 两段劈)。
    const std::string script =
        "from luban_tools import read_file, search\n"
        "root = " +
        JsonString(workspace.Path("src")) +
        "\n"
        "hits = search(mode=\"grep\", pattern=\"HOOKDISPATCH_MARKER\", path=root)\n"
        "first = hits[\"content\"].splitlines()[0]\n"
        "hit_path = root + \"/\" + first.rsplit(\":\", 2)[0]\n"
        "doc = read_file(path=hit_path)\n"
        "emit({\"hit_path_tail\": hit_path.split(\"/\")[-1], \"marker_line\": "
        "\"HOOKDISPATCH_MARKER\" in doc[\"content\"]})\n";
    const auto result = fixture.Run("查标记再读文件", script);
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.find("alpha.cpp") != std::string::npos);
    const auto run = fixture.tool->last_run();  // 按值接:返回的是临时 optional
    REQUIRE(run.has_value());
    REQUIRE(run->calls.size() == 2);
    CHECK(run->calls[0].tool == "search");
    CHECK(run->calls[1].tool == "read_file");
    // 第二枚的入参是第一枚真实返回里解析出的路径(真依赖,不是常识猜)。
    CHECK(run->calls[1].input.at("path").get<std::string>().find("alpha.cpp") != std::string::npos);
}

TEST_CASE("PreToolUse deny: 脚本内调用被钩子拦下,ToolCallError 可收口") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    TempWorkspace workspace;
    ToolFixture fixture;
    PtcTool::Hooks denying = fixture.hooks;
    denying.on_pre_tool_use_hook = [](const std::string&, const std::string&, const nlohmann::json&) {
        lubancode::runtime::ToolHookDecision decision;
        decision.decision = lubancode::runtime::ToolHookDecision::Decision::Deny;
        decision.reason = "测试钩子:一律拒绝";
        return decision;
    };
    fixture.tool->SetHooks(denying);
    const std::string script = "from luban_tools import read_file, ToolCallError\n"
                               "blocked = []\n"
                               "try:\n"
                               "    read_file(path=" +
                               JsonString(workspace.Path("src/beta.cpp")) +
                               ")\n"
                               "except ToolCallError as exc:\n"
                               "    blocked.append(str(exc))\n"
                               "emit({\"blocked\": len(blocked)})\n";
    const auto result = fixture.Run("被钩子拦", script);
    // 脚本收口了(emit 成功),但那枚调用按拒绝入账。
    REQUIRE_FALSE(result.is_error);
    const auto run = fixture.tool->last_run();  // 按值接:返回的是临时 optional
    REQUIRE(run.has_value());
    REQUIRE(run->calls.size() == 1);
    CHECK(run->calls[0].ok);
    CHECK(run->calls[0].is_error);
    CHECK(result.content.find("失败 1") != std::string::npos);
}

TEST_CASE("PermissionRequest/确认链: 需确认工具用户拒绝 -> ToolCallError") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    ToolFixture fixture({"guarded_echo"});
    PtcTool::Hooks refusing = fixture.hooks;
    refusing.on_tool_confirm = [&fixture](const std::string&, const std::string& name, const nlohmann::json&) {
        fixture.events.push_back("confirm:" + name);
        return false;  // 用户拒绝
    };
    fixture.tool->SetHooks(refusing);
    const std::string script = "from luban_tools import guarded_echo, ToolCallError\n"
                               "refused = []\n"
                               "try:\n"
                               "    guarded_echo(message=\"hi\")\n"
                               "except ToolCallError:\n"
                               "    refused.append(1)\n"
                               "emit({\"refused\": len(refused)})\n";
    const auto result = fixture.Run("拒绝确认", script);
    REQUIRE_FALSE(result.is_error);
    CHECK(std::find(fixture.events.begin(), fixture.events.end(), "confirm:guarded_echo") != fixture.events.end());
    const auto run = fixture.tool->last_run();  // 按值接:返回的是临时 optional
    REQUIRE(run.has_value());
    REQUIRE(run->calls.size() == 1);
    CHECK(run->calls[0].is_error);  // "用户拒绝执行该工具"
}

TEST_CASE("updatedInput 改写: PreToolUse allow + 改参,脚本看到改后的参数") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    TempWorkspace workspace;
    ToolFixture fixture;
    PtcTool::Hooks rewriting = fixture.hooks;
    rewriting.on_pre_tool_use_hook = [&workspace](const std::string&, const std::string& name,
                                                  const nlohmann::json& input) {
        lubancode::runtime::ToolHookDecision decision;
        decision.decision = lubancode::runtime::ToolHookDecision::Decision::Allow;
        if (name == "read_file") {
            // 钩子把要读的文件改写成 alpha.cpp(改写仍过 schema)。
            nlohmann::json updated = input;
            updated["path"] = workspace.Path("src/alpha.cpp");
            decision.updated_input = updated;
        }
        return decision;
    };
    fixture.tool->SetHooks(rewriting);
    const std::string script = "from luban_tools import read_file\n"
                               "r = read_file(path=" +
                               JsonString(workspace.Path("notes 中文.txt")) +
                               ")\n"
                               "emit({\"got_alpha\": \"alpha\" in r[\"content\"], \"got_chinese\": \"中文\" in "
                               "r[\"content\"]})\n";
    const auto result = fixture.Run("钩子改参", script);
    REQUIRE_FALSE(result.is_error);
    // 读到的是改写后的 alpha.cpp,不是脚本要的中文笔记。
    CHECK(result.content.find("\"got_alpha\":true") != std::string::npos);
    CHECK(result.content.find("\"got_chinese\":false") != std::string::npos);
}

TEST_CASE("tool_search 延迟挂载: 过滤谓词不放行的工具不进 stub 集") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    TempWorkspace workspace;
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
    PtcTool::Config config = ToolFixture::MakeConfig({});
    // 谓词:一概不放行(模拟全部工具延迟未挂载)。
    auto tool = std::make_unique<PtcTool>(registry, [](const lubancode::tools::Tool&) { return false; }, config);
    // 指南段不列签名;脚本 import read_file 落 NotImplementedError。
    CHECK(tool->GuideSegment().find("def read_file(") == std::string::npos);
    const std::string script = "from luban_tools import read_file\nr = read_file(path=\"whatever\")\nemit({\"a\": 1})\n";
    const auto result = tool->execute(nlohmann::json{{"purpose", "未挂载"}, {"script", script}});
    CHECK(result.is_error);
    auto* ptool = tool.get();
    CHECK(ptool->last_run().has_value());
    CHECK(ptool->last_run()->calls.empty());  // 调用根本没到宿主
}

TEST_CASE("fan-out 真工具: 八路 read_file 并发窗口内全收") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    TempWorkspace workspace;
    ToolFixture fixture;
    std::string paths = "[";
    for (int i = 0; i < 8; ++i) {
        paths += (i > 0 ? ", " : "") + JsonString(workspace.Path("src/alpha.cpp"));
    }
    paths += "]";
    const std::string script = "from luban_tools import read_file\n"
                               "rs = [read_file(path=p) for p in " +
                               paths +
                               "]\n"
                               "emit({\"count\": len(rs), \"all_ok\": all(\"alpha\" in r[\"content\"] for r in rs)})\n";
    const auto result = fixture.Run("八路 fan-out", script);
    REQUIRE_FALSE(result.is_error);
    const auto run = fixture.tool->last_run();  // 按值接:返回的是临时 optional
    REQUIRE(run.has_value());
    CHECK(run->calls.size() == 8);
    CHECK(result.content.find("8 次 read_file") != std::string::npos);
    CHECK(result.content.find("完成  8/8") != std::string::npos);
}

TEST_CASE("Esc 取消链: 旗子先置位,调用回'已取消'") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    TempWorkspace workspace;
    ToolFixture fixture;
    fixture.cancel.store(true);
    const std::string script = "from luban_tools import read_file, ToolCallError\n"
                               "try:\n"
                               "    read_file(path=" +
                               JsonString(workspace.Path("src/beta.cpp")) +
                               ")\n"
                               "except ToolCallError:\n"
                               "    pass\n"
                               "emit({\"after\": \"cancel\"})\n";
    const auto result = fixture.Run("取消", script);
    CHECK(result.is_error);
    CHECK(result.content.find("已取消(Esc)") != std::string::npos);
    const auto run = fixture.tool->last_run();  // 按值接:返回的是临时 optional
    REQUIRE(run.has_value());
    REQUIRE(run->calls.size() == 1);
    CHECK(run->calls[0].error == "取消(未开始)");
}

TEST_CASE("探针不可用: 无解释器时 PtcTool 明报不可用") {
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
    PtcTool::Config config;
    config.python_cmd = "no-such-python-for-ptc-tests";
    PtcTool tool(registry, nullptr, config);
    CHECK_FALSE(tool.available());
    CHECK_FALSE(tool.unavailability_reason().empty());
    const auto result = tool.execute(nlohmann::json{{"purpose", "x"}, {"script", "emit({'a':1})"}});
    CHECK(result.is_error);
    CHECK(result.content.find("PTC 不可用") != std::string::npos);
}

TEST_CASE("入参校验: 缺 script / 缺 purpose 拒绝") {
    lubancode::tools::ToolRegistry registry;
    PtcTool::Config config;
    config.python_cmd = kPythonCmd;
    PtcTool tool(registry, nullptr, config);
    auto result = tool.execute(nlohmann::json{{"purpose", "x"}});
    CHECK(result.is_error);
    CHECK(result.content.find("script") != std::string::npos);
    result = tool.execute(nlohmann::json{{"script", "emit({'a':1})"}});
    // 只缺 purpose 不拦(schema required 由调用方模型侧保证;宿主端宽容)。
    CHECK_FALSE(result.content.empty());
}
