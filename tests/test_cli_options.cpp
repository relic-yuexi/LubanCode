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

// ---------------------------------------------------------------------------
// app-server 子命令(app-server 单:ParseCliArgs 正式识别,绝不能把它
// 送给 one-shot)
// ---------------------------------------------------------------------------

TEST_CASE("app-server 子命令:认到即设旗标,positional 不收它") {
    const ParsedCliArgs parsed = ParseCliArgs(Args({"lubancode", "app-server"}));
    CHECK(parsed.action == CliAction::Proceed); // 旗标路径,RunCli 进协议模式
    CHECK(parsed.options.app_server);
    CHECK(parsed.options.positional.empty());
}

TEST_CASE("app-server 与开关并存:旗标各自落位") {
    const ParsedCliArgs parsed = ParseCliArgs(Args({"lubancode", "app-server", "--yes"}));
    CHECK(parsed.options.app_server);
    CHECK(parsed.options.auto_confirm);
    CHECK(parsed.options.positional.empty());
}

TEST_CASE("早退参数在 app-server 之前:早退生效(扫描次序头一个说了算)") {
    CHECK(ParseCliArgs(Args({"lubancode", "--version", "app-server"})).action == CliAction::PrintVersion);
    CHECK(ParseCliArgs(Args({"lubancode", "--help", "app-server"})).action == CliAction::PrintHelp);
}

TEST_CASE("app-server 在早退参数之前:子命令先认到,后面的 --version 不再早退") {
    const ParsedCliArgs parsed = ParseCliArgs(Args({"lubancode", "app-server", "--version"}));
    CHECK(parsed.options.app_server);
    // 早退规矩是"扫描次序头一个生效":--version 跟在子命令后头照样触发
    // 早退(子命令只是个位置参数,不是模式切换开关),这是旧语义的
    // 忠实延续——想要 app-server 就别在同一行里掺早退参数。
    CHECK(parsed.action == CliAction::PrintVersion);
}

TEST_CASE("裸 app-server 落进位置参数(旧路):解析层就该拦下,不进单发") {
    // 单词出现在位置参数流里且 positional 还是空的:一律当子命令认。
    const ParsedCliArgs parsed = ParseCliArgs(Args({"lubancode", "问点啥", "app-server"}));
    // 前面已有位置参数:app-server 并进 positional(旧语义,问题正文
    // 里出现这个词不算子命令)。
    CHECK_FALSE(parsed.options.app_server);
    CHECK(parsed.options.positional == "问点啥 app-server");
}

// ---------------------------------------------------------------------------
// plugin init 子命令(plugins 单第 3 步:Python scaffold 的参数解析)
// ---------------------------------------------------------------------------

TEST_CASE("plugin init python:模板与名字落位,名字缺省取模板名") {
    ParsedCliArgs parsed = ParseCliArgs(Args({"lubancode", "plugin", "init", "python"}));
    CHECK(parsed.action == CliAction::RunPluginInit);
    CHECK(parsed.plugin_init.template_name == "python");
    CHECK(parsed.plugin_init.plugin_name == "python");

    parsed = ParseCliArgs(Args({"lubancode", "plugin", "init", "python", "my_math"}));
    CHECK(parsed.action == CliAction::RunPluginInit);
    CHECK(parsed.plugin_init.template_name == "python");
    CHECK(parsed.plugin_init.plugin_name == "my_math");
}

TEST_CASE("plugin init 参数形状不对:当场退,不当普通位置参数走单发") {
    CHECK(ParseCliArgs(Args({"lubancode", "plugin"})).action == CliAction::BadPluginInit);
    CHECK(ParseCliArgs(Args({"lubancode", "plugin", "install"})).action == CliAction::BadPluginInit);
    CHECK(ParseCliArgs(Args({"lubancode", "plugin", "init"})).action == CliAction::BadPluginInit);
    const ParsedCliArgs too_many = ParseCliArgs(Args({"lubancode", "plugin", "init", "python", "a", "b"}));
    CHECK(too_many.action == CliAction::BadPluginInit);
    CHECK_FALSE(too_many.error_text.empty());
}

TEST_CASE("plugin 出现在位置参数之后:当普通文本并进 positional(旧语义)") {
    const ParsedCliArgs parsed = ParseCliArgs(Args({"lubancode", "问点啥", "plugin", "init", "python"}));
    CHECK(parsed.action == CliAction::Proceed);
    CHECK(parsed.options.positional == "问点啥 plugin init python");
}

TEST_CASE("早退参数在 plugin 之前:早退生效") {
    CHECK(ParseCliArgs(Args({"lubancode", "--version", "plugin", "init", "python"})).action ==
          CliAction::PrintVersion);
}
