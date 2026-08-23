// ScheduledActionResolver 与 catalog 的骨架实现(见 scheduled_action.hpp)。

#include "runtime/scheduled_action.hpp"

#include <cctype>

namespace lubancode::runtime::loop {

std::vector<SchedulableActionInfo> SchedulableActionCatalog() {
    return {
        {SchedulableAction::ReportStatus, ActionSafety::LocalOnly,
         "汇报会话/worktree/loop 状态(纯本地,零副作用)"},
        {SchedulableAction::SummarizeDiff, ActionSafety::LocalOnly,
         "汇总当前分支 diff(只读 git)"},
        {SchedulableAction::RunTests, ActionSafety::NeedsCommand,
         "跑测试套件(过 run_command 权限链)"},
        {SchedulableAction::CheckCi, ActionSafety::NeedsRemote,
         "查 CI/PR 状态(只读远端查询)"},
    };
}

ActionSafety SafetyOf(SchedulableAction action) {
    switch (action) {
        case SchedulableAction::ReportStatus:
        case SchedulableAction::SummarizeDiff:
            return ActionSafety::LocalOnly;
        case SchedulableAction::RunTests:
            return ActionSafety::NeedsCommand;
        case SchedulableAction::CheckCi:
            return ActionSafety::NeedsRemote;
    }
    return ActionSafety::LocalOnly;
}

namespace {

// 小写化(ASCII;关键词表全用小写,中文原样)。
std::string LowerAscii(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// 关键词表:动作 -> 触发词(小写;子串匹配)。按 catalog 序扫,先命中先返。
// 表只收"明确说了要做什么"的说法,模糊措辞不进表(交模型,见头文件的
// 保守性注释)。
struct ActionKeywords {
    SchedulableAction action;
    std::vector<const char*> keywords;
};

const std::vector<ActionKeywords>& KeywordTable() {
    static const std::vector<ActionKeywords> table = {
        {SchedulableAction::RunTests,
         {"跑测试", "跑一遍测试", "运行测试", "执行测试", "全量测试", "回归测试",
          "run tests", "run the tests", "run test suite", "跑 ctest", "ctest"}},
        {SchedulableAction::CheckCi,
         {"查 ci", "看看 ci", "检查 ci", "ci 状态", "ci 好了没", "看 pr", "查 pr",
          "check ci", "ci status", "check the ci", "pr 状态"}},
        {SchedulableAction::SummarizeDiff,
         {"汇总 diff", "总结 diff", "看看改动", "改了什么", "当前分支的改动",
          "summarize diff", "summarize the diff", "what changed"}},
        {SchedulableAction::ReportStatus,
         {"汇报状态", "报告状态", "当前状态", "会话状态", "没事就报平安",
          "report status", "status report", "nothing to do"}},
    };
    return table;
}

}  // namespace

std::optional<SchedulableAction> ResolveScheduledAction(const std::string& prompt) {
    // 空白正文给 nullopt(不冒充"没事做")。
    bool all_blank = true;
    for (const unsigned char c : prompt) {
        if (!std::isspace(c)) {
            all_blank = false;
            break;
        }
    }
    if (all_blank) {
        return std::nullopt;
    }
    const std::string haystack = LowerAscii(prompt);
    for (const auto& entry : KeywordTable()) {
        for (const char* keyword : entry.keywords) {
            if (haystack.find(keyword) != std::string::npos) {
                return entry.action;
            }
        }
    }
    return std::nullopt;  // 认不出:回落发模型,不硬猜
}

}  // namespace lubancode::runtime::loop
