// /evolve 二级参数的纯解析(照 /package 的 ParsePackageCommand 单测路数)。

#include <doctest/doctest.h>

#include <string>

#include "app/commands/evolve_commands.hpp"

namespace {

const char* ActionName(lubancode::app::EvolveCommandAction action) {
    switch (action) {
        case lubancode::app::EvolveCommandAction::Invalid: return "Invalid";
        case lubancode::app::EvolveCommandAction::Status: return "Status";
        case lubancode::app::EvolveCommandAction::List: return "List";
        case lubancode::app::EvolveCommandAction::Show: return "Show";
    }
    return "?";
}

}  // namespace

TEST_CASE("evolve.parse:裸敲与 status") {
    CHECK(lubancode::app::ParseEvolveCommand("").action ==
          lubancode::app::EvolveCommandAction::Status);
    CHECK(lubancode::app::ParseEvolveCommand("   ").action ==
          lubancode::app::EvolveCommandAction::Status);
    const auto status = lubancode::app::ParseEvolveCommand("status");
    CHECK(status.action == lubancode::app::EvolveCommandAction::Status);
    CHECK(status.source_filter.empty());
    // 大小写不敏感;前后空白剥掉。
    CHECK(lubancode::app::ParseEvolveCommand("  STATUS ").action ==
          lubancode::app::EvolveCommandAction::Status);
}

TEST_CASE("evolve.parse:list 与来源过滤") {
    const auto bare = lubancode::app::ParseEvolveCommand("list");
    CHECK(bare.action == lubancode::app::EvolveCommandAction::List);
    CHECK(bare.source_filter.empty());
    for (const char* scope : {"all", "run", "goal", "recording", "tooltrace", "memory"}) {
        const auto parsed = lubancode::app::ParseEvolveCommand(std::string("list ") + scope);
        INFO(scope);
        CHECK(parsed.action == lubancode::app::EvolveCommandAction::List);
        CHECK(parsed.source_filter == scope);
    }
    // 认不得的过滤词:Invalid,bad_word 记原词。
    const auto bad = lubancode::app::ParseEvolveCommand("list nope");
    CHECK(bad.action == lubancode::app::EvolveCommandAction::Invalid);
    CHECK(bad.bad_word == "nope");
}

TEST_CASE("evolve.parse:show 带目标;缺目标 Invalid") {
    const auto show = lubancode::app::ParseEvolveCommand("show obs-abcdef0123456789");
    CHECK(show.action == lubancode::app::EvolveCommandAction::Show);
    CHECK(show.target == "obs-abcdef0123456789");
    const auto missing = lubancode::app::ParseEvolveCommand("show");
    CHECK(missing.action == lubancode::app::EvolveCommandAction::Invalid);
    CHECK(missing.bad_word == "show");
    // 多段目标当一整个 id(观察 id 不含空白,但原样递给 Find,查无自会报)。
    CHECK(lubancode::app::ParseEvolveCommand("show a b").target == "a b");
}

TEST_CASE("evolve.parse:认不得的子命令 Invalid") {
    const auto parsed = lubancode::app::ParseEvolveCommand("propose run-1");
    CHECK(parsed.action == lubancode::app::EvolveCommandAction::Invalid);
    CHECK(parsed.bad_word == "propose");
    CHECK(ActionName(parsed.action) == std::string("Invalid"));  // 全案覆盖(防未用告警)
}
