// tools/shell_lexing:审批闸(command_safety)与 Plan 只读闸(plan_mode)
// 共用的无策略 shell 词法(AR-07 从两处重复实现合并而来)。这里钉词法
// 事实本身:拆段/空段/引号状态机/重定向/子表达式/脚本块/拆词/带路径
// exe 归一。判词(白名单/黑名单)的册见 test_command_safety.cpp 与
// test_plan_mode.cpp——两道门的策略差异在那边各有单独断言,不在此处。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "tools/shell_lexing.hpp"

using lubancode::tools::HasUnquotedRedirection;
using lubancode::tools::HasUnquotedScriptBlock;
using lubancode::tools::HasSubexpression;
using lubancode::tools::NormalizeWord;
using lubancode::tools::SplitSegments;
using lubancode::tools::ToLowerWord;
using lubancode::tools::Tokenize;

TEST_CASE("shell_lexing: 拆段——链分隔符逐个拆,引号内的不拆") {
    const std::vector<std::string> segs = SplitSegments("git status && git log", true);
    REQUIRE(segs.size() == 3);
    CHECK(segs[0] == "git status ");
    CHECK(segs[1] == "");  // && 连写中缝的空段,消费方按纯空白段跳过
    CHECK(segs[2] == " git log");

    // 单个分隔符各拆一刀:; | & 换行。
    CHECK(SplitSegments("a; b", true).size() == 2);
    CHECK(SplitSegments("a | b", true).size() == 2);
    CHECK(SplitSegments("echo a & echo b", true).size() == 2);
    CHECK(SplitSegments("git status\ngit log", true).size() == 2);
    CHECK(SplitSegments("git status\r\nls", true).size() == 3);  // \r 与 \n 各算一个

    // 引号里的分隔符不作数(引号状态机)。
    CHECK(SplitSegments("echo \"a && b\"", true).size() == 1);
    CHECK(SplitSegments("echo 'a | b'", true).size() == 1);  // powershell 单引号算引号

    // cmd 不认单引号(single_quotes=false):照样拆。
    const std::vector<std::string> cmd = SplitSegments("echo 'a | b'", false);
    REQUIRE(cmd.size() == 2);
    CHECK(cmd[0] == "echo 'a ");
    CHECK(cmd[1] == " b'");
}

TEST_CASE("shell_lexing: 重定向——引号外 > >> < 都认,引号内的不算") {
    CHECK(HasUnquotedRedirection("echo hi > out.txt", true));
    CHECK(HasUnquotedRedirection("dir >> log.txt", true));
    CHECK(HasUnquotedRedirection("sort < in.txt", true));
    CHECK(HasUnquotedRedirection("echo hi>x.txt", true));  // 贴着写也认得
    CHECK(HasUnquotedRedirection("git log 2>&1", true));   // 2>&1 也当重定向
    CHECK_FALSE(HasUnquotedRedirection("echo \"a > b\"", true));
    CHECK_FALSE(HasUnquotedRedirection("grep \"->\" main.cpp", true));
    CHECK_FALSE(HasUnquotedRedirection("rg pattern src", true));
}

TEST_CASE("shell_lexing: 子表达式——单引号外的 $( 认,双引号里照样算") {
    CHECK(HasSubexpression("echo $(rm x)", true));
    CHECK(HasSubexpression("Write-Output \"$(rm x)\"", true));  // powershell 双引号里也执行
    CHECK_FALSE(HasSubexpression("'$(rm x)'", true));          // 单引号里是字面量
    CHECK_FALSE(HasSubexpression("echo $x", true));            // 光 $ 不成
    // cmd 不认单引号:single_quotes=false 时 '$(x)' 挡不住。
    CHECK(HasSubexpression("'$(rm x)'", false));
}

TEST_CASE("shell_lexing: 脚本块——引号外的 { 认,引号里的不算") {
    CHECK(HasUnquotedScriptBlock("where-object { del x }", true));
    CHECK(HasUnquotedScriptBlock("{ rm -rf build }", true));
    CHECK_FALSE(HasUnquotedScriptBlock("echo 'literal { brace'", true));
    CHECK_FALSE(HasUnquotedScriptBlock("Select-String -Pattern \"a { b\"", true));
    CHECK_FALSE(HasUnquotedScriptBlock("Get-ChildItem | Where-Object Length -gt 100", true));  // 无块简化写法
    // cmd 不认单引号:'{' 照样算(调用方按 shell 分流,词法只报事实)。
    CHECK(HasUnquotedScriptBlock("echo '{'", false));
}

TEST_CASE("shell_lexing: 拆词——词身上的引号剥掉,引号内空白不拆") {
    const std::vector<std::string> quoted = Tokenize("\"C:\\a b\\git.exe\" status", true);
    REQUIRE(quoted.size() == 2);
    CHECK(quoted[0] == "C:\\a b\\git.exe");  // 引号内的空白不拆词,引号字符剥掉
    CHECK(quoted[1] == "status");

    // 连续空白不产空词,首尾空白不吃进去。
    const std::vector<std::string> spaced = Tokenize("  rg   -n  x  ", true);
    REQUIRE(spaced.size() == 3);
    CHECK(spaced[0] == "rg");
    CHECK(spaced[1] == "-n");
    CHECK(spaced[2] == "x");

    CHECK(Tokenize("   ", true).empty());  // 纯空白 → 零词
    // 单引号:powershell 算引号(剥掉、护空白),cmd 不算(普通字符)。
    const std::vector<std::string> sq = Tokenize("echo 'a b'", true);
    REQUIRE(sq.size() == 2);
    CHECK(sq[1] == "a b");
    const std::vector<std::string> cmd = Tokenize("echo 'a b'", false);
    REQUIRE(cmd.size() == 3);
    CHECK(cmd[1] == "'a");
    CHECK(cmd[2] == "b'");
}

TEST_CASE("shell_lexing: 带路径 exe 归一——剥路径、剥扩展、小写化") {
    CHECK(NormalizeWord("C:\\Windows\\System32\\where.exe") == "where");
    CHECK(NormalizeWord("/usr/bin/Git") == "git");  // POSIX 路径也认,大小写归一
    CHECK(NormalizeWord("LS") == "ls");
    CHECK(NormalizeWord("x.bat") == "x");
    CHECK(NormalizeWord("build.cmd") == "build");
    CHECK(NormalizeWord("a.com") == "a");
    CHECK(NormalizeWord("git") == "git");   // 无路径无扩展不动
    CHECK(NormalizeWord("cmd") == "cmd");   // 裸 cmd 不撞 .cmd 扩展
    CHECK(NormalizeWord("git.exe") == "git");
    // ToLowerWord 是词形归一的共用辅件,查表前的小写化同一份。
    CHECK(ToLowerWord("Git-Status") == "git-status");
}
