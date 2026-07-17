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
