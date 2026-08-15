// ParseSlashCommand:纯函数,输入串 -> 命令枚举 + 参数,不碰任何 IO。

#include <doctest/doctest.h>

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
