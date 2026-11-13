// --time-budget / LUBANCODE_TIME_BUDGET_SECS(one-shot 墙钟预算单):解析层
// 只认正整数秒,缺值/认不得当场退;定稿选项优先,环境变量值由调用方递进来,
// 认不得就当没给并写明。
#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

#include "app/cli_options.hpp"

using namespace lubancode::app;

namespace {

std::vector<std::string> Args(std::initializer_list<std::string> args) {
    return std::vector<std::string>(args);
}

}  // namespace

TEST_CASE("ParseTimeBudgetSecs:只认 1~7 天的十进制正整数") {
    CHECK(ParseTimeBudgetSecs("900") == std::optional<int>(900));
    CHECK(ParseTimeBudgetSecs(" 60\n") == std::optional<int>(60));
    CHECK(ParseTimeBudgetSecs("1") == std::optional<int>(1));
    CHECK(ParseTimeBudgetSecs(std::to_string(kMaxTimeBudgetSecs)) == std::optional<int>(kMaxTimeBudgetSecs));
    CHECK_FALSE(ParseTimeBudgetSecs(std::to_string(kMaxTimeBudgetSecs + 1)).has_value());
    CHECK_FALSE(ParseTimeBudgetSecs("0").has_value());
    CHECK_FALSE(ParseTimeBudgetSecs("-5").has_value());
    CHECK_FALSE(ParseTimeBudgetSecs("1.5").has_value());
    CHECK_FALSE(ParseTimeBudgetSecs("900s").has_value());
    CHECK_FALSE(ParseTimeBudgetSecs("").has_value());
    CHECK_FALSE(ParseTimeBudgetSecs("99999999999999").has_value());
}

TEST_CASE("--time-budget 吃掉下一个参数,缺值/认不得退 BadTimeBudget") {
    const ParsedCliArgs ok =
        ParseCliArgs(Args({"lubancode", "--yes", "--time-budget", "900", "--output", "o.jsonl", "干点活"}));
    CHECK(ok.action == CliAction::Proceed);
    CHECK(ok.options.time_budget_given);
    CHECK(ok.options.time_budget_secs == 900);
    CHECK(ok.options.positional == "干点活");
    CHECK(ok.options.output_given);

    CHECK(ParseCliArgs(Args({"lubancode", "--time-budget"})).action == CliAction::BadTimeBudget);
    CHECK(ParseCliArgs(Args({"lubancode", "--time-budget", "abc", "问句"})).action == CliAction::BadTimeBudget);
    CHECK(ParseCliArgs(Args({"lubancode", "--time-budget", "0", "问句"})).action == CliAction::BadTimeBudget);
    const ParsedCliArgs bad = ParseCliArgs(Args({"lubancode", "--time-budget", "", "问句"}));
    CHECK(bad.action == CliAction::BadTimeBudget);
    CHECK(bad.error_text.find("--time-budget") != std::string::npos);
}

TEST_CASE("--time-budget 混进交互模式:解析层照收,模式错配由 RunCli 门卫报") {
    const ParsedCliArgs interactive = ParseCliArgs(Args({"lubancode", "--time-budget", "60"}));
    CHECK(interactive.action == CliAction::Proceed);
    CHECK(interactive.options.time_budget_given);
    CHECK(interactive.options.positional.empty());
}

TEST_CASE("预算定稿:选项优先于环境变量") {
    CliOptions with_flag;
    with_flag.time_budget_given = true;
    with_flag.time_budget_secs = 300;
    const TimeBudgetResolution flag_wins = ResolveTimeBudgetSecs(with_flag, std::string("900"));
    CHECK(flag_wins.secs == std::optional<int>(300));
    CHECK(flag_wins.warning.empty());
    // 选项给了,环境变量哪怕是坏值也不看、不告警。
    CHECK(ResolveTimeBudgetSecs(with_flag, std::string("garbage")).warning.empty());
}

TEST_CASE("预算定稿:没选项看环境变量;没设/空串 = 没预算;坏值忽略并告警") {
    const CliOptions none;
    CHECK(ResolveTimeBudgetSecs(none, std::string("900")).secs == std::optional<int>(900));
    CHECK_FALSE(ResolveTimeBudgetSecs(none, std::nullopt).secs.has_value());
    CHECK_FALSE(ResolveTimeBudgetSecs(none, std::string()).secs.has_value());
    CHECK(ResolveTimeBudgetSecs(none, std::string()).warning.empty());
    const TimeBudgetResolution bad = ResolveTimeBudgetSecs(none, std::string("15m"));
    CHECK_FALSE(bad.secs.has_value());
    CHECK(bad.warning.find("LUBANCODE_TIME_BUDGET_SECS") != std::string::npos);
}
