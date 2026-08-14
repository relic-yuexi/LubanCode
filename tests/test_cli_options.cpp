// ParseCliArgs 参数矩阵:纯函数,不读配置、不探终端、不打印,行为对齐
// 旧 RunCli 内联扫描(参数落点、早退优先次序、缺值报错)。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "app/cli_options.hpp"

using namespace lubancode::app;

namespace {

std::vector<std::string> Args(std::initializer_list<std::string> args) {
    return std::vector<std::string>(args);
}

}  // namespace

TEST_CASE("裸程序名:全部默认,走正常启动") {
    const ParsedCliArgs parsed = ParseCliArgs(Args({"lubancode"}));
    CHECK(parsed.action == CliAction::Proceed);
    CHECK(parsed.options.positional.empty());
    CHECK_FALSE(parsed.options.auto_confirm);
    CHECK_FALSE(parsed.options.print_config);
    CHECK_FALSE(parsed.options.continue_last);
    CHECK(parsed.options.system_prompt_file_arg.empty());
}

TEST_CASE("开关各自落位,互不干扰") {
    const ParsedCliArgs parsed = ParseCliArgs(Args({"lubancode", "--yes", "--config", "--continue"}));
    CHECK(parsed.action == CliAction::Proceed);
    CHECK(parsed.options.auto_confirm);
    CHECK(parsed.options.print_config);
    CHECK(parsed.options.continue_last);
    CHECK(parsed.options.positional.empty());
}

TEST_CASE("位置参数按出现次序空格拼接") {
    const ParsedCliArgs parsed = ParseCliArgs(Args({"lubancode", "帮我", "看看", "这个仓库"}));
    CHECK(parsed.action == CliAction::Proceed);
    CHECK(parsed.options.positional == "帮我 看看 这个仓库");
}

TEST_CASE("早退参数:出现即返回,后扫的不覆盖先扫的") {
    CHECK(ParseCliArgs(Args({"lubancode", "--version"})).action == CliAction::PrintVersion);
    CHECK(ParseCliArgs(Args({"lubancode", "--help"})).action == CliAction::PrintHelp);
    CHECK(ParseCliArgs(Args({"lubancode", "--check-update"})).action == CliAction::CheckUpdate);
    CHECK(ParseCliArgs(Args({"lubancode", "--reset-system-prompt"})).action == CliAction::ResetSystemPrompt);
    // 先 --version 后 --help:头一个生效,跟旧的就地 return 一致。
    CHECK(ParseCliArgs(Args({"lubancode", "--version", "--help"})).action == CliAction::PrintVersion);
    // 早退参数出现在位置参数之后也一样当场退。
    CHECK(ParseCliArgs(Args({"lubancode", "问点啥", "--version"})).action == CliAction::PrintVersion);
}

TEST_CASE("--system-prompt 吃掉下一个参数,缺值报错") {
    const ParsedCliArgs ok = ParseCliArgs(Args({"lubancode", "--system-prompt", "D:/p.md", "问句"}));
    CHECK(ok.action == CliAction::Proceed);
    CHECK(ok.options.system_prompt_file_arg == "D:/p.md");
    CHECK(ok.options.positional == "问句");

    const ParsedCliArgs missing = ParseCliArgs(Args({"lubancode", "--system-prompt"}));
    CHECK(missing.action == CliAction::MissingSystemPromptValue);
}

TEST_CASE("重复开关与重复位置参数按旧语义合并") {
    const ParsedCliArgs parsed = ParseCliArgs(Args({"lubancode", "a", "--yes", "b", "--yes"}));
    CHECK(parsed.options.positional == "a b");
    CHECK(parsed.options.auto_confirm);
}

TEST_CASE("不认识的参数不是错误:并进位置参数") {
    const ParsedCliArgs parsed = ParseCliArgs(Args({"lubancode", "--nonsense", "尾巴"}));
    CHECK(parsed.action == CliAction::Proceed);
    CHECK(parsed.options.positional == "--nonsense 尾巴");
}
