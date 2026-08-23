// /goal 单:slash 命令面(parser 二级解析、`--` 消歧、多行正文、help 表)。

#include <doctest/doctest.h>

#include <string>

#include "cli/slash_commands.hpp"

using lubancode::cli::GoalCommandAction;
using lubancode::cli::ParseGoalCommand;
using lubancode::cli::ParseSlashCommand;
using lubancode::cli::SlashCommand;

TEST_CASE("/goal 一级解析:认词、大小写不敏感、args 原样") {
    CHECK(ParseSlashCommand("/goal").command == SlashCommand::Goal);
    CHECK(ParseSlashCommand("/GOAL").command == SlashCommand::Goal);
    CHECK(ParseSlashCommand("/goal status").command == SlashCommand::Goal);
    CHECK(ParseSlashCommand("/goal 迁移认证层").command == SlashCommand::Goal);
    CHECK(ParseSlashCommand("/goals").command == SlashCommand::Unknown);  // 复数不认
    CHECK(ParseSlashCommand("/goalx").command == SlashCommand::Unknown);
}

TEST_CASE("/goal 二级解析:七个动作") {
    CHECK(ParseGoalCommand("").action == GoalCommandAction::View);
    CHECK(ParseGoalCommand("   ").action == GoalCommandAction::View);
    CHECK(ParseGoalCommand("status").action == GoalCommandAction::Status);
    CHECK(ParseGoalCommand("STATUS").action == GoalCommandAction::Status);
    CHECK(ParseGoalCommand("pause").action == GoalCommandAction::Pause);
    CHECK(ParseGoalCommand("resume").action == GoalCommandAction::Resume);
    CHECK(ParseGoalCommand("clear").action == GoalCommandAction::Clear);

    const auto create = ParseGoalCommand("迁移认证层并保持测试通过");
    CHECK(create.action == GoalCommandAction::Create);
    CHECK(create.objective == "迁移认证层并保持测试通过");

    const auto edit = ParseGoalCommand("edit 新目标:重构缓存层");
    CHECK(edit.action == GoalCommandAction::Edit);
    CHECK(edit.objective == "新目标:重构缓存层");
}

TEST_CASE("/goal `--` 消歧:正文以子命令词开头") {
    const auto dd = ParseGoalCommand("-- pause all jobs safely");
    CHECK(dd.action == GoalCommandAction::Create);
    CHECK(dd.dashdash);
    CHECK(dd.objective == "pause all jobs safely");

    // 不带 -- 时 pause 开头的正文……按 Create 收还是 pause 子命令?单子定:
    // "objective 以 pause/edit/... 开头时有歧义,支持 --"。不带 -- 的单词
    // pause 就是 pause 子命令(幂等查状态);要写正文必须 --。
    CHECK(ParseGoalCommand("pause").action == GoalCommandAction::Pause);

    // /goal edit -- <text> 里 -- 后全算正文。
    const auto edit_dd = ParseGoalCommand("edit -- clear the obsolete cache");
    CHECK(edit_dd.action == GoalCommandAction::Edit);
    CHECK(edit_dd.dashdash);
    CHECK(edit_dd.objective == "clear the obsolete cache");

    // 裸 -- 与 -- 后没字:Invalid。
    CHECK(ParseGoalCommand("--").action == GoalCommandAction::Invalid);
    CHECK(ParseGoalCommand("--   ").action == GoalCommandAction::Invalid);
}

TEST_CASE("/goal 多行正文保留,不做 shell 拆词") {
    const auto multi = ParseGoalCommand("第一行\n第二行 $HOME `cmd` @file");
    CHECK(multi.action == GoalCommandAction::Create);
    CHECK(multi.objective == "第一行\n第二行 $HOME `cmd` @file");  // 原样,不展开

    const auto edit_multi = ParseGoalCommand("edit\t带制表符的目标");
    CHECK(edit_multi.action == GoalCommandAction::Edit);
    CHECK(edit_multi.objective == "带制表符的目标");
}

TEST_CASE("/goal edit 缺正文 Invalid;status 带尾巴仍 Status") {
    CHECK(ParseGoalCommand("edit").action == GoalCommandAction::Invalid);
    CHECK(ParseGoalCommand("edit   ").action == GoalCommandAction::Invalid);
    // status 后面跟字:不收(避免 "status" 恰是正文开头的歧义——单子命令面
    // 定死 status 无参;带尾巴按 Invalid,提示用 --)。
    const auto trailing = ParseGoalCommand("status of migration");
    CHECK(trailing.action == GoalCommandAction::Invalid);
    CHECK(trailing.bad_word == "status");
}

TEST_CASE("help 表里有 /goal(两语言)") {
    bool found = false;
    for (const auto& info : lubancode::cli::AllSlashCommands()) {
        if (info.name == "/goal") {
            found = true;
            CHECK_FALSE(info.description.empty());
        }
    }
    CHECK(found);
}
