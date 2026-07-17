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
