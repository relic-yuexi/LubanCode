// plan_mode.hpp 的实现:全部纯函数,零 IO、零终端依赖。

#include "runtime/plan_mode.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <vector>

#include "tools/command_safety.hpp"  // HasUnquotedScriptBlock:与 command_safety 共用同一份

namespace lubancode::runtime {

namespace {

constexpr std::string_view kOpenTag = "<proposed_plan>";
constexpr std::string_view kCloseTag = "</proposed_plan>";

// 引号外的空白剥掉(命令串归一用)。
std::string TrimAscii(std::string s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---------------------------------------------------------------------------
// Plan shell 分类(与 command_safety 的 Safe 不同表,单子明令另写)
// ---------------------------------------------------------------------------

// 引号状态机拆段:引号外的 && || ; | & 换行都是分隔符。single_quotes:
// powershell 单引号算引号,cmd 不算(与 command_safety 同取舍)。
std::vector<std::string> SplitShellSegments(const std::string& command, bool single_quotes) {
    std::vector<std::string> segments;
    std::string current;
    char quote = '\0';
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
            segments.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    segments.push_back(current);
    return segments;
}

// 引号外有没有重定向(> >> <)——Plan 一律不认写盘。
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

// 引号外有没有子表达式 $((powershell 双引号里也执行,单引号外一律拦)。
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

// 引号外有没有 PowerShell 脚本块起始 {(真机实测 P2-3 放行 Where-Object
// 时补的闸):实现在 tools/command_safety,两档共用同一份,别写第二份。
// 语义:脚本块体内是任意代码,静态证明不了只读,一律 Unknown;cmd 的
// { } 没有执行语义,不查。

// 按引号外空白拆词,词身上的引号剥掉。
std::vector<std::string> TokenizeShell(const std::string& segment, bool single_quotes) {
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
        if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
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

// 词形归一:剥路径前缀取文件名、剥 .exe/.bat/.cmd/.com、小写化。
std::string NormalizeShellWord(const std::string& token) {
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

// Plan 只读件(两 shell 通用的窄表;cd/echo 这些 Auto 档放行的
// 一概不在——单子"首版只放"清单。P2-3 补 type/cat/head/tail/wc/grep
// 这批纯读件:command_safety 的 kGenericSafe 本就把它们分在只读档,
// Plan 照放,不再一刀拦)。
constexpr std::array<std::string_view, 13> kPlanSafeGeneric = {
    "rg", "findstr", "pwd", "where", "which", "ls", "dir",
    "type", "cat", "head", "tail", "wc", "grep"};

// PowerShell 只读 cmdlet 的窄表。P2-3 补齐 command_safety kPowershellSafe
// 里的只读管道件——只读管道(Get-ChildItem | Select-Object)原先逐段判
// 时撞上 Select-Object 不在表,整条被拒。脚本块写法另由
// HasUnquotedScriptBlock 拦(体内是任意代码,证明不了只读)。
constexpr std::array<std::string_view, 16> kPlanSafePowershell = {
    "get-content", "get-childitem", "select-string", "get-location", "get-item", "test-path",
    "select-object", "where-object", "sort-object", "measure-object", "format-table", "format-list",
    "get-process", "get-date", "write-output", "write-host"};

// 版本探针旗标:首词后只跟这些才认探针。
constexpr std::array<std::string_view, 3> kPlanProbeFlags = {"--version", "-v", "--help"};

// git 只读子命令(单子首版清单 status/log/diff/show/ls-files;P2-3 补
// ls-tree/rev-parse 一族——真机实测里 git ls-tree -r --name-only HEAD 被
// 拦,它只读树对象,无副作用;blame/cat-file/grep/shortlog/describe/
// rev-list/show-ref 同为纯查询)。
constexpr std::array<std::string_view, 14> kPlanGitReadOnly = {
    "status", "log", "diff", "show", "ls-files", "ls-tree", "rev-parse", "rev-list",
    "blame", "cat-file", "grep", "shortlog", "describe", "show-ref"};

// git diff/show 可能起 pager 或外部 diff driver 的旗标:带这些的按 Unknown
// (单子"git pager/external diff 不偷偷起任意程序"——禁 pager 由装配层在
// 执行环境里钉死,分类器只保证不认明知会起进程的组合)。
constexpr std::array<std::string_view, 4> kGitExternalFlags = {"--ext-diff", "--textconv", "--no-index", "--interactive"};

// 单段判定。段已保证非纯空白。拒绝时 rule 说明撞了哪条(P2-3:拦截
// 回执把命中的规则打印出来,模型好改写命令)。
PlanShellClassification ClassifyPlanSegment(const std::string& segment, bool is_powershell,
                                            bool single_quotes) {
    const auto deny = [](std::string rule) {
        return PlanShellClassification{PlanShellVerdict::Unknown, std::move(rule)};
    };
    if (HasUnquotedRedirection(segment, single_quotes)) {
        return deny("段内含未加引号的重定向(> <),Plan 一律不认写盘");
    }
    if (HasSubexpression(segment, single_quotes)) {
        return deny("段内含子表达式 $(,双引号里也执行");
    }
    if (is_powershell && tools::HasUnquotedScriptBlock(segment, single_quotes)) {
        return deny("段内含 PowerShell 脚本块 { },体内是任意代码;请改用无脚本块写法"
                    "(如 Where-Object Name -eq 'x')");
    }
    const std::vector<std::string> tokens = TokenizeShell(segment, single_quotes);
    if (tokens.empty()) {
        return deny("空命令");
    }
    // 环境赋值($env:X= / set X=)能改后续行为,不下 ReadOnly。
    if (ToLower(tokens.front()).rfind("$env:", 0) == 0) {
        return deny("段内含环境变量赋值($env:...),能改后续行为");
    }
    const std::string first = NormalizeShellWord(tokens.front());
    if (first.empty()) {
        return deny("首词为空");
    }
    // 探针:首词任意(黑名单在上面几道先拦不了——这里探针只认"后面全是
    // 探针旗标",写盘件带 --version 不改它的写盘性,但写盘件在白名单里
    // 本来就查不到,落到 Unknown,天然保守)。
    if (tokens.size() >= 2) {
        bool all_probe = true;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            if (!InList(kPlanProbeFlags, ToLower(tokens[i]))) {
                all_probe = false;
                break;
            }
        }
        if (all_probe) {
            return PlanShellClassification{PlanShellVerdict::ReadOnly, ""};
        }
    }
    // git:看第二词;git -C <主树> 这类全局选项照 Unknown(保守,单子同款)。
    if (first == "git") {
        if (tokens.size() >= 2 && InList(kPlanGitReadOnly, ToLower(tokens[1]))) {
            for (std::size_t i = 2; i < tokens.size(); ++i) {
                if (InList(kGitExternalFlags, ToLower(tokens[i]))) {
                    return deny("git 旗标 " + ToLower(tokens[i]) +
                                " 可起外部 diff driver/pager,不在 Plan 只读组合内");
                }
            }
            return PlanShellClassification{PlanShellVerdict::ReadOnly, ""};
        }
        return deny("git 子命令 " + (tokens.size() >= 2 ? ToLower(tokens[1]) : std::string("(缺)")) +
                    " 不在 Plan 只读子命令表(status/log/diff/show/ls-files/ls-tree/rev-parse 等)");
    }
    if (InList(kPlanSafeGeneric, first)) {
        return PlanShellClassification{PlanShellVerdict::ReadOnly, ""};
    }
    if (is_powershell && InList(kPlanSafePowershell, first)) {
        return PlanShellClassification{PlanShellVerdict::ReadOnly, ""};
    }
    return deny("首词 " + first + " 不在 Plan 只读白名单(rg/findstr/dir/type/Get-Content/"
                "Get-ChildItem/Select-Object 等)");
}

}  // namespace

PlanShellClassification ClassifyPlanShellDetailed(const std::string& command, const std::string& shell) {
    const bool is_powershell = (shell == "powershell");
    if (!is_powershell && shell != "cmd") {
        return {PlanShellVerdict::Unknown, "不认识的 shell(" + shell + "),Plan 不猜"};  // 不认识的 shell,不猜
    }
    const bool single_quotes = is_powershell;
    const std::vector<std::string> segments = SplitShellSegments(command, single_quotes);
    bool any_segment = false;
    for (const std::string& segment : segments) {
        const bool blank = std::all_of(segment.begin(), segment.end(),
                                       [](unsigned char c) { return std::isspace(c) != 0; });
        if (blank) {
            continue;
        }
        any_segment = true;
        PlanShellClassification judged = ClassifyPlanSegment(segment, is_powershell, single_quotes);
        if (judged.verdict != PlanShellVerdict::ReadOnly) {
            return judged;
        }
    }
    // 首版没有"判成 Mutating"的独立通路:写盘动词落在 Unknown 里,照样拒。
    // 枚举留 Mutating 一档,给将来 PlanCheck 沙箱单接手时细分用。
    if (any_segment) {
        return {PlanShellVerdict::ReadOnly, ""};
    }
    return {PlanShellVerdict::Unknown, "空命令"};
}

PlanShellVerdict ClassifyPlanShell(const std::string& command, const std::string& shell) {
    return ClassifyPlanShellDetailed(command, shell).verdict;
}

bool IsPlanAllowedBuiltinTool(const std::string& name) {
    // 单子首版工具表:放行的内置件。skill 是 P2-3 补的:加载技能只往
    // 上下文装 SKILL.md 说明,不改状态,Plan 不该管"读"。
    static constexpr std::array<std::string_view, 12> kAllowed = {
        "read_file", "search", "context_search", "context_read", "lsp",
        "web_search", "web_fetch", "ask_user", "tool_search", "skill", "agent", "run_command"};
    return std::find(kAllowed.begin(), kAllowed.end(), name) != kAllowed.end();
}

ModeVerdict EvaluateModePolicy(CollaborationMode mode, const PlanToolCapability& capability,
                               const PlanToolInput& input) {
    ModeVerdict verdict;
    if (mode != CollaborationMode::Plan) {
        return verdict;  // Default 一概放行,Plan 闸不越权管 Default
    }
    // 拒绝文案统一带 /plan off 指引(单子:UI 要写"Plan 模式禁止写入",
    // 并给 /plan off 提示;模型拿到同样语义的 tool_result)。
    const auto deny = [&verdict](const char* code, const std::string& reason) {
        verdict.allowed = false;
        verdict.code = code;
        verdict.reason = reason + "(Plan 模式只读研究;如需实施请让用户在计划审阅框批准,或 /plan off 退出)";
        return verdict;
    };
    // 宿主显式声明的 plan_safe 优先放行(信任根是注册表,不是工具自报)。
    if (capability.plan_safe_by_default) {
        return verdict;
    }
    // 未知来源:unknown(mcp 双身份还是 unknown)——MCP annotation 只是
    // hint,缺宿主显式 allow 一概拒(单子"MCP 与插件")。
    switch (capability.origin) {
        case PlanToolOrigin::Mcp:
        case PlanToolOrigin::PluginLua:
        case PlanToolOrigin::PluginNative:
        case PlanToolOrigin::Unknown:
            return deny(kErrModeDeniedUnknownSource,
                        "工具 " + capability.name + " 来自外部挂载,Plan 模式默认拒绝");
        case PlanToolOrigin::Builtin:
        case PlanToolOrigin::Lsp:
        case PlanToolOrigin::Agent:
        case PlanToolOrigin::Ptc:
            break;
    }
    if (capability.mutating) {
        return deny(kErrModeDeniedWrite, "工具 " + capability.name + " 会改动项目状态,Plan 模式禁止");
    }
    if (capability.name == "run_command") {
        if (!input.shell_safe) {
            // 拒绝回执带命中的规则(P2-3:把撞了哪条说清楚,模型好改写)。
            std::string reason = "命令不在 Plan 模式的只读白名单内";
            if (!input.shell_rule.empty()) {
                reason += ",命中规则: " + input.shell_rule;
            } else {
                reason += "(重定向/子表达式/脚本块/环境赋值/写盘命令均不放行)";
            }
            return deny(kErrModeDeniedShell, reason);
        }
        return verdict;
    }
    if (capability.name == "agent") {
        // Plan 按工具面放只读子代理(P2-3):内置 Explore 走只读表;
        // 自定义 Agent 看 tools.allow 是否全为只读工具(permissions 不设
        // read_only 档,契约 4.9——只读由 tools.allow 表达,装配层把判定
        // 结果经 agent_tools_readonly 递进来)。general-purpose 与工具面含
        // 写盘/命令工具的一概拒。
        const std::string role = ToLower(input.agent_role);
        if (role == "explore" || input.agent_tools_readonly) {
            return verdict;
        }
        return deny(kErrModeDeniedAgentRole,
                    "Plan 模式只放只读工具面的子代理(内置 Explore,或 tools.allow 全为只读工具的"
                    "自定义 Agent);\"" + input.agent_role + "\" 的工具面含写盘/命令工具或未加只读限制");
    }
    if (capability.name == "lsp") {
        return verdict;  // LSP 工具首版只有 definition/references/symbols/diagnostics,全只读
    }
    if (IsPlanAllowedBuiltinTool(capability.name)) {
        return verdict;
    }
    return deny(kErrModeDeniedUnknownSource, "工具 " + capability.name + " 不在 Plan 模式白名单内");
}

std::string ToString(CollaborationMode mode) {
    return mode == CollaborationMode::Plan ? "plan" : "default";
}

bool ParseCollaborationMode(const std::string& s, CollaborationMode& out) {
    if (s == "plan") {
        out = CollaborationMode::Plan;
        return true;
    }
    if (s == "default") {
        out = CollaborationMode::Default;
        return true;
    }
    return false;
}

std::string ToString(PlanReviewState state) {
    switch (state) {
        case PlanReviewState::Draft: return "draft";
        case PlanReviewState::Presented: return "presented";
        case PlanReviewState::Approved: return "approved";
        case PlanReviewState::Rejected: return "rejected";
        case PlanReviewState::Superseded: return "superseded";
    }
    return "draft";
}

bool ParsePlanReviewState(const std::string& s, PlanReviewState& out) {
    if (s == "draft") { out = PlanReviewState::Draft; return true; }
    if (s == "presented") { out = PlanReviewState::Presented; return true; }
    if (s == "approved") { out = PlanReviewState::Approved; return true; }
    if (s == "rejected") { out = PlanReviewState::Rejected; return true; }
    if (s == "superseded") { out = PlanReviewState::Superseded; return true; }
    return false;
}

ProposedPlanScan ScanProposedPlan(const std::string& text) {
    ProposedPlanScan scan;
    // 围栏状态机:``` 或 ~~~ 开头的行(允许前三枚空白)翻围栏态。围栏里的
    // <proposed_plan> 是代码示例,不当计划触发(单子"防代码块")。
    std::size_t open_count = 0;
    std::size_t close_count = 0;
    std::size_t first_open = std::string::npos;
    std::size_t first_close = std::string::npos;
    bool in_fence = false;
    std::size_t line_begin = 0;
    std::size_t fence_marker_len = 0;
    while (line_begin <= text.size()) {
        const std::size_t line_end = text.find('\n', line_begin);
        const std::size_t line_len = (line_end == std::string::npos ? text.size() : line_end) - line_begin;
        const std::string_view line(text.data() + line_begin, line_len);
        std::size_t non_space = 0;
        while (non_space < line.size() && (line[non_space] == ' ' || line[non_space] == '\t')) {
            ++non_space;
        }
        const std::string_view stripped = line.substr(non_space);
        if (!in_fence && (stripped.starts_with("```") || stripped.starts_with("~~~"))) {
            in_fence = true;
            fence_marker_len = stripped.substr(0, 3) == "```" ? 3 : 3;
        } else if (in_fence && !stripped.empty() && stripped.size() >= 3 &&
                   stripped.substr(0, 3) == text.substr(line_begin + non_space, 3) &&
                   (stripped.substr(0, 3) == "```" || stripped.substr(0, 3) == "~~~")) {
            // 关围栏:与本轮所开围栏同标记的独占行。简化判定:独占行(剥
            // 空白后恰是 ``` 或 ~~~,或标记后只有空白)即关。
            const std::size_t after = 3;
            const bool only_spaces_after =
                stripped.size() == after ||
                stripped.find_first_not_of(" \t", after) == std::string_view::npos;
            if (only_spaces_after) {
                in_fence = false;
            }
        }
        if (!in_fence) {
            // 引号内的标签也当普通文本扫?不——模型正文里的标签没有"引用"
            // 语义,按原文扫;围栏外逐个找。
            std::size_t search_from = line_begin;
            while (true) {
                const std::size_t open = text.find(kOpenTag, search_from);
                const std::size_t limit = line_end == std::string::npos ? text.size() : line_end;
                if (open == std::string::npos || open >= limit) {
                    break;
                }
                ++open_count;
                if (first_open == std::string::npos) {
                    first_open = open;
                }
                search_from = open + kOpenTag.size();
            }
            search_from = line_begin;
            while (true) {
                const std::size_t close = text.find(kCloseTag, search_from);
                const std::size_t limit = line_end == std::string::npos ? text.size() : line_end;
                if (close == std::string::npos || close >= limit) {
                    break;
                }
                ++close_count;
                if (first_close == std::string::npos) {
                    first_close = close;
                }
                search_from = close + kCloseTag.size();
            }
        }
        if (line_end == std::string::npos) {
            break;
        }
        line_begin = line_end + 1;
    }
    (void)fence_marker_len;
    if (open_count == 0) {
        return scan;  // 没有计划,普通正文
    }
    if (open_count > 1) {
        scan.ambiguous = true;  // 两份/嵌套:按普通 text,不弹审批
        return scan;
    }
    if (close_count == 0 || first_close == std::string::npos || first_close < first_open) {
        scan.truncated = true;  // 有开无合:流式中途的正常态
        return scan;
    }
    // 恰一对:闭合标签后的尾巴可以有(答疑短句),成品只收标签内正文。
    scan.found = true;
    scan.markdown = text.substr(first_open + kOpenTag.size(),
                                first_close - (first_open + kOpenTag.size()));
    return scan;
}

}  // namespace lubancode::runtime
