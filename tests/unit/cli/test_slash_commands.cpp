// ParseSlashCommand:纯函数,输入串 -> 命令枚举 + 参数,不碰任何 IO。

#include <doctest/doctest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "cli/console_input.hpp"
#include "cli/i18n.hpp"  // /context-window 描述词条对账(ContextWindow 单)
#include "cli/slash_commands.hpp"

using namespace lubancode;

TEST_CASE("ParseSlashCommand: 非 / 开头,不拦截") {
    const auto parsed = cli::ParseSlashCommand("帮我看看这个文件");
    CHECK(parsed.command == cli::SlashCommand::NotSlash);
}

TEST_CASE("ParseSlashCommand: 空串,不拦截") {
    const auto parsed = cli::ParseSlashCommand("");
    CHECK(parsed.command == cli::SlashCommand::NotSlash);
}

TEST_CASE("ParseSlashCommand: 只有空白,不拦截") {
    const auto parsed = cli::ParseSlashCommand("   ");
    CHECK(parsed.command == cli::SlashCommand::NotSlash);
}

TEST_CASE("ParseSlashCommand: /help") {
    const auto parsed = cli::ParseSlashCommand("/help");
    CHECK(parsed.command == cli::SlashCommand::Help);
    CHECK(parsed.args.empty());
}

TEST_CASE("ParseSlashCommand: /model 不带参数") {
    const auto parsed = cli::ParseSlashCommand("/model");
    CHECK(parsed.command == cli::SlashCommand::Model);
    CHECK(parsed.args.empty());
}

TEST_CASE("ParseSlashCommand: /model 带参数,参数就是模型名") {
    const auto parsed = cli::ParseSlashCommand("/model MiniMax-M2.7");
    CHECK(parsed.command == cli::SlashCommand::Model);
    CHECK(parsed.args == "MiniMax-M2.7");
}

TEST_CASE("ParseSlashCommand: /model 参数前后带多余空白,剥掉") {
    const auto parsed = cli::ParseSlashCommand("/model   MiniMax-M3   ");
    CHECK(parsed.command == cli::SlashCommand::Model);
    CHECK(parsed.args == "MiniMax-M3");
}

TEST_CASE("ParseSlashCommand: /config") {
    CHECK(cli::ParseSlashCommand("/config").command == cli::SlashCommand::Config);
}

TEST_CASE("ParseSlashCommand: /update 与 check 参数") {
    CHECK(cli::ParseSlashCommand("/update").command == cli::SlashCommand::Update);
    const auto parsed = cli::ParseSlashCommand("/UPDATE check");
    CHECK(parsed.command == cli::SlashCommand::Update);
    CHECK(parsed.args == "check");

    bool listed = false;
    for (const auto& command : cli::AllSlashCommands()) {
        if (command.name == "/update") listed = true;
    }
    CHECK(listed);
}

TEST_CASE("ParseSlashCommand: /init 大小写不敏感,出现在候选里") {
    CHECK(cli::ParseSlashCommand("/init").command == cli::SlashCommand::Init);
    CHECK(cli::ParseSlashCommand("/INIT").command == cli::SlashCommand::Init);
    bool found = false;
    for (const auto& command : cli::AllSlashCommands()) {
        found = found || command.name == "/init";
    }
    CHECK(found);
}

TEST_CASE("ParseSlashCommand: /clear") {
    CHECK(cli::ParseSlashCommand("/clear").command == cli::SlashCommand::Clear);
}

TEST_CASE("ParseSlashCommand: /exit 和 /quit 都算 Exit") {
    CHECK(cli::ParseSlashCommand("/exit").command == cli::SlashCommand::Exit);
    CHECK(cli::ParseSlashCommand("/quit").command == cli::SlashCommand::Exit);
}

TEST_CASE("ParseSlashCommand: 命令词大小写不敏感") {
    CHECK(cli::ParseSlashCommand("/Help").command == cli::SlashCommand::Help);
    CHECK(cli::ParseSlashCommand("/MODEL").command == cli::SlashCommand::Model);
    CHECK(cli::ParseSlashCommand("/ExIt").command == cli::SlashCommand::Exit);
}

TEST_CASE("ParseSlashCommand: 不认得的命令,返回 Unknown 且带上原始命令词") {
    const auto parsed = cli::ParseSlashCommand("/foobar");
    CHECK(parsed.command == cli::SlashCommand::Unknown);
    CHECK(parsed.raw_word == "/foobar");
}

TEST_CASE("ParseSlashCommand: 单独一个 / 也算 Unknown(不是 NotSlash)") {
    const auto parsed = cli::ParseSlashCommand("/");
    CHECK(parsed.command == cli::SlashCommand::Unknown);
}

TEST_CASE("ParseSlashCommand: 输入前后有空白,先剥掉再判断") {
    const auto parsed = cli::ParseSlashCommand("   /help   ");
    CHECK(parsed.command == cli::SlashCommand::Help);
}

TEST_CASE("ParseSlashCommand: /context 不带参数") {
    const auto parsed = cli::ParseSlashCommand("/context");
    CHECK(parsed.command == cli::SlashCommand::Context);
    CHECK(parsed.args.empty());
}

TEST_CASE("ParseSlashCommand: /context 带档位参数") {
    const auto parsed = cli::ParseSlashCommand("/context 512k");
    CHECK(parsed.command == cli::SlashCommand::Context);
    CHECK(parsed.args == "512k");
}

TEST_CASE("ParseSlashCommand: /compact 不带参数") {
    const auto parsed = cli::ParseSlashCommand("/compact");
    CHECK(parsed.command == cli::SlashCommand::Compact);
    CHECK(parsed.args.empty());
}

TEST_CASE("ParseSlashCommand: /compact 带重点说明参数") {
    const auto parsed = cli::ParseSlashCommand("/compact 重点保留数据库配置");
    CHECK(parsed.command == cli::SlashCommand::Compact);
    CHECK(parsed.args == "重点保留数据库配置");
}

TEST_CASE("ParseSlashCommand: /think 不带参数") {
    const auto parsed = cli::ParseSlashCommand("/think");
    CHECK(parsed.command == cli::SlashCommand::Think);
    CHECK(parsed.args.empty());
}

TEST_CASE("ParseSlashCommand: /think 带档位参数") {
    const auto parsed = cli::ParseSlashCommand("/think high");
    CHECK(parsed.command == cli::SlashCommand::Think);
    CHECK(parsed.args == "high");
}

TEST_CASE("ParseSlashCommand: /context /compact /think 命令词大小写不敏感") {
    CHECK(cli::ParseSlashCommand("/CONTEXT").command == cli::SlashCommand::Context);
    CHECK(cli::ParseSlashCommand("/Compact").command == cli::SlashCommand::Compact);
    CHECK(cli::ParseSlashCommand("/THINK").command == cli::SlashCommand::Think);
}

TEST_CASE("AllSlashCommands: 新命令 /context /compact /think 都在列表里") {
    const auto& commands = cli::AllSlashCommands();
    bool has_context = false;
    bool has_compact = false;
    bool has_think = false;
    for (const auto& c : commands) {
        if (c.name == "/context") has_context = true;
        if (c.name == "/compact") has_compact = true;
        if (c.name == "/think") has_think = true;
    }
    CHECK(has_context);
    CHECK(has_compact);
    CHECK(has_think);
}

TEST_CASE("ParseSlashCommand: /effort 是 /think 的别名,解到同一个命令") {
    const auto parsed = cli::ParseSlashCommand("/effort");
    CHECK(parsed.command == cli::SlashCommand::Think);
    CHECK(parsed.args.empty());
}

TEST_CASE("ParseSlashCommand: /context-window 裸敲开面板,与 /context 不混") {
    const auto parsed = cli::ParseSlashCommand("/context-window");
    CHECK(parsed.command == cli::SlashCommand::ContextWindow);
    CHECK(parsed.args.empty());
    // 带参数(第一版不认):args 原样递给 handler 给短用法。
    const auto with_args = cli::ParseSlashCommand("/context-window 200k");
    CHECK(with_args.command == cli::SlashCommand::ContextWindow);
    CHECK(with_args.args == "200k");
    // 大小写不敏感;与 /context 占用分析仍是两个命令。
    CHECK(cli::ParseSlashCommand("/Context-Window").command == cli::SlashCommand::ContextWindow);
    CHECK(cli::ParseSlashCommand("/CONTEXT").command == cli::SlashCommand::Context);
}

TEST_CASE("AllSlashCommands: /context-window 在列表里,描述走 i18n 词条") {
    const auto& commands = cli::AllSlashCommands();
    bool has_context_window = false;
    for (const auto& c : commands) {
        if (c.name == "/context-window") {
            has_context_window = true;
            CHECK(c.description == cli::tr("slash.desc.context_window"));
            CHECK(!c.description.empty());
        }
    }
    CHECK(has_context_window);  // /help 与 Tab 补全同出这张表,不另造名单
}

TEST_CASE("ParseSlashCommand: /effort 带档位参数,大小写不敏感") {
    const auto parsed = cli::ParseSlashCommand("/Effort xhigh");
    CHECK(parsed.command == cli::SlashCommand::Think);
    CHECK(parsed.args == "xhigh");
}

TEST_CASE("AllSlashCommands: /effort 单独列一条,/help、Tab 候选都能看见") {
    const auto& commands = cli::AllSlashCommands();
    bool has_effort = false;
    for (const auto& c : commands) {
        if (c.name == "/effort") has_effort = true;
    }
    CHECK(has_effort);
}

TEST_CASE("ParseSlashCommand: /todos") {
    const auto parsed = cli::ParseSlashCommand("/todos");
    CHECK(parsed.command == cli::SlashCommand::Todos);
    CHECK(parsed.args.empty());
}

TEST_CASE("ParseSlashCommand: /memory 在解析和候选表里") {
    const auto parsed = cli::ParseSlashCommand("/Memory remember fact AgentLoop 入口");
    CHECK(parsed.command == cli::SlashCommand::Memory);
    CHECK(parsed.args == "remember fact AgentLoop 入口");
    bool found = false;
    for (const auto& command : cli::AllSlashCommands()) {
        if (command.name == "/memory") found = true;
    }
    CHECK(found);
}

TEST_CASE("ParseSlashCommand: /Todos 大小写不敏感") {
    CHECK(cli::ParseSlashCommand("/Todos").command == cli::SlashCommand::Todos);
}

TEST_CASE("AllSlashCommands: /todos 在列表里") {
    const auto& commands = cli::AllSlashCommands();
    bool has_todos = false;
    for (const auto& c : commands) {
        if (c.name == "/todos") has_todos = true;
    }
    CHECK(has_todos);
}

TEST_CASE("ParseSlashCommand: /plugins(M7)") {
    const auto parsed = cli::ParseSlashCommand("/plugins");
    CHECK(parsed.command == cli::SlashCommand::Plugins);
    CHECK(parsed.args.empty());
    CHECK(cli::ParseSlashCommand("/Plugins").command == cli::SlashCommand::Plugins);
}

TEST_CASE("AllSlashCommands: /plugins 在列表里") {
    const auto& commands = cli::AllSlashCommands();
    bool has_plugins = false;
    for (const auto& c : commands) {
        if (c.name == "/plugins") has_plugins = true;
    }
    CHECK(has_plugins);
}

TEST_CASE("ParseSlashCommand: /lsp,大小写不敏感") {
    CHECK(cli::ParseSlashCommand("/lsp").command == cli::SlashCommand::Lsp);
    CHECK(cli::ParseSlashCommand("/LSP").command == cli::SlashCommand::Lsp);
}

TEST_CASE("AllSlashCommands: /lsp 在列表里") {
    const auto& commands = cli::AllSlashCommands();
    bool has_lsp = false;
    for (const auto& c : commands) {
        if (c.name == "/lsp") has_lsp = true;
    }
    CHECK(has_lsp);
}

TEST_CASE("ParseSlashCommand: /soul 不带参数(0.16.x 魂法分家)") {
    const auto parsed = cli::ParseSlashCommand("/soul");
    CHECK(parsed.command == cli::SlashCommand::Soul);
    CHECK(parsed.args.empty());
}

TEST_CASE("ParseSlashCommand: /soul 带内容、clear 和旧的名字/off/default 参数") {
    CHECK(cli::ParseSlashCommand("/soul 答话短些").args == "答话短些");
    CHECK(cli::ParseSlashCommand("/soul clear").args == "clear");
    CHECK(cli::ParseSlashCommand("/soul wenyan").args == "wenyan");
    CHECK(cli::ParseSlashCommand("/soul wenyan").command == cli::SlashCommand::Soul);
    CHECK(cli::ParseSlashCommand("/soul off").args == "off");
    CHECK(cli::ParseSlashCommand("/soul default").args == "default");
    CHECK(cli::ParseSlashCommand("/Soul wenyan").command == cli::SlashCommand::Soul);
}

TEST_CASE("ParseSlashCommand: /prompt 不带参数和带 reset") {
    const auto bare = cli::ParseSlashCommand("/prompt");
    CHECK(bare.command == cli::SlashCommand::Prompt);
    CHECK(bare.args.empty());
    const auto reset = cli::ParseSlashCommand("/prompt reset");
    CHECK(reset.command == cli::SlashCommand::Prompt);
    CHECK(reset.args == "reset");
    CHECK(cli::ParseSlashCommand("/PROMPT").command == cli::SlashCommand::Prompt);
}

TEST_CASE("AllSlashCommands: /soul //prompt 都在列表里") {
    const auto& commands = cli::AllSlashCommands();
    bool has_soul = false;
    bool has_prompt = false;
    for (const auto& c : commands) {
        if (c.name == "/soul") has_soul = true;
        if (c.name == "/prompt") has_prompt = true;
    }
    CHECK(has_soul);
    CHECK(has_prompt);
}

TEST_CASE("ParseSlashCommand: /tools(tool_search 延迟挂载)") {
    const auto parsed = cli::ParseSlashCommand("/tools");
    CHECK(parsed.command == cli::SlashCommand::Tools);
    CHECK(parsed.args.empty());
    CHECK(cli::ParseSlashCommand("/Tools").command == cli::SlashCommand::Tools);
}

TEST_CASE("AllSlashCommands: /tools 在列表里") {
    const auto& commands = cli::AllSlashCommands();
    bool has_tools = false;
    for (const auto& c : commands) {
        if (c.name == "/tools") has_tools = true;
    }
    CHECK(has_tools);
}

TEST_CASE("ParseSlashCommand: /image 识别为附图命令") {
    const auto parsed = cli::ParseSlashCommand("/image screenshots/err.png");
    CHECK(parsed.command == cli::SlashCommand::Image);
    CHECK(parsed.args == "screenshots/err.png");
}

TEST_CASE("ParseSlashCommand: /worktree 与参数") {
    const auto parsed = cli::ParseSlashCommand("/Worktree new fix_54");
    CHECK(parsed.command == cli::SlashCommand::Worktree);
    CHECK(parsed.args == "new fix_54");
}

TEST_CASE("AllSlashCommands: /worktree 在列表里") {
    bool found = false;
    for (const auto& command : cli::AllSlashCommands()) {
        if (command.name == "/worktree") {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("ParseSlashCommand: /skill 分发命令与参数") {
    const auto install = cli::ParseSlashCommand("/Skill install https://example.test/poem.md");
    CHECK(install.command == cli::SlashCommand::Skill);
    CHECK(install.args == "install https://example.test/poem.md");

    const auto update = cli::ParseSlashCommand("/skill update poem");
    CHECK(update.command == cli::SlashCommand::Skill);
    CHECK(update.args == "update poem");

    bool found = false;
    for (const auto& command : cli::AllSlashCommands()) {
        if (command.name == "/skill") {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("ParseSlashCommand: /provider 大小写不敏感,出现在候选里") {
    const auto parsed = cli::ParseSlashCommand("/Provider list");
    CHECK(parsed.command == cli::SlashCommand::Provider);
    CHECK(parsed.args == "list");

    bool found = false;
    for (const auto& command : cli::AllSlashCommands()) {
        if (command.name == "/provider") {
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("ParseProviderCommand: add 解出必填项与可选项") {
    const auto parsed = cli::ParseProviderCommand(
        "add glm https://open.bigmodel.cn/api/paas/v4 responses --key-env ZAI_API_KEY --model glm-4.5 --window 128k");
    CHECK(parsed.action == cli::ProviderCommandAction::Add);
    CHECK(parsed.name == "glm");
    CHECK(parsed.base_url == "https://open.bigmodel.cn/api/paas/v4");
    CHECK(parsed.wire == "responses");
    CHECK(parsed.key_env == "ZAI_API_KEY");
    CHECK(parsed.model == "glm-4.5");
    CHECK(parsed.window == "128k");
}

TEST_CASE("ParseProviderCommand: list/switch/remove 与错参") {
    CHECK(cli::ParseProviderCommand("").action == cli::ProviderCommandAction::List);
    CHECK(cli::ParseProviderCommand("list").action == cli::ProviderCommandAction::List);
    CHECK(cli::ParseProviderCommand("refresh").action == cli::ProviderCommandAction::Refresh);
    CHECK(cli::ParseProviderCommand("refresh now").action == cli::ProviderCommandAction::Invalid);

    const auto switched = cli::ParseProviderCommand("switch glm glm-4.5-air");
    CHECK(switched.action == cli::ProviderCommandAction::Switch);
    CHECK(switched.name == "glm");
    CHECK(switched.model == "glm-4.5-air");

    // 裸敲 /provider switch:不判 Invalid、不倒总帮助,交给交互层开选择器。
    const auto bare = cli::ParseProviderCommand("switch");
    CHECK(bare.action == cli::ProviderCommandAction::SwitchInteractive);
    // 词数超了(4 个)照旧 Invalid。
    CHECK(cli::ParseProviderCommand("switch a b c").action == cli::ProviderCommandAction::Invalid);

    const auto removed = cli::ParseProviderCommand("remove glm");
    CHECK(removed.action == cli::ProviderCommandAction::Remove);
    CHECK(removed.name == "glm");

    CHECK(cli::ParseProviderCommand("add only-two https://example.test").action ==
          cli::ProviderCommandAction::Invalid);
    CHECK(cli::ParseProviderCommand("add x https://example.test anthropic --model").action ==
          cli::ProviderCommandAction::Invalid);
    CHECK(cli::ParseProviderCommand("switch glm extra unexpected").action ==
          cli::ProviderCommandAction::Invalid);
}

TEST_CASE("CanRemoveProvider: 当前在用端不许删") {
    CHECK_FALSE(cli::CanRemoveProvider("glm", "glm"));
    CHECK(cli::CanRemoveProvider("glm", "minimax"));
    CHECK(cli::CanRemoveProvider("", "glm"));
}

TEST_CASE("ParseProviderCommand: set 拆出名字/字段/值,字段值原样小写化") {
    const auto parsed = cli::ParseProviderCommand("set glm native_web_search on");
    CHECK(parsed.action == cli::ProviderCommandAction::Set);
    CHECK(parsed.name == "glm");
    CHECK(parsed.field == "native_web_search");
    CHECK(parsed.value == "on");

    // 命令词、字段名、值都不区分大小写——解析层只管拆词、转小写,认不认得
    // 字段/值留给 main.cpp 去判断。
    const auto mixed_case = cli::ParseProviderCommand("Set GLM Native_Web_Search OFF");
    CHECK(mixed_case.action == cli::ProviderCommandAction::Set);
    CHECK(mixed_case.name == "GLM");  // 名字本身不转小写,跟 add/switch/remove 一致
    CHECK(mixed_case.field == "native_web_search");
    CHECK(mixed_case.value == "off");
}

TEST_CASE("ParseProviderCommand: set 词数不对(多或少)一律 Invalid") {
    CHECK(cli::ParseProviderCommand("set glm").action == cli::ProviderCommandAction::Invalid);
    CHECK(cli::ParseProviderCommand("set glm native_web_search").action == cli::ProviderCommandAction::Invalid);
    CHECK(cli::ParseProviderCommand("set glm native_web_search on extra").action ==
          cli::ProviderCommandAction::Invalid);
}

TEST_CASE("ParseProviderCommand: set extra_body 取字段名之后到行尾的原始文本,不按空格切词") {
    const auto parsed = cli::ParseProviderCommand(
        R"(set glm extra_body {"thinking":{"type":"enabled"},"reasoning_effort":"max"})");
    CHECK(parsed.action == cli::ProviderCommandAction::Set);
    CHECK(parsed.name == "glm");
    CHECK(parsed.field == "extra_body");
    CHECK(parsed.value == R"({"thinking":{"type":"enabled"},"reasoning_effort":"max"})");
    CHECK(parsed.header_name.empty());

    // 字段名本身不区分大小写,但 JSON 原文的大小写、空格必须原样保留——
    // 拿到手上再去解析合不合法是 main.cpp/config 层的事,这层只管抠原文。
    const auto mixed_case = cli::ParseProviderCommand(R"(set GLM Extra_Body {"A": 1, "b": 2})");
    CHECK(mixed_case.name == "GLM");
    CHECK(mixed_case.field == "extra_body");
    CHECK(mixed_case.value == R"({"A": 1, "b": 2})");
}

TEST_CASE("ParseProviderCommand: set extra_body 清空(空 object 或空文本)也解得出,值原样传下去") {
    const auto cleared_braces = cli::ParseProviderCommand("set glm extra_body {}");
    CHECK(cleared_braces.action == cli::ProviderCommandAction::Set);
    CHECK(cleared_braces.value == "{}");

    // 字段名之后什么都没有——"到行尾的原始文本"就是空串,不该判成词数不够
    // 的 Invalid(这跟 native_web_search 那种固定词数的字段不是一个规矩)。
    const auto cleared_empty = cli::ParseProviderCommand("set glm extra_body");
    CHECK(cleared_empty.action == cli::ProviderCommandAction::Set);
    CHECK(cleared_empty.value.empty());
}

TEST_CASE("ParseProviderCommand: set extra_header 拆出头名字(保留大小写)和值(保留空格)") {
    const auto parsed = cli::ParseProviderCommand("set glm extra_header X-Api-Version 2024-06-01 beta");
    CHECK(parsed.action == cli::ProviderCommandAction::Set);
    CHECK(parsed.name == "glm");
    CHECK(parsed.field == "extra_header");
    CHECK(parsed.header_name == "X-Api-Version");   // 头名字原样大小写,不转小写
    CHECK(parsed.value == "2024-06-01 beta");        // 值可以带空格,原样保留

    // 值留空 = 删除这一条头;头名字之后没有值文本也该解出来,而不是 Invalid。
    const auto deleted = cli::ParseProviderCommand("set glm extra_header X-Api-Version");
    CHECK(deleted.action == cli::ProviderCommandAction::Set);
    CHECK(deleted.header_name == "X-Api-Version");
    CHECK(deleted.value.empty());
}

TEST_CASE("ParseProviderCommand: set extra_header 缺头名字(只给了字段名,没有紧跟的词)是 Invalid") {
    const auto parsed = cli::ParseProviderCommand("set glm extra_header");
    CHECK(parsed.action == cli::ProviderCommandAction::Invalid);
}

// ---------------------------------------------------------------------------
// /provider edit 解析(容错单)
// ---------------------------------------------------------------------------

TEST_CASE("ParseProviderCommand: edit 拆名字,裸敲进 EditInteractive,词数超了 Invalid") {
    const auto edited = cli::ParseProviderCommand("edit custom");
    CHECK(edited.action == cli::ProviderCommandAction::Edit);
    CHECK(edited.name == "custom");

    const auto bare = cli::ParseProviderCommand("edit");
    CHECK(bare.action == cli::ProviderCommandAction::EditInteractive);

    // 名字之后再冒词照旧 Invalid(短用法由容错层给)。
    CHECK(cli::ParseProviderCommand("edit a b").action == cli::ProviderCommandAction::Invalid);
    CHECK(cli::ParseProviderCommand("EDIT custom").action == cli::ProviderCommandAction::Edit);  // 大小写不敏感
}

// ---------------------------------------------------------------------------
// /provider 子命令容错(容错单):编辑距离近邻纯函数,单测钉死
// ---------------------------------------------------------------------------

TEST_CASE("NearestProviderSubcommand: 真机实录的拼错词各归各主") {
    CHECK(cli::NearestProviderSubcommand("swtich") == std::optional<std::string>("switch"));
    CHECK(cli::NearestProviderSubcommand("lst") == std::optional<std::string>("list"));
    CHECK(cli::NearestProviderSubcommand("remvoe") == std::optional<std::string>("remove"));
    CHECK(cli::NearestProviderSubcommand("refesh") == std::optional<std::string>("refresh"));
    CHECK(cli::NearestProviderSubcommand("adds") == std::optional<std::string>("add"));
    CHECK(cli::NearestProviderSubcommand("edti") == std::optional<std::string>("edit"));
    CHECK(cli::NearestProviderSubcommand("se") == std::optional<std::string>("set"));
    // 大小写不敏感;本身就是已知子命令时原样返回(距离 0)。
    CHECK(cli::NearestProviderSubcommand("SWITCH") == std::optional<std::string>("switch"));
    CHECK(cli::NearestProviderSubcommand("Switch") == std::optional<std::string>("switch"));
}

TEST_CASE("NearestProviderSubcommand: 无理词无近邻,超阈值不硬凑") {
    CHECK_FALSE(cli::NearestProviderSubcommand("zzzzzz").has_value());
    CHECK_FALSE(cli::NearestProviderSubcommand("hello-world").has_value());
    CHECK_FALSE(cli::NearestProviderSubcommand("listtess").has_value());
    CHECK_FALSE(cli::NearestProviderSubcommand("").has_value());
    // 距离 3,超过 <=2 的阈值,不给近邻。
    CHECK_FALSE(cli::NearestProviderSubcommand("swtichx").has_value());
}

TEST_CASE("NearestProviderSubcommand: 距离打平取清单序排前面的") {
    // "a" 离 add 2 步,是唯一 <=2 的近邻。
    CHECK(cli::NearestProviderSubcommand("a") == std::optional<std::string>("add"));
    CHECK(cli::NearestProviderSubcommand("st") == std::optional<std::string>("set"));
}

TEST_CASE("NearestProviderSubcommand: 距离 1 优先于距离 2") {
    // "swich" 离 switch 1 步、离 set 远,取 switch。
    CHECK(cli::NearestProviderSubcommand("swich") == std::optional<std::string>("switch"));
}

TEST_CASE("ProviderSubcommandUsageLine: 每个子命令都有短用法,switch 复用既有键") {
    for (const std::string& sub : cli::ProviderSubcommands()) {
        const std::string usage = cli::ProviderSubcommandUsageLine(sub);
        CHECK_FALSE(usage.empty());
        CHECK(usage.find(sub) != std::string::npos);  // 用法里得提到自己
    }
    CHECK(cli::ProviderSubcommandUsageLine("bogus").empty());  // 认不得的不编用法
}

TEST_CASE("ParseProviderCommand: Invalid 时 bad_word 带着第一词原始拼写") {
    CHECK(cli::ParseProviderCommand("swtich custom").bad_word == "swtich");
    CHECK(cli::ParseProviderCommand("Swtich").bad_word == "Swtich");        // 保留大小写
    CHECK(cli::ParseProviderCommand("refresh now").bad_word == "refresh");  // 错参也算
    CHECK(cli::ParseProviderCommand("switch glm").bad_word == "switch");    // 合法路径也带着,不用而已
}

// ---------------------------------------------------------------------------
// AllSlashSubcommandGroups(多级 Tab 补全单):二级子命令补全词表。只登记
// provider/instructions/goal/loop/plan/record 这几个词汇本就固定的命令,
// 词表逐一核对对应 Parse*Command 函数得出。
// ---------------------------------------------------------------------------

TEST_CASE("AllSlashSubcommandGroups: /provider 组与 ProviderSubcommands() 词表一致") {
    const auto& groups = cli::AllSlashSubcommandGroups();
    const auto it = std::find_if(groups.begin(), groups.end(),
                                  [](const auto& g) { return g.command == "/provider"; });
    REQUIRE(it != groups.end());
    const std::vector<std::string> expected = cli::ProviderSubcommands();
    REQUIRE(it->entries.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(it->entries[i].name == expected[i]);
        CHECK_FALSE(it->entries[i].description.empty());
    }
}

TEST_CASE("AllSlashSubcommandGroups: 已登记的命令与子命令词表覆盖 Parse*Command 的字面量") {
    const auto& groups = cli::AllSlashSubcommandGroups();
    auto entries_for = [&](const std::string& command) -> std::vector<std::string> {
        const auto it = std::find_if(groups.begin(), groups.end(),
                                      [&](const auto& g) { return g.command == command; });
        REQUIRE(it != groups.end());
        std::vector<std::string> names;
        for (const auto& e : it->entries) {
            CHECK_FALSE(e.description.empty());  // 每条子命令都得有说明,不是裸词
            names.push_back(e.name);
        }
        return names;
    };

    CHECK(entries_for("/instructions") == std::vector<std::string>{"path", "reload"});
    CHECK(entries_for("/goal") == std::vector<std::string>{"status", "pause", "resume", "clear", "edit"});
    CHECK(entries_for("/loop") ==
          std::vector<std::string>{"list", "status", "pause", "resume", "stop", "run"});
    CHECK(entries_for("/plan") == std::vector<std::string>{"status", "off", "review"});
    CHECK(entries_for("/record") == std::vector<std::string>{"status", "start", "note", "pause", "resume",
                                                               "stop", "cancel", "list", "install", "discard"});
}

TEST_CASE("AllSlashSubcommandGroups: 没有登记二级词表的命令查无(比如 /model)") {
    const auto& groups = cli::AllSlashSubcommandGroups();
    CHECK(std::none_of(groups.begin(), groups.end(), [](const auto& g) { return g.command == "/model"; }));
}

TEST_CASE("BuildSlashSubcommandCompletionCandidates: 与 AllSlashSubcommandGroups 逐条对齐") {
    const auto candidates = cli::BuildSlashSubcommandCompletionCandidates();
    const auto& groups = cli::AllSlashSubcommandGroups();
    REQUIRE(candidates.size() == groups.size());
    for (std::size_t i = 0; i < groups.size(); ++i) {
        CHECK(candidates[i].command == groups[i].command);
        REQUIRE(candidates[i].subcommands.size() == groups[i].entries.size());
        for (std::size_t j = 0; j < groups[i].entries.size(); ++j) {
            CHECK(candidates[i].subcommands[j].name == groups[i].entries[j].name);
            CHECK(candidates[i].subcommands[j].description == groups[i].entries[j].description);
        }
    }
}

// ---------------------------------------------------------------------------
// 三份名单对账(P3-2):--help 与 /help 打 cli::FormatSlashCommandListLines()
// 生成的行,Tab 补全打 BuildSlashCompletionCandidates() 的候选,两者都出
// AllSlashCommands()——命令增减只动一处,名单不许再各列各的。
// ---------------------------------------------------------------------------

TEST_CASE("FormatSlashCommandListLines: 每行一个 / 命令,名字与 AllSlashCommands 逐一对应") {
    const auto lines = cli::FormatSlashCommandListLines();
    const auto& commands = cli::AllSlashCommands();
    REQUIRE(lines.size() == commands.size());
    for (std::size_t i = 0; i < commands.size(); ++i) {
        // 行形:"  /名字<补白>说明"——名字后面至少两个空格,说明非空。
        CHECK_MESSAGE(lines[i].rfind("  " + commands[i].name, 0) == 0, commands[i].name);
        const std::size_t after_name = lines[i].find(commands[i].name) + commands[i].name.size();
        CHECK_MESSAGE(lines[i].compare(after_name, 2, "  ") == 0, commands[i].name);
        CHECK_MESSAGE(lines[i].size() > after_name + 2, commands[i].name);  // 有说明文字
    }
}

TEST_CASE("三份名单一致:帮助行、AllSlashCommands、Tab 补全候选同名同序") {
    const auto lines = cli::FormatSlashCommandListLines();
    std::vector<std::string> from_help;
    from_help.reserve(lines.size());
    for (const std::string& line : lines) {
        const std::size_t start = 2;  // 两格缩进
        const std::size_t end = line.find(' ', start);
        REQUIRE(end != std::string::npos);
        from_help.push_back(line.substr(start, end - start));
    }

    std::vector<std::string> from_table;
    for (const auto& command : cli::AllSlashCommands()) {
        from_table.push_back(command.name);
    }

    // 补全候选:AllSlashCommands 的名字打头;AdditionalSlashCandidatesSlot
    // 缺省为空(别处若注册过动态候选,只核对前 from_table.size() 个)。
    const auto candidates = cli::BuildSlashCompletionCandidates();
    std::vector<std::string> from_completion;
    from_completion.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        from_completion.push_back(candidate.name);
    }

    REQUIRE(from_help.size() == from_table.size());
    for (std::size_t i = 0; i < from_table.size(); ++i) {
        CHECK_MESSAGE(from_help[i] == from_table[i], from_table[i]);
    }
    REQUIRE(from_completion.size() >= from_table.size());
    for (std::size_t i = 0; i < from_table.size(); ++i) {
        CHECK_MESSAGE(from_completion[i] == from_table[i], from_table[i]);
    }

    // P3-2 的原始病灶:顶层 --help 手抄清单漏了 /plan /agents /agent。同源
    // 之后这三枚必须在帮助行里出现——漏一枚,这里红。
    bool has_plan = false, has_agents = false, has_agent = false;
    for (const std::string& name : from_help) {
        has_plan = has_plan || name == "/plan";
        has_agents = has_agents || name == "/agents";
        has_agent = has_agent || name == "/agent";
    }
    CHECK(has_plan);
    CHECK(has_agents);
    CHECK(has_agent);
}

// ---------------------------------------------------------------------------
// 词汇表对账(HC-05):主名、别名、隐藏词、展示规则只在
// SlashCommandDescriptors() 一处。这里把"解析、帮助、补全、保留词全从表
// 派生"折成可数的账——新增命令只添一行表,四处消费面自动跟上。
// ---------------------------------------------------------------------------

TEST_CASE("SlashCommandDescriptors: 词不重复,每枚枚举至多一个主名") {
    const auto& descriptors = cli::SlashCommandDescriptors();
    REQUIRE_FALSE(descriptors.empty());
    std::set<std::string> words;
    std::set<int> primaries;
    for (const auto& descriptor : descriptors) {
        CHECK_MESSAGE(words.insert(descriptor.word).second, descriptor.word);
        if (descriptor.primary) {
            CHECK_MESSAGE(primaries.insert(static_cast<int>(descriptor.command)).second,
                          descriptor.word);
        }
    }
    // 描述键只在展示行上:desc_key 非空必是可展示词汇(主名或单列别名)。
    for (const auto& descriptor : descriptors) {
        if (descriptor.desc_key != nullptr) {
            CHECK(!cli::tr(descriptor.desc_key).empty());
        }
    }
}

TEST_CASE("SlashCommandDescriptors: 表上每个词都解到自己的枚举(解析读表)") {
    for (const auto& descriptor : cli::SlashCommandDescriptors()) {
        const auto parsed = cli::ParseSlashCommand(std::string("/") + descriptor.word);
        CHECK_MESSAGE(parsed.command == descriptor.command, descriptor.word);
        CHECK_MESSAGE(parsed.args.empty(), descriptor.word);
        CHECK_MESSAGE(parsed.alias_word.empty(), descriptor.word);  // 内建词不走 alias 路
    }
}

TEST_CASE("AllSlashReservedWords: 保留词覆盖帮助名单与全部隐藏词/别名") {
    const auto reserved = cli::AllSlashReservedWords();
    std::set<std::string> reserved_set(reserved.begin(), reserved.end());
    // 帮助名单是保留词的子集:展示面只会少,不会多。
    std::set<std::string> help_names;
    for (const auto& command : cli::AllSlashCommands()) {
        help_names.insert(command.name);
        CHECK_MESSAGE(reserved_set.count(command.name) == 1, command.name);
    }
    // 别名与隐藏主命令都必须在保留词里——用户拿这些词起 Workflow alias
    // 会被内建命令压住,冲突检查不能漏。/effort /hooks 帮助面单列,
    // /quit /lang /bg 帮助面不列,/trace 暂不展示,保留词一律要收。
    for (const char* word :
         {"/hooks", "/trace", "/quit", "/lang", "/bg", "/effort"}) {
        CHECK_MESSAGE(reserved_set.count(word) == 1, word);
    }
    // 纯别名不进帮助面。
    for (const char* alias : {"/quit", "/lang", "/bg"}) {
        CHECK_MESSAGE(help_names.count(alias) == 0, alias);
    }
}

TEST_CASE("AllSlashCommands: /hooks 进列表(HC-05 补全修复),描述走 i18n 词条") {
    // HC-05 病灶:/hooks 在分派面有处理器,却因词汇表没这行而不进帮助与
    // Tab 补全。行为提交翻过 desc_key 之后,/help、Tab 候选都能看见它,
    // 与 /help、--help 同一份名单,不另造。
    const auto parsed = cli::ParseSlashCommand("/hooks");
    CHECK(parsed.command == cli::SlashCommand::Hooks);
    const auto& commands = cli::AllSlashCommands();
    bool has_hooks = false;
    for (const auto& c : commands) {
        if (c.name == "/hooks") {
            has_hooks = true;
            CHECK(c.description == cli::tr("slash.desc.hooks"));
            CHECK(!c.description.empty());
        }
    }
    CHECK(has_hooks);
}

TEST_CASE("词汇表现状: /trace 解析认词、能执行,帮助面暂缺") {
    // /trace 与 /hooks 同病(分派面有处理器、帮助面没有),但它的展示修复
    // 不在 HC-05 单内:desc_key 显式留空是它的展示规则,钉住防悄悄变。
    CHECK(cli::ParseSlashCommand("/trace").command == cli::SlashCommand::Trace);
    std::set<std::string> help_names;
    for (const auto& command : cli::AllSlashCommands()) {
        help_names.insert(command.name);
    }
    CHECK(help_names.count("/trace") == 0);
    const auto* trace = cli::FindSlashCommandDescriptor(cli::SlashCommand::Trace);
    REQUIRE(trace != nullptr);
    CHECK(trace->primary);
    CHECK(std::string(trace->word) == "trace");
    CHECK(trace->desc_key == nullptr);  // 显式不展示;要展示须另立行为提交
    // Unknown/NotSlash 不是用户词汇,表上没有。
    CHECK(cli::FindSlashCommandDescriptor(cli::SlashCommand::Unknown) == nullptr);
    CHECK(cli::FindSlashCommandDescriptor(cli::SlashCommand::NotSlash) == nullptr);
}

TEST_CASE("ParseSlashCommand: /doctor 与子命令参数") {
    const auto bare = cli::ParseSlashCommand("/doctor");
    CHECK(bare.command == cli::SlashCommand::Doctor);
    CHECK(bare.args.empty());

    const auto effort = cli::ParseSlashCommand("/doctor effort xhigh");
    CHECK(effort.command == cli::SlashCommand::Doctor);
    CHECK(effort.args == "effort xhigh");

    const auto cache = cli::ParseSlashCommand("/DOCTOR cache probe");
    CHECK(cache.command == cli::SlashCommand::Doctor);  // 大小写不敏感
    CHECK(cache.args == "cache probe");
}

// /instructions(AGENTS.md 作用域单 P1-1):词面认领 + 二级纯解析。
TEST_CASE("ParseSlashCommand: /instructions 认词,二级解析四路分明") {
    const auto bare = cli::ParseSlashCommand("/instructions");
    CHECK(bare.command == cli::SlashCommand::Instructions);
    CHECK(bare.args.empty());

    const auto with_args = cli::ParseSlashCommand("/INSTRUCTIONS path src/a.cpp");
    CHECK(with_args.command == cli::SlashCommand::Instructions);  // 大小写不敏感
    CHECK(with_args.args == "path src/a.cpp");

    using IA = cli::InstructionsCommandAction;
    CHECK(cli::ParseInstructionsCommand("").action == IA::Baseline);
    CHECK(cli::ParseInstructionsCommand("   ").action == IA::Baseline);
    CHECK(cli::ParseInstructionsCommand("reload").action == IA::Reload);
    CHECK(cli::ParseInstructionsCommand("reload now").action == IA::Invalid);  // reload 不带尾巴

    const auto path = cli::ParseInstructionsCommand("path src/parser/token.cpp");
    CHECK(path.action == IA::Path);
    CHECK(path.target == "src/parser/token.cpp");
    // 路径原样保留(含空格的路径也是一条路径,归一是 Resolver 的事)。
    CHECK(cli::ParseInstructionsCommand("path  my dir/AGENTS notes.md").target ==
          "my dir/AGENTS notes.md");
    // path 后面没路径:Invalid,不当基线猜。
    const auto no_target = cli::ParseInstructionsCommand("path  ");
    CHECK(no_target.action == IA::Invalid);

    // 认不得的子词:Invalid,bad_word 记原始拼写。
    const auto bad = cli::ParseInstructionsCommand("Frobnicate x");
    CHECK(bad.action == IA::Invalid);
    CHECK(bad.bad_word == "Frobnicate");
}
