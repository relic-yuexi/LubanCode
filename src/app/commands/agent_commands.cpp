// /agents 与 /agent doctor 的实现:Catalog 现扫现列、doctor 静态预检。
// 输出走 cli/terminal_port(散打 std::cout 清零的仓库规矩);排版走
// cli::frame 三助手(TUI 排版批 3,约定见 docs/development/tui_style.md)。
#include "app/commands/agent_commands.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

#include "agent/prompt_assembler.hpp"  // BuildPromptProfileLedger(阶段 2 来源账本)
#include "app/tool_runtime.hpp"  // McpServerRuntime(/agent doctor 的 MCP 面材料)
#include "cli/line_editor.hpp"     // TruncateUtf8ToDisplayWidth:框顶标题帽(批 3)
#include "cli/terminal_frame.hpp"  // frame::*(TUI 排版批 3:/agents /agent 渲染段)
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"  // ResolveTheme:没接会话主题时按批 2 CLI 裁量现起
#include "config/config.hpp"                  // HomeLubancodeDir
#include "config/project_instructions.hpp"    // FindProjectRoot(项目层根)
#include "package/mounting.hpp"               // MountAgentEntries/MountProfileRoots(阶段 3)
#include "platform/console.hpp"               // GetScreenInfo:整条 /agent 的框宽同一把尺
#include "platform/paths.hpp"

using lubancode::cli::TermOut;

namespace lubancode::app {

namespace {

// ---- TUI 排版批 3(/agents 与 /agent doctor/inspect)的公共小件 --------------
//
// 渲染段只调 cli::frame::* 三助手(批 0 基件,约定见 docs/development/
// tui_style.md)。本文件的历史文案是硬编码中文(不走 i18n 表),单子合同
// "不新增文案"在此读作:既有句子原样进 frame,一字不添不改(批 2 裁量 1);
// 表头与列名用数据 schema 名(agent/layer/state/level…,批 1 裁量 1);句内
// 冒号按 SentenceField 拆两列;长正文(YAML 迁移片段)框外原样(批 1 裁量 3)。

namespace frame = lubancode::cli::frame;

int AgentFrameWidth() {
    if (const auto info = lubancode::platform::GetScreenInfo()) {
        return info->width;
    }
    return 0;
}

void EmitFrameLines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        TermOut() << line << "\n";
    }
}

std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

frame::Field SentenceField(const std::string& sentence,
                           frame::FieldAccent accent = frame::FieldAccent::None) {
    const std::size_t colon = sentence.find(':');
    if (colon == std::string::npos) {
        return frame::Field{"", sentence, accent};
    }
    return frame::Field{TrimAscii(sentence.substr(0, colon)), TrimAscii(sentence.substr(colon + 1)), accent};
}

// 框顶标题帽:WrapInBox 不截标题,超预算会撑破框——先按"预算-7"(两侧
// 边框衬空 4 列 + 标题前后衬 3 列)截掉,保字头不劈宽字。窄终端不破框。
std::string ClampFrameTitle(std::string title, int width) {
    if (width <= 0) {
        return title;
    }
    const int cap = width - 7;
    if (cap <= 0) {
        return {};
    }
    return lubancode::cli::TruncateUtf8ToDisplayWidth(std::move(title), cap);
}

void PrintNotice(const lubancode::cli::Theme& theme, std::initializer_list<std::string> sentences,
                 frame::FieldAccent accent = frame::FieldAccent::None) {
    std::vector<frame::Field> fields;
    for (const std::string& sentence : sentences) {
        fields.push_back(SentenceField(sentence, accent));
    }
    EmitFrameLines(frame::RenderKeyValues({}, fields, theme, frame::Light(), AgentFrameWidth()));
}

// 会话主题优先;没接(测试/空分派表)按批 2 CLI 裁量现起——管道/重定向
// 自然降 plain,测试进程里钉的就是 plain 形状。
lubancode::cli::Theme ResolveAgentTheme(const AgentCommandContext& ctx) {
    if (ctx.theme != nullptr) {
        return *ctx.theme;
    }
    return lubancode::cli::ResolveTheme(std::string(),
                                        lubancode::cli::DetectConsoleCapability().colors_enabled);
}

// 嵌入式资源里的 builtin Agent 目录:<exe 目录>/agents(与官方 skills 同一
// 相对布局)。开发构建与发行包都没有这目录 = builtin 层只剩码内两条,静默。
std::optional<std::filesystem::path> EmbeddedAgentsDir() {
    const auto executable = lubancode::platform::ExecutablePath();
    if (!executable.has_value()) {
        return std::nullopt;
    }
    const std::filesystem::path candidate = executable->parent_path() / "agents";
    std::error_code ec;
    if (!std::filesystem::is_directory(candidate, ec) || ec) {
        return std::nullopt;
    }
    return candidate;
}

// 模型/Profile/工具三行的展示值:省了就写"继承",不写空串让人猜。
std::string DescribeModelRole(const lubancode::agent::AgentDefinition& def) {
    return def.model.role.empty() ? std::string("inherit") : def.model.role;
}

std::string DescribeEffort(const lubancode::agent::AgentDefinition& def) {
    return def.model.effort.empty() ? std::string("inherit") : def.model.effort;
}

std::string DescribeProfile(const lubancode::agent::AgentDefinition& def) {
    return def.prompt.profile.has_value() ? *def.prompt.profile : std::string("继承(落回 default)");
}

std::string DescribeTools(const lubancode::agent::AgentDefinition& def) {
    if (def.tools.allow.empty() && def.tools.deny.empty()) {
        return "全量(未裁)";
    }
    std::string out = std::to_string(def.tools.allow.size()) + " allow";
    if (!def.tools.deny.empty()) {
        out += " / " + std::to_string(def.tools.deny.size()) + " deny";
    }
    return out;
}

// 交集(保 allow 原序):allow 与 deny 撞名时 deny 胜出(单子测试账),
// doctor 把撞上的名字摆出来。
std::vector<std::string> AllowDenyOverlap(const lubancode::agent::AgentDefinition& def) {
    std::set<std::string> deny(def.tools.deny.begin(), def.tools.deny.end());
    std::vector<std::string> out;
    for (const std::string& name : def.tools.allow) {
        if (deny.count(name) != 0) {
            out.push_back(name);
        }
    }
    return out;
}

bool HasError(const lubancode::agent::AgentCatalogEntry& entry) {
    for (const auto& issue : entry.issues) {
        if (!issue.warning) {
            return true;
        }
    }
    return false;
}

// 用户 prompts 根(~/.lubancode/prompts)的 UTF-8 串;没主目录给空串
//(= 只有嵌入层,与拼装侧"prompts_dir 空只用嵌入版"同语义)。
std::string UserPromptsRootUtf8() {
    const auto home = lubancode::config::HomeLubancodeDir();
    return home.has_value() ? (*home + "/prompts") : std::string();
}

// Profile 是否真有覆盖:账本里只要有任一模块来自三个 Profile 层(内置/
// 用户/项目)就算存在。doctor 的"Profile 是否存在"查的就是这个。
std::size_t ProfileOverlayCount(const lubancode::agent::PromptSourceLedger& ledger) {
    std::size_t count = 0;
    for (const auto& entry : ledger.entries) {
        if (entry.origin == lubancode::agent::PromptModuleOrigin::EmbeddedProfile ||
            entry.origin == lubancode::agent::PromptModuleOrigin::UserProfile ||
            entry.origin == lubancode::agent::PromptModuleOrigin::ProjectProfile) {
            ++count;
        }
    }
    return count;
}

}  // namespace

std::vector<std::string> FormatAgentCatalogListing(const lubancode::agent::AgentCatalog& catalog,
                                                   const lubancode::cli::Theme& theme) {
    std::vector<std::string> lines;
    const int width = AgentFrameWidth();
    const std::string title = ClampFrameTitle(
        "Agent Catalog 共 " + std::to_string(catalog.entries.size()) +
            " 个(优先级 project > user > package > builtin;包层带 canonical 名"
            " <包id>:<名>;/agent doctor <名字> 看静态预检)",
        width);  // 既有首句进框顶,引导下文的尾冒号剥掉(批 2 裁量 3)
    if (catalog.entries.empty()) {
        // 空目录:表格没行,标题句降为键值对提示,信息不丢。
        std::vector<frame::Field> fields;
        fields.push_back(frame::Field{"", title, frame::FieldAccent::Muted});
        return frame::RenderKeyValues({}, fields, theme, frame::Light(), width);
    }
    // 主表:一名一行,短字段(agent/layer/state);state 列 pass/fail 语义色。
    std::vector<frame::TableColumn> columns;
    columns.push_back({"agent"});
    columns.push_back({"layer"});
    columns.push_back({"state"});
    std::vector<frame::TableRow> rows;
    for (const auto& entry : catalog.entries) {
        rows.push_back(frame::TableRow{
            {entry.name, lubancode::agent::ToString(entry.layer),
             entry.available ? std::string("可用") : "不可用: " + entry.FirstError()},
            {frame::CellTone::Normal, frame::CellTone::Normal,
             entry.available ? frame::CellTone::Pass : frame::CellTone::Fail}});
    }
    for (const std::string& line : frame::RenderTable(title, columns, rows, theme, frame::Light(), width)) {
        lines.push_back(line);
    }
    // 明细表:描述/模型账/覆盖账是长字段,另起一张表(批 2"长字段另表"
    // 裁量)。覆盖账多来源时以 "; " 连排(拆的是排版,不添改文字)。
    std::vector<frame::TableColumn> detail_columns;
    detail_columns.push_back({"agent"});
    detail_columns.push_back({"desc"});
    detail_columns.push_back({"model"});
    detail_columns.push_back({"shadow"});
    std::vector<frame::TableRow> detail_rows;
    for (const auto& entry : catalog.entries) {
        if (!entry.definition.has_value()) {
            continue;
        }
        std::string shadows;
        for (const std::string& shadow : entry.shadowed_sources) {
            shadows += (shadows.empty() ? "" : "; ") + shadow;
        }
        detail_rows.push_back(frame::TableRow{
            {entry.name, entry.definition->description,
             "模型 " + DescribeModelRole(*entry.definition) + " · effort " + DescribeEffort(*entry.definition) +
                 " · Profile " + DescribeProfile(*entry.definition) + " · 工具 " +
                 DescribeTools(*entry.definition) + " · 预装 Skill " +
                 std::to_string(entry.definition->skills_preload.size()),
             shadows.empty() ? std::string() : "(盖住: " + shadows + ")"}});
    }
    for (const std::string& line :
         frame::RenderTable({}, detail_columns, detail_rows, theme, frame::Light(), width)) {
        lines.push_back(line);
    }
    if (!catalog.load_errors.empty()) {
        std::vector<frame::Field> warn_fields;
        for (const std::string& error : catalog.load_errors) {
            warn_fields.push_back(SentenceField(error));
        }
        for (const std::string& line :
             frame::RenderKeyValues("加载警告", warn_fields, theme, frame::Light(), width)) {
            lines.push_back(line);
        }
    }
    return lines;
}

std::vector<std::string> FormatAgentDoctorReport(const lubancode::agent::AgentCatalog& catalog,
                                                 const std::string& name, const AgentDoctorMaterials& materials,
                                                 const AgentPromptContext& prompts,
                                                 const lubancode::cli::Theme& theme) {
    std::vector<std::string> lines;
    const int width = AgentFrameWidth();
    const auto* entry = catalog.Find(name);
    if (entry == nullptr) {
        const std::vector<frame::Field> not_found{
            frame::Field{"", "没有叫 \"" + name + "\" 的 Agent(先 /agents 看清单;名字大小写敏感)。",
                         frame::FieldAccent::Error}};
        return frame::RenderKeyValues({}, not_found, theme, frame::Light(), width);
    }
    // 字段账:先攒后渲。✗ 缺项计数在拼值处点数(旧逻辑数行里的 " ✗",
    // 行里带 ✗ 的只有这些值,口径不变);覆盖链/诊断表在账缝里各自成框。
    std::vector<frame::Field> fields;
    bool titled = false;  // 首个键值对框顶嵌 "agent doctor: <名>"
    const auto flush = [&] {
        if (fields.empty()) {
            return;
        }
        std::string flush_title;
        if (!titled) {
            flush_title = ClampFrameTitle("agent doctor: " + entry->name, width);
            titled = true;
        }
        for (const std::string& line :
             frame::RenderKeyValues(flush_title, fields, theme, frame::Light(), width)) {
            lines.push_back(line);
        }
        fields.clear();
    };
    fields.push_back(frame::Field{
        "来源", lubancode::agent::ToString(entry->layer) + " " + entry->file});
    if (!entry->shadowed_sources.empty()) {
        flush();
        std::vector<frame::ListRow> shadow_rows;
        for (const std::string& shadow : entry->shadowed_sources) {
            shadow_rows.push_back(frame::ListRow{"", shadow});
        }
        for (const std::string& line : frame::RenderList("覆盖链(被盖住的来源,优先级从高到低)", shadow_rows,
                                                         theme, frame::Light(), width)) {
            lines.push_back(line);
        }
    }

    // ---- 定义本体:解析诊断逐条摆(错在前、警告在后,保持解析次序) ----
    if (entry->definition.has_value() && !HasError(*entry)) {
        fields.push_back(frame::Field{"定义", "解析通过", frame::FieldAccent::Pass});
    } else {
        fields.push_back(frame::Field{"定义", "不可用,诊断 " + std::to_string(entry->issues.size()) + " 条",
                                      frame::FieldAccent::Error});
    }
    if (!entry->issues.empty()) {
        flush();
        std::vector<frame::TableColumn> issue_columns;
        issue_columns.push_back({"level"});
        issue_columns.push_back({"issue"});
        std::vector<frame::TableRow> issue_rows;
        for (const auto& issue : entry->issues) {
            issue_rows.push_back(frame::TableRow{
                {std::string("[") + (issue.warning ? "警告" : "错误") + "]", issue.Format(entry->file)},
                {issue.warning ? frame::CellTone::Skip : frame::CellTone::Fail, frame::CellTone::Normal}});
        }
        for (const std::string& line :
             frame::RenderTable({}, issue_columns, issue_rows, theme, frame::Light(), width)) {
            lines.push_back(line);
        }
    }
    if (!entry->definition.has_value()) {
        fields.push_back(frame::Field{"结论", "不可用 —— 定义没解析成,先把上面的错改了再查依赖。",
                                      frame::FieldAccent::Error});
        flush();
        return lines;
    }
    const auto& def = *entry->definition;
    std::size_t problems = 0;  // ✗ 缺项账(拼值处点数,口径与旧"数行"一致)

    // ---- 模型与 Profile:role 写法在此定死三档;能力校验属阶段 3 ----
    fields.push_back(frame::Field{"模型", "role=" + DescribeModelRole(def) + " · effort=" + DescribeEffort(def) +
                                             "(档位是否越过 provider 能力,阶段 3 的 resolver 查)"});

    // ---- Profile(阶段 2):名字 + 覆盖是否存在(三层里有没有任何模块) ----
    // 阶段 3:包层根一并递进——canonical 名("<包id>:<名>")的覆盖只在包里。
    if (!def.prompt.profile.has_value() || *def.prompt.profile == "default") {
        fields.push_back(frame::Field{"Profile",
                                      DescribeProfile(def) + "(default 上下文,三层覆盖不参与)"});
    } else {
        const lubancode::agent::PromptSourceLedger ledger = lubancode::agent::BuildPromptProfileLedger(
            *def.prompt.profile, prompts.user_prompts_dir, prompts.project_prompts_dir,
            prompts.package_roots);
        const std::size_t overlays = ProfileOverlayCount(ledger);
        if (overlays > 0) {
            fields.push_back(frame::Field{
                "Profile", *def.prompt.profile + "(三层共 " + std::to_string(overlays) +
                               " 个模块覆盖;/agent inspect " + entry->name + " 看逐段来源账本)"});
        } else {
            fields.push_back(frame::Field{
                "Profile", *def.prompt.profile +
                               " ✗(内置/用户/项目三层都没有任何模块覆盖,现全走 default 模块;先建 "
                               "profiles/" + *def.prompt.profile + "/ 下的覆盖文件)",
                frame::FieldAccent::Error});
            ++problems;
        }
    }

    // ---- Skill 预装 ----
    if (def.skills_preload.empty()) {
        fields.push_back(frame::Field{"Skill 预装", "无"});
    } else {
        std::string text;
        for (std::size_t i = 0; i < def.skills_preload.size(); ++i) {
            if (i != 0) {
                text += "; ";
            }
            text += def.skills_preload[i];
            if (materials.skills != nullptr) {
                bool found = false;
                for (const auto& skill : *materials.skills) {
                    if (skill.name == def.skills_preload[i]) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    text += " ✓";
                } else {
                    text += " ✗(不在已扫描技能清单)";
                    ++problems;
                }
            }
        }
        fields.push_back(frame::Field{"Skill 预装", std::move(text)});
    }

    // ---- 工具引用:allow/deny/requires 对注册表;交叠点名列出(deny 胜出) ----
    if (materials.registry != nullptr) {
        const auto check_list = [&](const std::vector<std::string>& names, const std::string& label) {
            if (names.empty()) {
                return std::string();
            }
            std::string text;
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i != 0) {
                    text += "; ";
                }
                text += names[i];
                if (materials.registry->Find(names[i]) != nullptr) {
                    text += " ✓";
                } else {
                    text += " ✗(当前会话注册表里没有)";
                    ++problems;
                }
            }
            fields.push_back(frame::Field{label, std::move(text)});
            return std::string();
        };
        check_list(def.tools.allow, "tools.allow");
        check_list(def.tools.deny, "tools.deny");
        check_list(def.requires_tools, "requires.tools");
    } else {
        fields.push_back(frame::Field{"工具引用", "会话工具表不可用,跳过比对"});
    }
    if (const std::vector<std::string> overlap = AllowDenyOverlap(def); !overlap.empty()) {
        std::string text;
        for (std::size_t i = 0; i < overlap.size(); ++i) {
            text += (i == 0 ? "" : "; ") + overlap[i];
        }
        text += "(deny 胜出)";
        fields.push_back(frame::Field{"allow 与 deny 交叠", std::move(text)});
    }

    // ---- MCP:只许引用已挂载的服务名 ----
    if (def.mcp_servers.empty()) {
        fields.push_back(frame::Field{"MCP", "无"});
    } else {
        std::string text;
        for (std::size_t i = 0; i < def.mcp_servers.size(); ++i) {
            if (i != 0) {
                text += "; ";
            }
            text += def.mcp_servers[i];
            if (materials.mcp_server_names != nullptr) {
                const bool mounted = std::find(materials.mcp_server_names->begin(),
                                               materials.mcp_server_names->end(),
                                               def.mcp_servers[i]) != materials.mcp_server_names->end();
                if (mounted) {
                    text += " ✓ 已挂载";
                } else {
                    text += " ✗ 未挂载";
                    ++problems;
                }
            }
        }
        fields.push_back(frame::Field{"MCP", std::move(text)});
    }

    // ---- runtime 与 permissions:登账;权限越界比对属阶段 3 ----
    // 阶段 3 起五枚预算字段一并登(并流口径:YAML 显式 > 父值;steps 另有
    // 入参与配置默认两级,见 AgentProfileResolver)。显式与否如实标——
    // "继承"就是落父值,不猜数。P1-0(turn 预算单 §5.2/§11.3)起 max_turns
    // 一并登,并列明生效的是任务级 turn 预算还是 legacy per-run step 预算。
    // 同名防混(清理批):这里登的 max_turns 是 Agent Definition 域的任务
    // 总 turn;配置文件顶层同名旧键 max_turns(config.hpp)是
    // max_steps_per_turn(每输入轮步数)的弃用别名——两域极性相反,诊断
    // 文案必须带上下文,别裸写 max_turns。
    std::string runtime = "max_output_tokens=";
    runtime += def.max_output_tokens.has_value()
                   ? std::to_string(*def.max_output_tokens) + "(YAML 显式,视同 config 级)"
                   : std::string("继承");
    runtime += " · max_steps_per_turn=";
    runtime += def.max_steps_per_turn.has_value()
                   ? std::to_string(*def.max_steps_per_turn) + "(入参 > YAML > 配置默认;legacy,待迁移)"
                   : std::string("继承");
    runtime += " · max_turns=";
    runtime += def.max_turns.has_value()
                   ? std::to_string(*def.max_turns) + "(任务总 turn;override > YAML > subagent.default_max_turns)"
                   : std::string("继承(配置 subagent.default_max_turns,未设 = 0 不限)");
    runtime += " · context_window_tokens=";
    runtime += def.context_window_tokens.has_value() ? std::to_string(*def.context_window_tokens)
                                                     : std::string("继承");
    runtime += " · length_continuations=";
    runtime += def.length_continuations.has_value() ? std::to_string(*def.length_continuations)
                                                    : std::string("继承");
    runtime += " · execution_mode=" + (def.execution_mode.empty() ? std::string("auto") : def.execution_mode);
    runtime += " · isolation=" + (def.isolation.empty() ? std::string("none") : def.isolation);
    fields.push_back(frame::Field{"runtime", std::move(runtime)});
    // ---- 预算合同判读(turn 预算单 §11.3/§5.1,P1-0)--------------------------
    // 列明生效的是哪条路,顺带给迁移建议:老定义不突变,新定义不掉进每轮
    // 重置漏洞,用户一眼看得出自己走哪条。
    if (def.max_turns.has_value()) {
        fields.push_back(frame::Field{"预算合同", "task turn 预算 " + std::to_string(*def.max_turns) +
                                                      "(来源: Agent Definition runtime.max_turns;续投、孩子回流、Stop 钩子续跑共这本账)"});
    } else if (def.max_steps_per_turn.has_value()) {
        fields.push_back(frame::Field{"预算合同", "legacy per-run step 预算 " +
                                                      std::to_string(*def.max_steps_per_turn) +
                                                      "(每个 input round 各自上限;续投/Stop 钩子会重领额度)——待迁移"});
        fields.push_back(frame::Field{"迁移建议", "删掉 runtime.max_steps_per_turn,改写 runtime.max_turns: " +
                                                      std::to_string(*def.max_steps_per_turn) +
                                                      "(任务总 turn,一道闸管到底;语义从\"每轮各自\"变\"整任务合计\",按需调大数值)"});
    } else {
        fields.push_back(frame::Field{
            "预算合同", "未显式声明(task turn 落 subagent.default_max_turns,未设 = 0 不限)"});
    }
    fields.push_back(frame::Field{"预算归属", "TaskLedger 任务记录(attempted/completed 分账;正常收场 reserved=0)"});
    fields.push_back(frame::Field{"permissions",
                                  (def.permissions_mode.empty() ? std::string("inherit") : def.permissions_mode) +
                                      "(与父 Agent 按自动能力集合求交，may_prompt 取 AND，子不得扩大父能力)"});

    // ---- 结论:定义解析过 ≠ 依赖齐;缺项如实数出来 ----
    if (entry->available && problems == 0) {
        fields.push_back(frame::Field{"结论", "静态预检通过(没发现缺项;运行期合并父上下文是阶段 3 的事)。",
                                      frame::FieldAccent::Pass});
    } else if (entry->available) {
        fields.push_back(frame::Field{"结论", "定义可用,但静态预检发现 " + std::to_string(problems) +
                                                  " 处缺项(派活时 resolver 会按 requires 报缺,不会悄悄放宽)。"});
    } else {
        fields.push_back(frame::Field{"结论", "不可用 —— " + entry->FirstError(), frame::FieldAccent::Error});
    }
    flush();
    return lines;
}

// /agent inspect <name> 的报告(阶段 2 落地):定义来源与覆盖链、prompt 段
// 三笔开关、Prompt 来源账本(单子 §5.5——哪个模块从哪层哪文件来,出了
// 覆盖问题一眼看见是谁压了谁)。模型/权限的最终合并属阶段 3,这里只登
// 定义里写的值;依赖预检归 /agent doctor,各管一摊。
std::vector<std::string> FormatAgentInspectReport(const lubancode::agent::AgentCatalog& catalog,
                                                  const std::string& name, const AgentPromptContext& prompts,
                                                  const lubancode::cli::Theme& theme) {
    std::vector<std::string> lines;
    const int width = AgentFrameWidth();
    const auto* entry = catalog.Find(name);
    if (entry == nullptr) {
        const std::vector<frame::Field> not_found{
            frame::Field{"", "没有叫 \"" + name + "\" 的 Agent(先 /agents 看清单;名字大小写敏感)。",
                         frame::FieldAccent::Error}};
        return frame::RenderKeyValues({}, not_found, theme, frame::Light(), width);
    }
    std::vector<frame::Field> fields;
    bool titled = false;  // 首个键值对框顶嵌 "agent inspect: <名>"
    const auto flush = [&] {
        if (fields.empty()) {
            return;
        }
        std::string flush_title;
        if (!titled) {
            flush_title = ClampFrameTitle("agent inspect: " + entry->name, width);
            titled = true;
        }
        for (const std::string& line :
             frame::RenderKeyValues(flush_title, fields, theme, frame::Light(), width)) {
            lines.push_back(line);
        }
        fields.clear();
    };
    fields.push_back(frame::Field{
        "定义来源", lubancode::agent::ToString(entry->layer) + " " + entry->file});
    if (!entry->shadowed_sources.empty()) {
        flush();
        std::vector<frame::ListRow> shadow_rows;
        for (const std::string& shadow : entry->shadowed_sources) {
            shadow_rows.push_back(frame::ListRow{"", shadow});
        }
        for (const std::string& line : frame::RenderList("覆盖链(被盖住的来源,优先级从高到低)", shadow_rows,
                                                         theme, frame::Light(), width)) {
            lines.push_back(line);
        }
    }
    if (!entry->definition.has_value()) {
        fields.push_back(frame::Field{"定义", "没解析成,没有可查的 Prompt 账本 —— 先 /agent doctor " +
                                                  entry->name + " 看诊断。",
                                      frame::FieldAccent::Error});
        flush();
        return lines;
    }
    const auto& def = *entry->definition;

    // prompt 段三笔:profile / project_instructions / soul。
    const std::string project_instructions =
        def.prompt.project_instructions == lubancode::agent::AgentPromptSpec::ProjectInstructions::Omit
            ? "omit"
            : "inherit";
    const std::string soul = def.prompt.soul == lubancode::agent::AgentPromptSpec::Soul::Off ? "off" : "inherit";
    fields.push_back(frame::Field{"prompt", "profile=" + DescribeProfile(def) + " · project_instructions=" +
                                                project_instructions + " · soul=" + soul});

    // runtime 并流账(阶段 3):定义里显式声明的预算字段逐笔点名,没声明的
    // 落父值。来源口径:入参显式 > YAML runtime > 父值/配置默认
    //(AgentProfileResolver 是唯一权威,两条派发路同一份)。
    {
        std::string declared;
        const auto append = [&declared](const char* field, const std::string& value) {
            if (!declared.empty()) {
                declared += "、";
            }
            declared += std::string(field) + "=" + value;
        };
        if (def.max_output_tokens.has_value()) {
            append("max_output_tokens", std::to_string(*def.max_output_tokens) + "(来源档:config 级)");
        }
        if (def.max_steps_per_turn.has_value()) {
            append("max_steps_per_turn", std::to_string(*def.max_steps_per_turn) + "(legacy,待迁移)");
        }
        if (def.max_turns.has_value()) {
            append("max_turns", std::to_string(*def.max_turns) + "(任务总 turn)");
        }
        if (def.context_window_tokens.has_value()) {
            append("context_window_tokens", std::to_string(*def.context_window_tokens));
        }
        if (def.length_continuations.has_value()) {
            append("length_continuations", std::to_string(*def.length_continuations));
        }
        fields.push_back(frame::Field{"runtime 并流",
                                      declared.empty()
                                          ? std::string("定义未显式声明预算字段,四枚全落父值")
                                          : ("显式声明 " + declared + ";其余落父值")});
    }
    // ---- 迁移片段(turn 预算单 §5.2 阶段 B,P1-0):旧字段还在用的定义给
    // 一段可直接复制的替换 YAML;新字段的定义不补这段。语义长注是正文,
    // 框外原样跟出(批 1"长正文不塞框"裁量,截断会丢迁移口径)。
    if (def.max_steps_per_turn.has_value()) {
        flush();
        const std::vector<frame::Field> yaml_fields{
            frame::Field{"runtime", "max_turns: " + std::to_string(*def.max_steps_per_turn)}};
        for (const std::string& line :
             frame::RenderKeyValues("迁移片段(把 runtime 段的旧键换成下面这行即可)", yaml_fields, theme,
                                    frame::Light(), width)) {
            lines.push_back(line);
        }
        lines.push_back(
            "(语义变化:旧键是\"每个 input round 各自上限\",新键是\"整项任务合计\";"
            "按任务实际规模调数值,再删旧键——两者同现会按 agent.turn_budget_conflict 拒载)");
    }

    // 来源账本:整张 default 模块树在这个 Profile 上下文下逐段解析。
    flush();
    const std::string profile = def.prompt.profile.value_or(std::string());
    std::vector<frame::ListRow> ledger_rows;
    for (const auto& ledger_entry :
         lubancode::agent::BuildPromptProfileLedger(profile, prompts.user_prompts_dir,
                                                    prompts.project_prompts_dir, prompts.package_roots)
             .entries) {
        ledger_rows.push_back(frame::ListRow{"", ledger_entry.FormatLine()});
    }
    for (const std::string& line : frame::RenderList("Prompt 来源账本(逐模块,谁压了谁)", ledger_rows, theme,
                                                     frame::Light(), width)) {
        lines.push_back(line);
    }
    if (!lubancode::agent::IsPromptProfileActive(profile)) {
        fields.push_back(frame::Field{"", "(default 上下文:三层 Profile 覆盖不参与;改一个用户 Profile 文件只影响"
                                          "点名它的 Agent)",
                                      frame::FieldAccent::Muted});
    }
    fields.push_back(frame::Field{"依赖预检(Skill/MCP/工具/模型)", "/agent doctor " + entry->name});
    flush();
    return lines;
}

lubancode::agent::AgentCatalogScanRoots ComputeAgentScanRoots(
    std::vector<lubancode::agent::PackagedAgentEntry> packaged) {
    lubancode::agent::AgentCatalogScanRoots roots;
    roots.builtin_dir = EmbeddedAgentsDir();
    if (const auto home = lubancode::config::HomeLubancodeDir(); home.has_value()) {
        roots.user_dir = lubancode::platform::Utf8ToPath(*home) / "agents";
    }
    roots.project_dir = lubancode::config::FindProjectRoot(std::filesystem::current_path()) / ".lubancode" /
                        "agents";
    // 统一 Package 封装单阶段 3:包层成品件(调用方从会话钉快照折来)。
    roots.packaged = std::move(packaged);
    return roots;
}

// 现行快照折包层成品件(快照缺席 = 空表,行为与从前一致)。HC-06 第三
// 小批:改经 package_snapshot_provider 现取——reload 换档后旧快照会释放,
// 冻指针会悬垂;provider 返回现行 shared_ptr,调用期间保活。
std::vector<lubancode::agent::PackagedAgentEntry> PackagedAgentsFromMount(const AgentCommandContext& ctx) {
    if (ctx.package_snapshot_provider == nullptr) {
        return {};
    }
    const std::shared_ptr<const lubancode::package::PackageSnapshot> snapshot = ctx.package_snapshot_provider();
    if (snapshot == nullptr) {
        return {};
    }
    return lubancode::package::MountAgentEntries(snapshot->mount());
}

std::vector<lubancode::agent::PackageProfileRoot> PackagedProfileRootsFromMount(const AgentCommandContext& ctx) {
    if (ctx.package_snapshot_provider == nullptr) {
        return {};
    }
    const std::shared_ptr<const lubancode::package::PackageSnapshot> snapshot = ctx.package_snapshot_provider();
    if (snapshot == nullptr) {
        return {};
    }
    return lubancode::package::MountProfileRoots(snapshot->mount());
}

// Prompt Profile 的项目层根(阶段 2):<项目根>/.lubancode/prompts,UTF-8
// 串。项目根与 Agent 扫描同一条发现规则(FindProjectRoot),不各自猜 cwd。
// 目录不存在照旧返回——拼装侧对缺席层静默跳过。会话装配与 /agent inspect
// 都从这儿拿,一个口径。
std::string ComputeProjectPromptsRoot() {
    return lubancode::platform::PathToUtf8(lubancode::config::FindProjectRoot(std::filesystem::current_path()) /
                                           ".lubancode" / "prompts");
}

CommandFlow HandleSlashAgents(const AgentCommandContext& ctx,
                              const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    const lubancode::cli::Theme theme = ResolveAgentTheme(ctx);
    const lubancode::agent::AgentCatalog catalog = lubancode::agent::LoadAgentCatalog(
        ComputeAgentScanRoots(PackagedAgentsFromMount(ctx)));
    EmitFrameLines(FormatAgentCatalogListing(catalog, theme));
    return CommandFlow::Continue;
}

CommandFlow HandleSlashAgent(const AgentCommandContext& ctx,
                             const lubancode::cli::ParsedSlashCommand& parsed) {
    const lubancode::cli::Theme theme = ResolveAgentTheme(ctx);
    // 拆子命令与名字(名可含连字符,不能按词数硬拆,取第一个词后全部当名字)。
    std::string sub = parsed.args;
    std::string rest;
    const std::size_t space = parsed.args.find_first_of(" \t");
    if (space != std::string::npos) {
        sub = parsed.args.substr(0, space);
        rest = parsed.args.substr(space + 1);
    }
    const auto trim = [](std::string value) {
        const auto begin = value.find_first_not_of(" \t");
        if (begin == std::string::npos) {
            return std::string();
        }
        const auto end = value.find_last_not_of(" \t");
        return value.substr(begin, end - begin + 1);
    };
    rest = trim(rest);

    if (sub.empty()) {
        PrintNotice(theme, {"用法:/agent doctor <名字>(静态预检)、/agent inspect <名字>(Prompt 来源账本)。",
                            "/agents 列清单。"});
        return CommandFlow::Continue;
    }
    if (sub == "doctor") {
        if (rest.empty()) {
            PrintNotice(theme, {"用法:/agent doctor <名字>(名字看 /agents;大小写敏感)。"});
            return CommandFlow::Continue;
        }
        const lubancode::agent::AgentCatalog catalog = lubancode::agent::LoadAgentCatalog(
            ComputeAgentScanRoots(PackagedAgentsFromMount(ctx)));
        AgentDoctorMaterials materials;
        materials.skills = ctx.skills;
        materials.registry = ctx.registry;
        std::vector<std::string> mcp_names;
        if (ctx.mcp_servers != nullptr) {
            for (const auto& runtime : *ctx.mcp_servers) {
                mcp_names.push_back(runtime.name);
            }
            materials.mcp_server_names = &mcp_names;
        }
        AgentPromptContext prompts;
        prompts.user_prompts_dir = UserPromptsRootUtf8();
        prompts.project_prompts_dir = ComputeProjectPromptsRoot();
        prompts.package_roots = PackagedProfileRootsFromMount(ctx);
        EmitFrameLines(FormatAgentDoctorReport(catalog, rest, materials, prompts, theme));
        return CommandFlow::Continue;
    }
    if (sub == "inspect") {
        if (rest.empty()) {
            PrintNotice(theme, {"用法:/agent inspect <名字>(名字看 /agents;大小写敏感)。"});
            return CommandFlow::Continue;
        }
        const lubancode::agent::AgentCatalog catalog = lubancode::agent::LoadAgentCatalog(
            ComputeAgentScanRoots(PackagedAgentsFromMount(ctx)));
        AgentPromptContext prompts;
        prompts.user_prompts_dir = UserPromptsRootUtf8();
        prompts.project_prompts_dir = ComputeProjectPromptsRoot();
        prompts.package_roots = PackagedProfileRootsFromMount(ctx);
        EmitFrameLines(FormatAgentInspectReport(catalog, rest, prompts, theme));
        return CommandFlow::Continue;
    }
    if (sub == "reload") {
        PrintNotice(theme, {"/agent reload 属后续阶段(阶段 3 统一解析时连同原子替换一起落);现阶段 Catalog "
                            "现扫现建,改了 YAML 下一次派发即生效。"});
        return CommandFlow::Continue;
    }
    PrintNotice(theme, {"认不得的子命令 \"" + sub + "\"。用法:/agent doctor <名字>、/agent inspect <名字>。"},
                frame::FieldAccent::Error);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
