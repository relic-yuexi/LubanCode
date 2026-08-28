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
    CHECK(Contains(lines, "runtime: max_steps_per_turn=继承"));
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
