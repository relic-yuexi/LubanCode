// TUI 排版批 3(tui优化todo.todo P3:/agents 与 /agent doctor/inspect)的
// 输出形状册。
//   - /agents:主表(agent/layer/state)+ 明细表(agent/desc/model/shadow)
//     + 加载警告框;state 列 pass/fail 语义色;
//   - /agent doctor:键值对框(标题嵌 "agent doctor: <名>")+ 诊断表
//     (level/issue,警告 skip 档/错误 error 档)+ 覆盖链列表;结论按缺项
//     数打 Pass/Error 档;
//   - /agent inspect:键值对框 + 迁移片段框(YAML 行)+ 来源账本列表;
//     语义长注框外原样跟出(批 1"长正文不塞框"裁量);
//   - 用法/reload/认不得的子命令全收 frame,不再裸打印;
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条;
//   - /agent 无 --json/机器消费分支(源码核实),字节级不变一条天然满足。
//
// 走纯 Format* 函数(手造 AgentCatalog,不摸三层扫描——目录扫描形状在
// tests/unit/agent_catalog/test_agent_commands.cpp 的 plain 册里钉);断言
// 只看形状与相对位置,不看绝对宽度(测试进程探不到终端,框宽按内容自适
// 应;真机宽度统一由主人会话内连敲验收)。

#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <vector>

#include "agent/agent_catalog.hpp"
#include "app/commands/agent_commands.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"

using namespace lubancode;

namespace {

class OutputCapture {
public:
    OutputCapture() { cli::TermPort().Redirect(&stream_, nullptr); }
    ~OutputCapture() { cli::TermPort().Reset(); }
    std::string text() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

std::string StripAnsi(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) {
                ++i;
            }
            if (i < text.size()) {
                ++i;  // 吃掉终结字母
            }
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int DisplayColOf(const std::string& row, const std::string& needle) {
    const std::size_t at = row.find(needle);
    if (at == std::string::npos) {
        return -1;
    }
    return static_cast<int>(cli::DisplayWidthUtf8(row.substr(0, at)));
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │
// 键值对的键按最宽键补空,分隔不是恒等两格——"键与值在同一行"才是稳定
// 形状(列宽随同框字段浮动,断言不跟补空走)。
bool LineWithText(const std::string& text, std::initializer_list<std::string> parts) {
    std::istringstream scan(text);
    std::string line;
    while (std::getline(scan, line)) {
        bool all = true;
        for (const std::string& part : parts) {
            if (line.find(part) == std::string::npos) {
                all = false;
                break;
            }
        }
        if (all) {
            return true;
        }
    }
    return false;
}

agent::AgentCatalogEntry MakeEntry(const std::string& name, bool available) {
    agent::AgentCatalogEntry entry;
    entry.name = name;
    entry.available = available;
    agent::AgentDefinition def;
    def.name = name;
    def.description = name + " 的描述";
    entry.definition = std::move(def);
    entry.layer = agent::AgentSourceLayer::User;
    entry.file = "test://" + name + ".yaml";
    if (!available) {
        agent::AgentDefinitionIssue error;
        error.field = "persona";
        error.message = "未知字段 persona";
        error.warning = false;
        entry.issues.push_back(std::move(error));
    }
    return entry;
}

struct AgentRig {
    cli::Theme theme;
    cli::Theme plain_theme;

    AgentRig() : theme(cli::BuiltinTheme("dark")), plain_theme(cli::BuiltinTheme("plain")) {}
};

}  // namespace

TEST_CASE("listing: 主表 + 明细表 + 加载警告框,state 列 pass/fail 语义色") {
    AgentRig rig;
    agent::AgentCatalog catalog;
    catalog.entries.push_back(MakeEntry("healthy", true));
    agent::AgentCatalogEntry broken = MakeEntry("wounded", false);
    broken.definition->skills_preload.push_back("no-such-skill");
    broken.shadowed_sources.push_back("test://lower.yaml");
    catalog.entries.push_back(std::move(broken));
    catalog.load_errors.push_back("user 层: dup-a.yaml 与 dup-b.yaml 同名");

    const std::vector<std::string> lines = app::FormatAgentCatalogListing(catalog, rig.theme);
    const std::string joined = [&] {
        std::string out;
        for (const std::string& line : lines) {
            out += line + "\n";
        }
        return out;
    }();
    const std::string plain = StripAnsi(joined);

    REQUIRE(Contains(joined, kBoxLightTopLeft));
    // 主表:首句进框顶(尾冒号剥掉),表头 schema 名。
    CHECK(Contains(plain, "Agent Catalog 共 2 个"));
    bool saw_header = false;
    std::istringstream scan(plain);
    std::string line;
    while (std::getline(scan, line)) {
        if (Contains(line, "agent") && Contains(line, "layer") && Contains(line, "state")) {
            saw_header = true;
        }
    }
    CHECK(saw_header);
    CHECK(LineWithText(plain, {"healthy", "user", "可用"}));
    CHECK(LineWithText(plain, {"wounded", "user", "不可用: "}));
    // state 语义色:可用 pass 档,不可用 error 档(fail 不另立色)。
    CHECK(Contains(joined, rig.theme.table_pass + "可用"));
    CHECK(Contains(joined, rig.theme.error + "不可用: "));
    // 明细表:描述/模型账/覆盖账,列头 desc/model/shadow。
    CHECK(Contains(plain, "desc"));
    CHECK(Contains(plain, "model"));
    CHECK(Contains(plain, "shadow"));
    CHECK(Contains(plain, "healthy 的描述"));
    CHECK(Contains(plain, "模型 inherit · effort inherit"));
    CHECK(Contains(plain, "(盖住: test://lower.yaml)"));
    // 加载警告框:标题 + 警告句。
    CHECK(Contains(plain, "加载警告"));
    CHECK(Contains(plain, "dup-a.yaml"));
}

TEST_CASE("listing: 25 名目录 ≥50 行,state 列对齐一处不差") {
    AgentRig rig;
    agent::AgentCatalog catalog;
    for (int i = 0; i < 25; ++i) {
        catalog.entries.push_back(MakeEntry("agent-" + std::to_string(i), true));
    }
    const std::vector<std::string> lines = app::FormatAgentCatalogListing(catalog, rig.plain_theme);
    REQUIRE(lines.size() >= 50);
    // plain 册对齐:所有"可用"单元格起始显示列一致(表列宽对齐后处处同列)。
    int first_col = -1;
    bool aligned = true;
    for (const std::string& line : lines) {
        const int col = DisplayColOf(line, "可用");
        if (col < 0) {
            continue;
        }
        if (first_col < 0) {
            first_col = col;
        } else if (col != first_col) {
            aligned = false;
        }
    }
    CHECK(first_col > 0);
    CHECK(aligned);
}

TEST_CASE("doctor: 键值对框 + 诊断表,缺项 ✗ 上 error 档、结论按缺项数落档") {
    AgentRig rig;
    agent::AgentCatalog catalog;
    agent::AgentCatalogEntry entry = MakeEntry("probe", true);
    entry.definition->skills_preload.push_back("missing-skill");
    entry.definition->tools.allow.push_back("read_file");
    entry.definition->tools.deny.push_back("read_file");
    entry.definition->mcp_servers.push_back("browser");
    agent::AgentDefinitionIssue warn;
    warn.field = "runtime.max_steps_per_turn";
    warn.message = "legacy 字段,建议迁移 agent.legacy_step_budget";
    warn.warning = true;
    entry.issues.push_back(std::move(warn));
    catalog.entries.push_back(std::move(entry));

    std::vector<tools::SkillMeta> no_skills;
    app::AgentDoctorMaterials materials;
    materials.skills = &no_skills;

    const std::vector<std::string> lines = app::FormatAgentDoctorReport(catalog, "probe", materials, {},
                                                                        rig.theme);
    const std::string joined = [&] {
        std::string out;
        for (const std::string& line : lines) {
            out += line + "\n";
        }
        return out;
    }();
    const std::string plain = StripAnsi(joined);

    REQUIRE(Contains(joined, kBoxLightTopLeft));
    CHECK(Contains(plain, "agent doctor: probe"));  // 标题嵌框顶
    CHECK(LineWithText(plain, {"定义", "解析通过"}));
    // 诊断表:level/issue 两列。首列恒走 row_label 加粗档(批 0 合同),
    // tone 只上非首列——级别色从简,信息靠文案。
    CHECK(Contains(joined, rig.theme.row_label + "[警告]"));
    CHECK(Contains(plain, "agent.legacy_step_budget"));
    // ✗ 缺项:技能清单里没有 preload 名,值上 error 档。
    CHECK(Contains(joined, rig.theme.error + "✗(不在已扫描技能清单)"));
    // 交叠与 runtime 账照登。
    CHECK(Contains(plain, "allow 与 deny 交叠  read_file(deny 胜出)"));
    CHECK(LineWithText(plain, {"runtime", "max_output_tokens=继承"}));
    // 结论:有缺项 → 中性档(不装通过)。MCP/工具材料没递,不产 ✗,
    // 缺项账只数技能那一处。
    CHECK(LineWithText(plain, {"结论", "定义可用,但静态预检发现 1 处缺项"}));
}

TEST_CASE("doctor: 解析通过零缺项时结论上 pass 档;查无此名给 error 框") {
    AgentRig rig;
    agent::AgentCatalog catalog;
    catalog.entries.push_back(MakeEntry("clean", true));
    {
        const std::vector<std::string> lines =
            app::FormatAgentDoctorReport(catalog, "clean", {}, {}, rig.theme);
        std::string joined;
        for (const std::string& line : lines) {
            joined += line + "\n";
        }
        CHECK(Contains(joined, rig.theme.table_pass + "解析通过"));
        CHECK(Contains(joined, rig.theme.table_pass + "静态预检通过"));
    }
    {
        const std::vector<std::string> lines =
            app::FormatAgentDoctorReport(catalog, "no-such", {}, {}, rig.theme);
        REQUIRE(lines.size() >= 1);
        const std::string plain = StripAnsi(lines[0]);
        CHECK(Contains(plain, "没有叫 \"no-such\""));
        CHECK(Contains(lines[0], rig.theme.error));
    }
}

TEST_CASE("inspect: 键值对框 + 迁移片段框 + 来源账本列表,语义长注框外原样") {
    AgentRig rig;
    agent::AgentCatalog catalog;
    agent::AgentCatalogEntry entry = MakeEntry("legacy", true);
    entry.definition->max_steps_per_turn = 9;
    catalog.entries.push_back(std::move(entry));

    const std::vector<std::string> lines = app::FormatAgentInspectReport(catalog, "legacy", {}, rig.theme);
    const std::string joined = [&] {
        std::string out;
        for (const std::string& line : lines) {
            out += line + "\n";
        }
        return out;
    }();
    const std::string plain = StripAnsi(joined);

    REQUIRE(Contains(joined, kBoxLightTopLeft));
    CHECK(Contains(plain, "agent inspect: legacy"));
    CHECK(Contains(plain, "定义来源  user test://legacy.yaml"));
    CHECK(LineWithText(plain, {"prompt", "profile=继承(落回 default)"}));
    CHECK(Contains(plain, "runtime 并流  显式声明 max_steps_per_turn=9"));
    // 迁移片段:YAML 行进框,语义长注(批 1 裁量 3)框外原样一行不截。
    CHECK(Contains(plain, "迁移片段(把 runtime 段的旧键换成下面这行即可)"));
    CHECK(Contains(plain, "runtime  max_turns: 9"));
    CHECK(Contains(plain, "agent.turn_budget_conflict"));
    // 来源账本列表:逐模块 FormatLine 原样。
    CHECK(Contains(plain, "Prompt 来源账本(逐模块,谁压了谁)"));
    CHECK(Contains(plain, "core/10-identity.md <- embedded default"));
}

TEST_CASE("usage/reload/认不得的子命令收 frame,认不得上 error 色") {
    AgentRig rig;
    app::AgentCommandContext ctx;
    ctx.theme = &rig.theme;
    {
        OutputCapture capture;
        cli::ParsedSlashCommand parsed;
        parsed.args = "reload";
        app::HandleSlashAgent(ctx, parsed);
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "属后续阶段"));
    }
    {
        OutputCapture capture;
        cli::ParsedSlashCommand parsed;
        parsed.args.clear();
        app::HandleSlashAgent(ctx, parsed);
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "用法"));
    }
    {
        OutputCapture capture;
        cli::ParsedSlashCommand parsed;
        parsed.args = "bogus-sub";
        app::HandleSlashAgent(ctx, parsed);
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "认不得的子命令"));
        CHECK(Contains(out, rig.theme.error));
    }
}

TEST_CASE("plain 主题: 全族输出零转义字节、无框字形(T3/--no-color 路径)") {
    AgentRig rig;
    agent::AgentCatalog catalog;
    catalog.entries.push_back(MakeEntry("plain-probe", true));
    for (const std::string& text : {
             [&] {  // listing
                 std::string out;
                 for (const std::string& line :
                      app::FormatAgentCatalogListing(catalog, rig.plain_theme)) {
                     out += line + "\n";
                 }
                 return out;
             }(),
             [&] {  // doctor
                 std::string out;
                 for (const std::string& line :
                      app::FormatAgentDoctorReport(catalog, "plain-probe", {}, {}, rig.plain_theme)) {
                     out += line + "\n";
                 }
                 return out;
             }(),
             [&] {  // inspect
                 std::string out;
                 for (const std::string& line :
                      app::FormatAgentInspectReport(catalog, "plain-probe", {}, rig.plain_theme)) {
                     out += line + "\n";
                 }
                 return out;
             }(),
         }) {
        CHECK(text.find("\x1b") == std::string::npos);
        CHECK(!Contains(text, kBoxLightTopLeft));
        CHECK(!Contains(text, kBoxLightVert));
    }
    // handler 面:reload 反馈在 plain 主题下同样零转义。
    app::AgentCommandContext ctx;
    ctx.theme = &rig.plain_theme;
    OutputCapture capture;
    cli::ParsedSlashCommand parsed;
    parsed.args = "reload";
    app::HandleSlashAgent(ctx, parsed);
    const std::string out = capture.text();
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(!Contains(out, kBoxLightTopLeft));
    // 信息一字不少:标题与结论文本都在,只是没了色与框。
    CHECK(Contains(out, "属后续阶段"));
}
