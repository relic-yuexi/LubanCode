// /loop 单第 2 期:slash 命令面(一级认词、二级解析、`--` 消歧、interval
// 形状判别、子命令与 prompt 的边界)。

#include <doctest/doctest.h>

#include <string>

#include "cli/slash_commands.hpp"

using lubancode::cli::LoopCommandAction;
using lubancode::cli::ParseLoopCommand;
using lubancode::cli::ParseSlashCommand;
using lubancode::cli::SlashCommand;

TEST_CASE("/loop 一级解析:认词、大小写不敏感、args 原样") {
    CHECK(ParseSlashCommand("/loop").command == SlashCommand::Loop);
    CHECK(ParseSlashCommand("/LOOP").command == SlashCommand::Loop);
    CHECK(ParseSlashCommand("/loop 5m check CI").command == SlashCommand::Loop);
    CHECK(ParseSlashCommand("/loop list").command == SlashCommand::Loop);
    CHECK(ParseSlashCommand("/loops").command == SlashCommand::Unknown);  // 复数不认
    CHECK(ParseSlashCommand("/loopx").command == SlashCommand::Unknown);
}

TEST_CASE("create 路:interval + prompt、默认间隔、裸敲") {
    // interval + prompt。
    auto c = ParseLoopCommand("5m check whether CI is green");
    CHECK(c.action == LoopCommandAction::Create);
    CHECK(c.interval_text == "5m");
    CHECK(c.prompt == "check whether CI is green");
    // 大小写。
    c = ParseLoopCommand("2H 看一眼日志");
    CHECK(c.action == LoopCommandAction::Create);
    CHECK(c.interval_text == "2H");
    CHECK(c.prompt == "看一眼日志");
    // 只有 interval:走 loop.md/内置。
    c = ParseLoopCommand("15m");
    CHECK(c.action == LoopCommandAction::Create);
    CHECK(c.interval_text == "15m");
    CHECK(c.prompt.empty());
    // 裸敲:默认 10m + loop.md/内置。
    c = ParseLoopCommand("");
    CHECK(c.action == LoopCommandAction::Create);
    CHECK(c.interval_text.empty());
    CHECK(c.prompt.empty());
    // 没 interval 的正文:默认间隔。
    c = ParseLoopCommand("盯住部署,红了就报");
    CHECK(c.action == LoopCommandAction::Create);
    CHECK(c.interval_text.empty());
    CHECK(c.prompt == "盯住部署,红了就报");
    // 多行正文保留。
    c = ParseLoopCommand("5m 第一行\n第二行");
    CHECK(c.action == LoopCommandAction::Create);
    CHECK(c.prompt == "第一行\n第二行");
}

TEST_CASE("interval 歪形状:明报不静默") {
    // 0m/8d/1h30m/30s/超大:Invalid + 人话提示。
    for (const std::string& bad : {"0m", "8d", "1h30m", "30s", "999999999999m", "1.5h", "-5m"}) {
        const auto c = ParseLoopCommand(bad + " check");
        CHECK(c.action == LoopCommandAction::Invalid);
        CHECK_FALSE(c.error_hint.empty());
    }
    // "5migrate" 是普通词,不当 interval,整句当 prompt。
    const auto word = ParseLoopCommand("5migrate the database now");
    CHECK(word.action == LoopCommandAction::Create);
    CHECK(word.interval_text.empty());
    CHECK(word.prompt == "5migrate the database now");
}

TEST_CASE("子命令:list/status/pause/resume/stop/run") {
    CHECK(ParseLoopCommand("list").action == LoopCommandAction::List);
    CHECK(ParseLoopCommand("status loop-3").action == LoopCommandAction::Status);
    CHECK(ParseLoopCommand("status 3").action == LoopCommandAction::Status);
    CHECK(ParseLoopCommand("status all").action == LoopCommandAction::Status);
    CHECK(ParseLoopCommand("pause loop-3").action == LoopCommandAction::Pause);
    CHECK(ParseLoopCommand("resume all").action == LoopCommandAction::Resume);
    CHECK(ParseLoopCommand("stop 3").action == LoopCommandAction::Stop);
    CHECK(ParseLoopCommand("run loop-3").action == LoopCommandAction::Run);

    auto s = ParseLoopCommand("status loop-3");
    CHECK(s.task_ref == "loop-3");
    s = ParseLoopCommand("pause all");
    CHECK(s.task_ref == "all");
    // 大小写不敏感的子命令。
    CHECK(ParseLoopCommand("LIST").action == LoopCommandAction::List);
    CHECK(ParseLoopCommand("Pause 3").action == LoopCommandAction::Pause);
}

TEST_CASE("子命令参数毛病:缺目标/多词/list 带参") {
    CHECK(ParseLoopCommand("status").action == LoopCommandAction::Invalid);
    CHECK(ParseLoopCommand("pause").action == LoopCommandAction::Invalid);
    CHECK(ParseLoopCommand("stop").action == LoopCommandAction::Invalid);
    CHECK(ParseLoopCommand("run").action == LoopCommandAction::Invalid);
    // list 不带参。
    auto c = ParseLoopCommand("list extra");
    CHECK(c.action == LoopCommandAction::Invalid);
    // 目标只收一个词:多词当 Invalid(防把 prompt 误吞成 id)。
    c = ParseLoopCommand("status loop-3 now");
    CHECK(c.action == LoopCommandAction::Invalid);
}

TEST_CASE("`--` 消歧:正文以子命令词开头") {
    // 不带 interval。
    auto c = ParseLoopCommand("-- stop deployment if red");
    CHECK(c.action == LoopCommandAction::Create);
    CHECK(c.dashdash);
    CHECK(c.prompt == "stop deployment if red");
    CHECK(c.interval_text.empty());
    // 带 interval。
    c = ParseLoopCommand("5m -- list open bugs");
    CHECK(c.action == LoopCommandAction::Create);
    CHECK(c.dashdash);
    CHECK(c.interval_text == "5m");
    CHECK(c.prompt == "list open bugs");
    // 裸 -- 与 -- 后没字:Invalid。
    CHECK(ParseLoopCommand("--").action == LoopCommandAction::Invalid);
    CHECK(ParseLoopCommand("--   ").action == LoopCommandAction::Invalid);
    CHECK(ParseLoopCommand("5m --").action == LoopCommandAction::Invalid);
    // 不带 -- 时单词 stop 就是子命令。
    CHECK(ParseLoopCommand("stop").action == LoopCommandAction::Invalid);  // 缺目标
}

TEST_CASE("inline prompt 以 / 开头:parser 放行,会话层拒") {
    // 单子:"inline prompt 以 / 开头时拒绝,提示改写成自然语言"——这道闸
    // 在会话层(feature/schedulable 边界),parser 只拆词。这里钉住 parser
    // 的行为:不当子命令、原样收。
    const auto c = ParseLoopCommand("20m /review-pr 1234");
    CHECK(c.action == LoopCommandAction::Create);
    CHECK(c.interval_text == "20m");
    CHECK(c.prompt == "/review-pr 1234");
}

TEST_CASE("help 表:slash.desc.loop 已进 i18n") {
    bool found = false;
    for (const auto& info : lubancode::cli::AllSlashCommands()) {
        if (info.name == "/loop") {
            found = true;
            CHECK_FALSE(info.description.empty());
        }
    }
    CHECK(found);
}
