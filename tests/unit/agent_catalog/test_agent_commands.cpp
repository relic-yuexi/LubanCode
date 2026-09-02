// /agents 与 /agent doctor 的纯函数单测(自定义 Agent 单阶段 1):清单行、
// doctor 报告、缺名提示。不碰 IO(输出行怎么打进终端是 handler 的事)。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent_catalog.hpp"
#include "app/commands/agent_commands.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 一枚最小假工具(名字进注册表,doctor 的 ✓/✗ 比对用)。
class FakeTool : public tools::Tool {
public:
    explicit FakeTool(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return name_; }
    std::string description() const override { return "fake"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }
    Result execute(const nlohmann::json& input) override { return Result::Text("ok"); }

private:
    std::string name_;
};

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line + "\n";
    }
    return out;
}

// 临时 user 层目录里写几份定义,把 Catalog 造出来。
struct Fixture {
    std::filesystem::path base;
    Fixture() {
        base = std::filesystem::temp_directory_path() /
               ("lubancode_agent_cmds_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        std::filesystem::create_directories(base / "agents", ec);
    }
    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
    void Write(const std::string& file, const std::string& content) const {
        std::ofstream out(base / "agents" / file, std::ios::binary);
        out << content;
    }
    agent::AgentCatalog Load() const {
        agent::AgentCatalogScanRoots roots;
        roots.user_dir = base / "agents";
        return agent::LoadAgentCatalog(roots);
    }
};

bool Contains(const std::vector<std::string>& lines, const std::string& needle) {
    return JoinLines(lines).find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("FormatAgentCatalogListing:名称/层/可用性/描述/盖住账都进输出") {
    Fixture fx;
    fx.Write("layered.yaml", "schema: 1\nname: layered\ndescription: 用户层定义\n");
    fx.Write("broken.yaml", "schema: 1\nname: broken\ndescription: d\npersona: 多余\n");
    const agent::AgentCatalog catalog = fx.Load();

    const std::vector<std::string> lines = app::FormatAgentCatalogListing(catalog);
    CHECK(Contains(lines, "Agent Catalog 共 4 个"));
    CHECK(Contains(lines, "general-purpose  [builtin]  可用"));
    CHECK(Contains(lines, "Explore  [builtin]  可用"));
    CHECK(Contains(lines, "layered  [user]  可用"));
    CHECK(Contains(lines, "用户层定义"));
    CHECK(Contains(lines, "broken  [user]  不可用:"));
    CHECK(Contains(lines, "未知字段"));
    // 模型/Profile/工具/Skill 那一行:内置 Explore 有 allow,写得出数字。
    CHECK(Contains(lines, "5 allow"));
    CHECK(Contains(lines, "Profile 继承(落回 default)"));
}

TEST_CASE("FormatAgentCatalogListing:跨层覆盖与加载警告") {
    Fixture fx;
    fx.Write("general-purpose.yaml",
             "schema: 1\nname: general-purpose\ndescription: 用户改口的通用代理\n");
    fx.Write("dup-a.yaml", "schema: 1\nname: dup\ndescription: 第一份\n");
    fx.Write("dup-b.yaml", "schema: 1\nname: dup\ndescription: 第二份\n");
    const agent::AgentCatalog catalog = fx.Load();
    const std::vector<std::string> lines = app::FormatAgentCatalogListing(catalog);
    CHECK(Contains(lines, "(盖住: (builtin))"));
    CHECK(Contains(lines, "加载警告:"));
    CHECK(Contains(lines, "dup-a.yaml; "));  // 冲突两处来源一并摆出
}

TEST_CASE("FormatAgentDoctorReport:查无此名给一行指路") {
    const agent::AgentCatalog catalog = agent::LoadAgentCatalog({});
    const std::vector<std::string> lines = app::FormatAgentDoctorReport(catalog, "no-such", {});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("没有叫 \"no-such\"") != std::string::npos);
}

TEST_CASE("FormatAgentDoctorReport:坏定义逐条摆诊断,结论不可用") {
    Fixture fx;
    fx.Write("broken.yaml", "schema: 1\nname: broken\ndescription: d\npersona: 多余\n");
    const agent::AgentCatalog catalog = fx.Load();
    const std::vector<std::string> lines = app::FormatAgentDoctorReport(catalog, "broken", {});
    CHECK(Contains(lines, "定义: 不可用"));
    CHECK(Contains(lines, "[错误]"));
    CHECK(Contains(lines, "未知字段"));
    CHECK(Contains(lines, "结论: 不可用"));
}

TEST_CASE("FormatAgentDoctorReport:好定义对着材料核依赖,缺项打 ✗") {
    Fixture fx;
    fx.Write("probe.yaml", R"yaml(schema: 1
name: probe
description: 依赖核对的样本。
skills:
  preload:
    - browser-testing
    - missing-skill
tools:
  allow:
    - read_file
    - mcp__nope__shot
  deny:
    - read_file
mcp_servers:
  - browser
)yaml");
    const agent::AgentCatalog catalog = fx.Load();

    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("read_file"));
    std::vector<tools::SkillMeta> skills;
    tools::SkillMeta browser_skill;
    browser_skill.name = "browser-testing";
    skills.push_back(browser_skill);
    app::AgentDoctorMaterials materials;
    materials.skills = &skills;
    materials.registry = &registry;
    const std::vector<std::string> mcp_names{"browser"};
    materials.mcp_server_names = &mcp_names;

    const std::vector<std::string> lines = app::FormatAgentDoctorReport(catalog, "probe", materials);
    CHECK(Contains(lines, "定义: 解析通过"));
    CHECK(Contains(lines, "browser-testing ✓"));
    CHECK(Contains(lines, "missing-skill ✗(不在已扫描技能清单)"));
    CHECK(Contains(lines, "read_file ✓"));
    CHECK(Contains(lines, "mcp__nope__shot ✗(当前会话注册表里没有)"));
    CHECK(Contains(lines, "allow 与 deny 交叠: read_file(deny 胜出)"));
    CHECK(Contains(lines, "browser ✓ 已挂载"));
    // 阶段 3:runtime 五枚预算字段一并登账,没声明的如实写"继承"。
    CHECK(Contains(lines, "runtime: max_output_tokens=继承"));
    CHECK(Contains(lines, "max_steps_per_turn=继承"));
    CHECK(Contains(lines, "length_continuations=继承"));
    CHECK(Contains(lines, "permissions: inherit"));
    CHECK(Contains(lines, "结论: 定义可用,但静态预检发现"));
}

TEST_CASE("FormatAgentDoctorReport:材料全空时跳过比对,不猜") {
    Fixture fx;
    fx.Write("plain.yaml", "schema: 1\nname: plain\ndescription: d\n");
    const agent::AgentCatalog catalog = fx.Load();
    const std::vector<std::string> lines = app::FormatAgentDoctorReport(catalog, "plain", {});
    CHECK(Contains(lines, "定义: 解析通过"));
    CHECK(Contains(lines, "工具引用: 会话工具表不可用,跳过比对"));
    CHECK(Contains(lines, "Skill 预装: 无"));
    CHECK(Contains(lines, "MCP: 无"));
    CHECK(Contains(lines, "结论: 静态预检通过"));
}

TEST_CASE("FormatAgentDoctorReport:覆盖链摆出被盖住的来源") {
    Fixture fx;
    fx.Write("general-purpose.yaml",
             "schema: 1\nname: general-purpose\ndescription: 用户改口的通用代理\n");
    const agent::AgentCatalog catalog = fx.Load();
    const std::vector<std::string> lines = app::FormatAgentDoctorReport(catalog, "general-purpose", {});
    CHECK(Contains(lines, "来源: user "));
    CHECK(Contains(lines, "覆盖链(被盖住的来源,优先级从高到低):"));
    CHECK(Contains(lines, "(builtin)"));
}

// ---------------------------------------------------------------------------
// P1-0(turn 预算单 §5.2/§11.3):doctor/inspect 的预算合同判读与迁移建议。
// ---------------------------------------------------------------------------

TEST_CASE("P1-0 doctor:预算合同列明生效路,legacy 定义给迁移建议") {
    Fixture fx;
    fx.Write("legacy.yaml", "schema: 1\nname: legacy\ndescription: d\nruntime:\n  max_steps_per_turn: 9\n");
    fx.Write("fresh.yaml", "schema: 1\nname: fresh\ndescription: d\nruntime:\n  max_turns: 12\n");
    const agent::AgentCatalog catalog = fx.Load();

    // legacy 定义:警告摆出来,预算合同判读走旧路,迁移建议带替换数值。
    const std::vector<std::string> legacy_lines = app::FormatAgentDoctorReport(catalog, "legacy", {});
    CHECK(Contains(legacy_lines, "[警告]"));
    CHECK(Contains(legacy_lines, "agent.legacy_step_budget"));
    CHECK(Contains(legacy_lines, "预算合同: legacy per-run step 预算 9"));
    CHECK(Contains(legacy_lines, "迁移建议: 删掉 runtime.max_steps_per_turn"));
    CHECK(Contains(legacy_lines, "runtime.max_turns: 9"));

    // 新字段定义:任务 turn 预算一行,归属行,不给迁移建议。
    const std::vector<std::string> fresh_lines = app::FormatAgentDoctorReport(catalog, "fresh", {});
    CHECK(Contains(fresh_lines, "预算合同: task turn 预算 12(来源: Agent Definition runtime.max_turns"));
    CHECK(Contains(fresh_lines, "预算归属: TaskLedger 任务记录"));
    CHECK_FALSE(Contains(fresh_lines, "迁移建议"));

    // 两枚都没写:如实说落宿主默认。
    const std::vector<std::string> bare_lines = app::FormatAgentDoctorReport(catalog, "general-purpose", {});
    CHECK(Contains(bare_lines, "预算合同: 未显式声明"));
}

TEST_CASE("P1-0 inspect:legacy 定义给可复制的迁移片段,新字段不给") {
    Fixture fx;
    fx.Write("legacy.yaml", "schema: 1\nname: legacy\ndescription: d\nruntime:\n  max_steps_per_turn: 9\n");
    fx.Write("fresh.yaml", "schema: 1\nname: fresh\ndescription: d\nruntime:\n  max_turns: 12\n");
    const agent::AgentCatalog catalog = fx.Load();

    const std::vector<std::string> legacy_lines = app::FormatAgentInspectReport(catalog, "legacy", {});
    CHECK(Contains(legacy_lines, "迁移片段"));
    CHECK(Contains(legacy_lines, "max_turns: 9"));
    CHECK(Contains(legacy_lines, "agent.turn_budget_conflict"));

    const std::vector<std::string> fresh_lines = app::FormatAgentInspectReport(catalog, "fresh", {});
    CHECK(Contains(fresh_lines, "max_turns=12(任务总 turn)"));
    CHECK_FALSE(Contains(fresh_lines, "迁移片段"));
}

// ---------------------------------------------------------------------------
// 阶段 2:Profile 覆盖检查(doctor)与来源账本(inspect)。
// ---------------------------------------------------------------------------

// 临时 prompts 根:像 Fixture 一样现造,剖析纯函数喂哪层就解析哪层。
struct PromptsFixture {
    std::filesystem::path base;
    PromptsFixture() {
        base = std::filesystem::temp_directory_path() /
               ("lubancode_agent_prompts_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        std::filesystem::create_directories(base, ec);
    }
    ~PromptsFixture() {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
    }
    void Write(const std::string& rel, const std::string& content) const {
        const std::filesystem::path full = base / rel;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << content;
    }
    std::string Str() const { return base.string(); }
};

TEST_CASE("doctor 的 Profile 行:覆盖存在报层数,不存在打 ✗,default 不查") {
    Fixture fx;
    PromptsFixture prompts;
    prompts.Write("profiles/browser-tester/features/web.md", "用户层的联网方针。");
    fx.Write("named.yaml", "schema: 1\nname: named\ndescription: d\nprompt:\n  profile: browser-tester\n");
    fx.Write("ghost.yaml", "schema: 1\nname: ghost\ndescription: d\nprompt:\n  profile: no-such\n");
    fx.Write("plain.yaml", "schema: 1\nname: plain\ndescription: d\n");
    const agent::AgentCatalog catalog = fx.Load();

    app::AgentPromptContext ctx;
    ctx.user_prompts_dir = prompts.Str();

    // browser-tester:内置层(core/10-identity.md + features/web.md)+ 用户层
    // 的 web.md,报得出层数。
    const std::vector<std::string> named = app::FormatAgentDoctorReport(catalog, "named", {}, ctx);
    CHECK(Contains(named, "Profile: browser-tester(三层共"));
    CHECK(Contains(named, "个模块覆盖;/agent inspect named"));

    // no-such:三层全空,打 ✗ 并指路建目录。
    const std::vector<std::string> ghost = app::FormatAgentDoctorReport(catalog, "ghost", {}, ctx);
    CHECK(Contains(ghost, "Profile: no-such ✗"));
    CHECK(Contains(ghost, "profiles/no-such/"));

    // 没点名 Profile:default 上下文,不进 ✗ 账。
    const std::vector<std::string> plain = app::FormatAgentDoctorReport(catalog, "plain", {}, ctx);
    CHECK(Contains(plain, "Profile: 继承(落回 default)(default 上下文,三层覆盖不参与)"));
    CHECK_FALSE(Contains(plain, "✗"));
}

TEST_CASE("FormatAgentInspectReport:定义来源 + prompt 三笔 + 逐模块来源账") {
    Fixture fx;
    PromptsFixture prompts;
    prompts.Write("profiles/browser-tester/core/10-identity.md", "用户层的 Profile 身份。");
    fx.Write("probe.yaml",
             "schema: 1\nname: probe\ndescription: d\nprompt:\n  profile: browser-tester\n"
             "  project_instructions: omit\n  soul: off\n");
    const agent::AgentCatalog catalog = fx.Load();

    app::AgentPromptContext ctx;
    ctx.user_prompts_dir = prompts.Str();
    const std::vector<std::string> lines = app::FormatAgentInspectReport(catalog, "probe", ctx);
    CHECK(Contains(lines, "agent inspect: probe"));
    CHECK(Contains(lines, "定义来源: user "));
    CHECK(Contains(lines, "prompt: profile=browser-tester · project_instructions=omit · soul=off"));
    CHECK(Contains(lines, "Prompt 来源账本(逐模块,谁压了谁):"));
    // 账本逐行:用户层压住内置 Profile 的那条,与走嵌入 default 的那条。
    CHECK(Contains(lines, "core/10-identity.md <- user profile browser-tester"));
    CHECK(Contains(lines, "features/web.md <- embedded profile browser-tester"));
    CHECK(Contains(lines, "core/20-workflow.md <- embedded default"));
    CHECK(Contains(lines, "modes/default.md <- embedded host policy"));
    CHECK(Contains(lines, "依赖预检(Skill/MCP/工具/模型): /agent doctor probe"));
}

TEST_CASE("FormatAgentInspectReport:default 上下文与查无此名") {
    Fixture fx;
    fx.Write("plain.yaml", "schema: 1\nname: plain\ndescription: d\n");
    const agent::AgentCatalog catalog = fx.Load();

    const std::vector<std::string> missing = app::FormatAgentInspectReport(catalog, "no-such", {});
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].find("没有叫 \"no-such\"") != std::string::npos);

    const std::vector<std::string> lines = app::FormatAgentInspectReport(catalog, "plain", {});
    CHECK(Contains(lines, "prompt: profile=继承(落回 default) · project_instructions=inherit · soul=inherit"));
    CHECK(Contains(lines, "core/10-identity.md <- embedded default"));
    CHECK(Contains(lines, "(default 上下文:三层 Profile 覆盖不参与"));
}
