#include "tools/command_safety.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <vector>

// 规则总表(保守为纲,不认识 = NeedsConfirm):
//   1. 命令链拆段:引号外的 && || ; | & 和换行都当分隔符,逐段判,每段都
//      Safe 整条才 Safe。单个 & 也拆——cmd 里它就是"顺序执行"分隔符,
//      powershell 里当它是后台符/调用符,拆了顶多多问一句,不会放走。
//   2. 引号状态机:powershell 单双引号都算引号,里头的分隔符/重定向不作数;
//      cmd 只认双引号——cmd 的单引号不是引号(echo 'a && del x' 在 cmd 里
//      真会跑 del),把它当引号反倒会漏放,这里保守处理。
//   3. 重定向即不安全:段内引号外出现 > >> < 一律 NeedsConfirm(2>&1 也
//      中招,保守不冤);PowerShell 的 Out-File/Set-Content/Add-Content/
//      Tee-Object 出现在段内任何位置同罪。
//   4. 子表达式即不安全:单引号外出现 $( 一律 NeedsConfirm——powershell
//      的 "$(rm x)" 在双引号里照样执行,echo 打头也拦不住它。
//   5. 首词判定:剥前导空白、剥路径前缀取文件名、剥 .exe/.bat/.cmd/.com
//      扩展、小写化,先过黑名单(黑名单压过一切),再看探版
//      (后面只跟 --version/-v/--help 的放行),再查白名单。
//   6. git 特判:首词 git 看第二词,只有 status/log/diff/show/branch/
//      remote/ls-files 安全。
//   7. 环境变量赋值($env:X=... / set X=...)→ NeedsConfirm(能改后续行为)。
//   8. 空串/纯空白 → NeedsConfirm。

namespace lubancode::tools {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

// 简单引号状态机走一遍,把命令按引号外的分隔符拆成段。single_quotes 表示
// 单引号算不算引号(powershell 算,cmd 不算,理由见顶注释)。
std::vector<std::string> SplitSegments(const std::string& command, bool single_quotes) {
    std::vector<std::string> segments;
    std::string current;
    char quote = '\0';  // '\0' = 引号外,否则存着当前引号字符
    for (const char c : command) {
        if (quote != '\0') {
            current.push_back(c);
            if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (c == '"' || (single_quotes && c == '\'')) {
            quote = c;
            current.push_back(c);
            continue;
        }
        if (c == '&' || c == '|' || c == ';' || c == '\n' || c == '\r') {
            // 分隔符:当前段收工。&&/|| 连着的第二个字符走到这儿时 current
            // 是空的,推出去也会在上层被"纯空白段跳过"处理掉。
            segments.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    segments.push_back(current);
    return segments;
}

// 段内引号外有没有重定向字符(> >> <)。
bool HasUnquotedRedirection(const std::string& segment, bool single_quotes) {
    char quote = '\0';
    for (const char c : segment) {
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (c == '"' || (single_quotes && c == '\'')) {
            quote = c;
            continue;
        }
        if (c == '>' || c == '<') {
            return true;
        }
    }
    return false;
}

// 段内"单引号外"有没有 $((powershell 子表达式,双引号里照样执行)。
// cmd 里 $( 是普通文本,也一并拦——顶多多问,不会漏。
bool HasSubexpression(const std::string& segment, bool single_quotes) {
    bool in_single = false;
    char prev = '\0';
    for (const char c : segment) {
        if (single_quotes && c == '\'') {
            in_single = !in_single;
        } else if (!in_single && prev == '$' && c == '(') {
            return true;
        }
        prev = c;
    }
    return false;
}

// 按引号外空白拆词,词身上的引号字符剥掉("C:\a b\git.exe" 是一个词)。
std::vector<std::string> Tokenize(const std::string& segment, bool single_quotes) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_token = false;
    char quote = '\0';
    for (const char c : segment) {
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else {
                current.push_back(c);
            }
            in_token = true;
            continue;
        }
        if (c == '"' || (single_quotes && c == '\'')) {
            quote = c;
            in_token = true;
            continue;
        }
        if (IsSpace(c)) {
            if (in_token) {
                tokens.push_back(current);
                current.clear();
                in_token = false;
            }
            continue;
        }
        current.push_back(c);
        in_token = true;
    }
    if (in_token) {
        tokens.push_back(current);
    }
    return tokens;
}

// 词形归一:剥路径前缀取文件名、剥可执行扩展、小写化。首词查表用,
// sudo/写盘 cmdlet 扫全段的时候也用同一套。
std::string NormalizeWord(const std::string& token) {
    std::string word = token;
    if (const std::size_t pos = word.find_last_of("/\\"); pos != std::string::npos) {
        word = word.substr(pos + 1);
    }
    word = ToLower(std::move(word));
    for (const std::string_view ext : {".exe", ".bat", ".cmd", ".com"}) {
        if (word.size() > ext.size() && word.ends_with(ext)) {
            word.resize(word.size() - ext.size());
            break;
        }
    }
    return word;
}

template <std::size_t N>
bool InList(const std::array<std::string_view, N>& list, std::string_view word) {
    return std::find(list.begin(), list.end(), word) != list.end();
}

// 危险动词黑名单:首词撞上直接问,压过白名单和探版。
constexpr std::array<std::string_view, 37> kDangerFirstWords = {
    "rm",      "del",     "erase",   "rmdir",    "rd",       "move",         "ren",
    "copy",    "xcopy",   "robocopy", "mklink",  "format",   "diskpart",     "reg",
    "sc",      "net",     "netsh",   "taskkill", "shutdown", "curl",         "wget",
    "invoke-webrequest",  "invoke-expression",   "iex",      "start",        "remove-item",
    "move-item",          "copy-item",           "new-item", "rename-item",  "set-itemproperty",
    "stop-process",       "sudo",    "set",      "setx",     "out-file",     "set-content",
};

// PowerShell 写盘 cmdlet:出现在段内任何位置(不止首词)都要问。
constexpr std::array<std::string_view, 4> kWriteCmdlets = {
    "out-file", "set-content", "add-content", "tee-object"};

// 通用白名单(两个 shell 都放行的只读/探查命令)。
constexpr std::array<std::string_view, 21> kGenericSafe = {
    "ls",   "dir",     "cat",  "type",  "head", "tail",     "wc",       "grep", "findstr", "rg",
    "find", "where",   "which", "echo", "pwd",  "cd",       "whoami",   "hostname", "date",
    "time", "ver"};

// PowerShell 专属白名单(cmdlet 只在 powershell 语境下认)。
constexpr std::array<std::string_view, 16> kPowershellSafe = {
    "get-childitem", "get-content",    "get-item",      "get-location", "select-string",
    "test-path",     "get-date",       "get-process",   "measure-object",
    "select-object", "where-object",   "sort-object",   "format-table", "format-list",
    "write-output",  "write-host"};

// cmd 内建白名单(dir/type/echo/cd/time/ver 已在通用表,这里补 cmd 特有的)。
constexpr std::array<std::string_view, 1> kCmdSafe = {"vol"};

// git 安全子命令(只读/查看类;add/commit/push/checkout/reset 这些一律问)。
constexpr std::array<std::string_view, 7> kGitSafeSubcommands = {
    "status", "log", "diff", "show", "branch", "remote", "ls-files"};

// 探版尾巴:首词后面只跟这几个的,放行(python --version 之类)。
constexpr std::array<std::string_view, 3> kProbeFlags = {"--version", "-v", "--help"};

// 单段判定。段已保证不是纯空白。
CommandSafety ClassifySegment(const std::string& segment, bool is_powershell, bool single_quotes) {
    if (HasUnquotedRedirection(segment, single_quotes) || HasSubexpression(segment, single_quotes)) {
        return CommandSafety::NeedsConfirm;
    }

    const std::vector<std::string> tokens = Tokenize(segment, single_quotes);
    if (tokens.empty()) {
        return CommandSafety::NeedsConfirm;
    }

    // 环境变量赋值前缀:$env:X=...(powershell)/ set X=...(cmd,set 已进
    // 黑名单)。$env: 打头的首词不成词形,单独拦。
    if (ToLower(tokens.front()).rfind("$env:", 0) == 0) {
        return CommandSafety::NeedsConfirm;
    }

    const std::string first = NormalizeWord(tokens.front());
    if (first.empty() || InList(kDangerFirstWords, first)) {
        return CommandSafety::NeedsConfirm;
    }

    // sudo 与写盘 cmdlet:扫全段每个词,不止首词。
    for (const std::string& token : tokens) {
        const std::string word = NormalizeWord(token);
        if (word == "sudo" || InList(kWriteCmdlets, word)) {
            return CommandSafety::NeedsConfirm;
        }
    }

    // 探版放行:首词随意(黑名单已在上头拦过),后面只跟 --version/-v/--help。
    if (tokens.size() >= 2) {
        bool all_probe = true;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            if (!InList(kProbeFlags, ToLower(tokens[i]))) {
                all_probe = false;
                break;
            }
        }
        if (all_probe) {
            return CommandSafety::Safe;
        }
    }

    // git 特判:看第二词。裸 git、带全局选项(git -C ...)一律问,保守。
    // branch/remote 两个子命令能改状态(git branch -D、git remote add),
    // 只放裸命令与只读旗标,带其余参数照问。
    if (first == "git") {
        if (tokens.size() >= 2 && InList(kGitSafeSubcommands, ToLower(tokens[1]))) {
            const std::string sub = ToLower(tokens[1]);
            if (sub == "branch" || sub == "remote") {
                static constexpr std::array<std::string_view, 6> kGitReadOnlyFlags = {
                    "-v", "-vv", "-a", "--list", "--all", "show"};
                for (std::size_t i = 2; i < tokens.size(); ++i) {
                    if (!InList(kGitReadOnlyFlags, ToLower(tokens[i]))) {
                        return CommandSafety::NeedsConfirm;
                    }
                }
            }
            return CommandSafety::Safe;
        }
        return CommandSafety::NeedsConfirm;
    }

    if (InList(kGenericSafe, first)) {
        return CommandSafety::Safe;
    }
    if (is_powershell && InList(kPowershellSafe, first)) {
        return CommandSafety::Safe;
    }
    if (!is_powershell && InList(kCmdSafe, first)) {
        return CommandSafety::Safe;
    }
    return CommandSafety::NeedsConfirm;
}

}  // namespace

CommandSafety ClassifyCommand(const std::string& command, const std::string& shell) {
    const bool is_powershell = (shell == "powershell");
    if (!is_powershell && shell != "cmd") {
        return CommandSafety::NeedsConfirm;  // 不认识的 shell,不猜
    }
    const bool single_quotes = is_powershell;  // cmd 不把单引号当引号,见顶注释

    const std::vector<std::string> segments = SplitSegments(command, single_quotes);
    bool any_segment = false;
    for (const std::string& segment : segments) {
        const bool blank = std::all_of(segment.begin(), segment.end(),
                                       [](unsigned char c) { return std::isspace(c) != 0; });
        if (blank) {
            continue;  // && / || 拆出来的空隙、首尾空白段,跳过
        }
        any_segment = true;
        if (ClassifySegment(segment, is_powershell, single_quotes) == CommandSafety::NeedsConfirm) {
            return CommandSafety::NeedsConfirm;
        }
    }
    // 空串/纯空白(一段都没有)→ NeedsConfirm。
    return any_segment ? CommandSafety::Safe : CommandSafety::NeedsConfirm;
}

}  // namespace lubancode::tools
