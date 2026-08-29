// tools/command_safety:auto 档"自动分析"的命令分类器。纯函数,逐条钉
// 规则:白名单各族、危险动词、git 子命令、命令链逐段判、引号状态机、
// 重定向、路径/扩展/大小写归一、探版放行、$env: 前缀、空串、写盘 cmdlet。

#include <doctest/doctest.h>

#include "tools/command_safety.hpp"

using lubancode::tools::ClassifyCommand;
using lubancode::tools::CommandSafety;

namespace {

bool Safe(const std::string& cmd, const std::string& shell = "powershell") {
    return ClassifyCommand(cmd, shell) == CommandSafety::Safe;
}

bool Asks(const std::string& cmd, const std::string& shell = "powershell") {
    return ClassifyCommand(cmd, shell) == CommandSafety::NeedsConfirm;
}

}  // namespace

TEST_CASE("命令分类:通用白名单只读命令放行") {
    CHECK(Safe("ls"));
    CHECK(Safe("dir src", "cmd"));
    CHECK(Safe("cat README.md"));
    CHECK(Safe("type CMakeLists.txt", "cmd"));
    CHECK(Safe("grep -rn TODO src"));
    CHECK(Safe("findstr /s ConfirmMode *.cpp", "cmd"));
    CHECK(Safe("rg ClassifyCommand src"));
    CHECK(Safe("where cmake"));
    CHECK(Safe("echo hello"));
    CHECK(Safe("pwd"));
    CHECK(Safe("whoami"));
    CHECK(Safe("hostname"));
    CHECK(Safe("vol", "cmd"));
}

TEST_CASE("命令分类:PowerShell cmdlet 白名单放行,cmd 语境不认") {
    CHECK(Safe("Get-ChildItem -Recurse src"));
    CHECK(Safe("Get-Content README.md"));
    CHECK(Safe("Select-String -Pattern TODO -Path src\\main.cpp"));
    CHECK(Safe("Test-Path build"));
    CHECK(Safe("Get-Date"));
    CHECK(Safe("Get-Process"));
    CHECK(Safe("Write-Output done"));
    // cmd 语境下没有这些 cmdlet,不认识 = 问。
    CHECK(Asks("Get-ChildItem", "cmd"));
}

TEST_CASE("命令分类:危险动词黑名单直接问") {
    CHECK(Asks("rm -rf build"));
    CHECK(Asks("del /f /q x.txt", "cmd"));
    CHECK(Asks("rmdir /s /q build", "cmd"));
    CHECK(Asks("move a.txt b.txt", "cmd"));
    CHECK(Asks("copy a.txt b.txt", "cmd"));
    CHECK(Asks("robocopy src dst /MIR", "cmd"));
    CHECK(Asks("format d:", "cmd"));
    CHECK(Asks("reg add HKCU\\Software\\x /v y /d z", "cmd"));
    CHECK(Asks("taskkill /im notepad.exe /f", "cmd"));
    CHECK(Asks("shutdown /s /t 0", "cmd"));
    CHECK(Asks("curl https://example.com"));
    CHECK(Asks("wget https://example.com/a.zip"));
    CHECK(Asks("Invoke-WebRequest https://example.com"));
    CHECK(Asks("iex (Get-Content payload.ps1 -Raw)"));
    CHECK(Asks("Remove-Item -Recurse build"));
    CHECK(Asks("New-Item -ItemType File x.txt"));
    CHECK(Asks("Stop-Process -Name notepad"));
    CHECK(Asks("start notepad.exe", "cmd"));
    CHECK(Asks("net user admin p@ss /add", "cmd"));
}

TEST_CASE("命令分类:黑名单压过探版尾巴") {
    // "即便有人把它塞白名单式样":危险首词哪怕只跟 --help 也照问。
    CHECK(Asks("rm --help"));
    CHECK(Asks("curl --version"));
}

TEST_CASE("命令分类:含 sudo 一律问,词中缀不冤枉") {
    CHECK(Asks("sudo ls"));
    CHECK(Asks("echo hi && sudo rm -rf /"));
    CHECK(Asks("git status | sudo tee /etc/x"));
    // "sudoku" 不是 "sudo",按词判,不按子串。
    CHECK(Safe("echo sudoku-solver"));
}

TEST_CASE("命令分类:git 安全子命令放行") {
    CHECK(Safe("git status"));
    CHECK(Safe("git log --oneline -2"));
    CHECK(Safe("git diff HEAD~1"));
    CHECK(Safe("git show HEAD"));
    CHECK(Safe("git branch -a"));
    CHECK(Safe("git remote -v"));
    CHECK(Safe("git ls-files"));
    CHECK(Safe("git --version"));  // 探版通道
}

TEST_CASE("命令分类:git 危险/未知子命令要问") {
    CHECK(Asks("git add -A"));
    CHECK(Asks("git commit -m x"));
    CHECK(Asks("git push origin main"));
    CHECK(Asks("git checkout ."));
    CHECK(Asks("git reset --hard HEAD"));
    CHECK(Asks("git clean -fd"));
    CHECK(Asks("git"));            // 裸 git,保守问
    CHECK(Asks("git -C .. log"));  // 全局选项挡在第二词位,保守问
}

TEST_CASE("命令分类:命令链逐段判,一段危险整条问") {
    CHECK(Safe("git status && git log --oneline -2"));
    CHECK(Safe("cd src; ls"));
    CHECK(Safe("git log --oneline | head"));
    CHECK(Asks("git status && rm -rf build"));
    CHECK(Asks("ls || del x.txt", "cmd"));
    CHECK(Asks("echo a; git push"));
    CHECK(Asks("cat x.txt | Out-File y.txt"));
    // cmd 的单个 & 也是分隔符,同样逐段判。
    CHECK(Asks("echo a & del x.txt", "cmd"));
    CHECK(Safe("echo a & echo b", "cmd"));
}

TEST_CASE("命令分类:引号内的分隔符不拆段") {
    CHECK(Safe("echo \"a && b\""));
    CHECK(Safe("echo \"a; rm x\""));
    CHECK(Safe("echo 'a | b'"));  // powershell 单引号也算引号
    CHECK(Safe("findstr \"a && b\" x.txt", "cmd"));
}

TEST_CASE("命令分类:cmd 的单引号不是引号,保守拆段") {
    // cmd 里 echo 'a && del x' 真会跑 del,不能当引号漏放。
    CHECK(Asks("echo 'a && del x'", "cmd"));
    CHECK(Safe("echo 'a && del x'", "powershell"));
}

TEST_CASE("命令分类:重定向即不安全,引号内的不算") {
    CHECK(Asks("echo hi > out.txt"));
    CHECK(Asks("dir >> log.txt", "cmd"));
    CHECK(Asks("sort < in.txt", "cmd"));
    CHECK(Asks("echo hi>x.txt", "cmd"));   // 贴着写也认得
    CHECK(Asks("git log 2>&1"));           // 保守:2>&1 也当重定向问
    CHECK(Safe("echo \"a > b\""));
    CHECK(Safe("grep \"->\" main.cpp"));
}

TEST_CASE("命令分类:PowerShell 写盘 cmdlet 同罪") {
    CHECK(Asks("Out-File -FilePath x.txt"));
    CHECK(Asks("Get-Content a.txt | Set-Content b.txt"));
    CHECK(Asks("Get-Date | Add-Content log.txt"));
    CHECK(Asks("Get-Process | Tee-Object proc.txt"));
}

TEST_CASE("命令分类:子表达式 $() 不放行") {
    CHECK(Asks("echo $(Remove-Item x)"));
    CHECK(Asks("Write-Output \"$(rm x)\""));  // 双引号里照样执行,得问
}

TEST_CASE("命令分类:PowerShell 脚本块拦——首词再白,{ } 体内是任意代码") {
    // 黑名单原先只查首词:where-object 在白名单,{ del x } 整段溜过去。
    CHECK(Asks("where-object { del x }"));
    CHECK(Asks("Where-Object { Remove-Item -Recurse build }"));
    CHECK(Asks("Get-ChildItem | Where-Object { $_.Length -gt 5 }"));
    CHECK(Asks("git status | where-object { rm -rf build }"));
    CHECK(Asks("Sort-Object { del x }; echo done"));
    // 引号里的 { 不是脚本块(引号状态机),照旧放行。
    CHECK(Safe("echo 'literal { brace'"));
    CHECK(Safe("Select-String -Pattern \"a { b\" src"));
    // 无脚本块的简化写法照放。
    CHECK(Safe("Get-ChildItem | Where-Object Length -gt 100"));
    // cmd 的 { } 没有执行语义,不受这道闸管。
    CHECK(Safe("echo { x }", "cmd"));
}

TEST_CASE("命令分类:路径前缀/扩展/大小写归一后查表") {
    CHECK(Safe("C:\\Windows\\System32\\where.exe cmake"));
    CHECK(Safe("C:\\Windows\\System32\\findstr.exe /s x *.txt", "cmd"));
    CHECK(Safe("LS -la"));
    CHECK(Safe("Git Status"));
    CHECK(Safe("ECHO done", "cmd"));
    CHECK(Asks("C:\\Windows\\System32\\Robocopy.exe a b", "cmd"));  // 路径也躲不开黑名单
    CHECK(Asks("\"C:\\Program Files\\Evil Tool\\evil.exe\" run"));  // 不认识 = 问
}

TEST_CASE("命令分类:探版尾巴放行,混别的参数不放") {
    CHECK(Safe("python --version"));
    CHECK(Safe("node -v"));
    CHECK(Safe("cmake --help"));
    CHECK(Safe("clang-format --version"));
    CHECK(Asks("python script.py --version"));  // 尾巴里混了别的,不算探版
    CHECK(Asks("python"));                       // 裸 python 进 REPL,问
}

TEST_CASE("命令分类:环境变量赋值前缀要问") {
    CHECK(Asks("$env:PATH='C:\\evil;'+$env:PATH"));
    CHECK(Asks("$env:FOO=1; git status"));
    CHECK(Asks("set FOO=1", "cmd"));
    CHECK(Asks("set FOO=1 && echo %FOO%", "cmd"));
}

TEST_CASE("命令分类:空串/纯空白/未知 shell 都问") {
    CHECK(Asks(""));
    CHECK(Asks("   "));
    CHECK(Asks("\n\t ", "cmd"));
    CHECK(Asks("&&"));
    CHECK(Asks("ls", "bash"));  // 不认识的 shell 值,不猜
}

TEST_CASE("命令分类:不认识的首词一律问") {
    CHECK(Asks("some-random-tool run"));
    CHECK(Asks("npm install"));
    CHECK(Asks("cmake --build build"));
    CHECK(Asks("msbuild all.sln", "cmd"));
}

TEST_CASE("git branch/remote 收紧:裸命令与只读旗标放行,可变参数照问") {
    using lubancode::tools::ClassifyCommand;
    using lubancode::tools::CommandSafety;
    CHECK(ClassifyCommand("git branch", "cmd") == CommandSafety::Safe);
    CHECK(ClassifyCommand("git branch -a", "cmd") == CommandSafety::Safe);
    CHECK(ClassifyCommand("git branch --list", "powershell") == CommandSafety::Safe);
    CHECK(ClassifyCommand("git remote -v", "cmd") == CommandSafety::Safe);
    CHECK(ClassifyCommand("git remote show", "cmd") == CommandSafety::Safe);
    CHECK(ClassifyCommand("git branch -D topic", "cmd") == CommandSafety::NeedsConfirm);
    CHECK(ClassifyCommand("git branch newbranch", "cmd") == CommandSafety::NeedsConfirm);
    CHECK(ClassifyCommand("git remote add origin x", "powershell") == CommandSafety::NeedsConfirm);
    CHECK(ClassifyCommand("git remote set-url origin x", "cmd") == CommandSafety::NeedsConfirm);
}

// ---------------------------------------------------------------------------
// 隔离的 git 改道闸(0.27.x)
// ---------------------------------------------------------------------------


// 隔离改道闸的测试路径按平台给:POSIX 上没有盘符,写死 D:/repo 只是个
// 怪相对名,TokenHitsMainRoot 解析回主树认不出(识别器没错,是测试的
// 路径形制挑了平台)。反斜杠与 cmd 变体只在 Windows 有意义。
#ifdef _WIN32
const char* const kIsoMain = "D:/repo";
const char* const kIsoRoom = "D:/repo/.lubancode/worktrees/fix-1";
const char* const kIsoUnrelated = "D:/tmp";
#else
const char* const kIsoMain = "/repo";
const char* const kIsoRoom = "/repo/.lubancode/worktrees/fix-1";
const char* const kIsoUnrelated = "/tmp";
#endif

lubancode::tools::IsolationScope IsoScope() {
    return {"fix-1", kIsoRoom, kIsoMain};
}
std::string IsoCmd(std::string_view a, std::string_view path, std::string_view b = {}) {
    return std::string(a) + std::string(path) + std::string(b);
}

TEST_CASE("隔离改道闸:git -C 指回主 checkout 拦,指房内放行") {
    using lubancode::tools::FindIsolationGitRedirect;
    using lubancode::tools::IsolationScope;
    const IsolationScope scope = IsoScope();

    CHECK(FindIsolationGitRedirect(IsoCmd("git -C ", kIsoMain, " status"), "powershell", scope).has_value());
#ifdef _WIN32
    // 反斜杠路径形制只在 Windows 有意义(POSIX 上 \\ 不是分隔符)
    CHECK(FindIsolationGitRedirect("git -C D:\\repo status", "cmd", scope).has_value());
#endif
    // 从房里往外爬的相对路径也解析得回主树
    CHECK(FindIsolationGitRedirect("git -C ../../.. status", "powershell", scope).has_value());
    // 指自己房里:放行
    CHECK_FALSE(
        FindIsolationGitRedirect(IsoCmd("git -C ", kIsoRoom, " log"), "powershell", scope).has_value());
    // 无关目录:放行
    CHECK_FALSE(FindIsolationGitRedirect(IsoCmd("git -C ", kIsoUnrelated, " status"), "powershell", scope).has_value());
    // 裸 git(在房内跑):放行
    CHECK_FALSE(FindIsolationGitRedirect("git status", "cmd", scope).has_value());
}

TEST_CASE("隔离改道闸:--git-dir/--work-tree 与 GIT_DIR 环境变量拦") {
    using lubancode::tools::FindIsolationGitRedirect;
    using lubancode::tools::IsolationScope;
    const IsolationScope scope = IsoScope();

    CHECK(FindIsolationGitRedirect(IsoCmd("git --git-dir ", std::string(kIsoMain) + "/.git", " status"), "powershell", scope).has_value());
    CHECK(FindIsolationGitRedirect(IsoCmd("git --git-dir=", std::string(kIsoMain) + "/.git", " status"), "powershell", scope).has_value());
    CHECK(FindIsolationGitRedirect(IsoCmd("git --work-tree ", kIsoMain, " status"), "cmd", scope).has_value());
    CHECK(FindIsolationGitRedirect(IsoCmd("git --work-tree=", kIsoMain, " status"), "powershell", scope).has_value());
    CHECK(FindIsolationGitRedirect(IsoCmd("$env:GIT_DIR='", std::string(kIsoMain) + "/.git", "'; git status"), "powershell", scope).has_value());
    // 赋了就是改道,不管值指哪——cmd 的 set 形式换拼接一样验
    CHECK(FindIsolationGitRedirect(IsoCmd("set GIT_WORK_TREE=", kIsoMain, "&& git status"), "cmd", scope).has_value());
    // 指向别处的 git-dir:不归这道闸管
    CHECK_FALSE(FindIsolationGitRedirect(IsoCmd("git --git-dir ", std::string(kIsoUnrelated) + "/x/.git", " status"), "powershell", scope).has_value());
}

TEST_CASE("隔离改道闸:cd 进主树再跑 git 拦,房内 cd 或无 git 放行") {
    using lubancode::tools::FindIsolationGitRedirect;
    using lubancode::tools::IsolationScope;
    const IsolationScope scope = IsoScope();

#ifdef _WIN32
    CHECK(FindIsolationGitRedirect("cd D:\\repo && git status", "cmd", scope).has_value());
#endif
    CHECK(FindIsolationGitRedirect(IsoCmd("Set-Location ", kIsoMain, "; git status"), "powershell", scope).has_value());
#ifdef _WIN32
    CHECK_FALSE(FindIsolationGitRedirect("cd D:\\repo && dir", "cmd", scope).has_value());
#endif
    CHECK_FALSE(FindIsolationGitRedirect(IsoCmd("cd ", kIsoMain, " && dir"), "cmd", scope).has_value());
    CHECK_FALSE(FindIsolationGitRedirect(IsoCmd("git status && cd ", kIsoMain), "cmd", scope).has_value());  // git 在前不算
    CHECK_FALSE(FindIsolationGitRedirect(IsoCmd("cd ", kIsoUnrelated, " && git status"), "powershell", scope).has_value());
}
