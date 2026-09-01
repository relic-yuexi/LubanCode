// /insights 的报告仓(Token 账本单 §9.2/A5)。
//
//   <insights_home>/            (~/.lubancode/insights)
//     reports/
//       20260830-154200-lubancode-7d.json
//       20260830-154200-lubancode-7d.html
//     latest.json
//     latest.html
//
// 历史报告保留(§9.2:按配置清理——本批不自动删,清理走 clean 的人手);
// latest.* 临时文件 + 原子替换,全程不留半截。clean --derived-only 只删
// 派生物(各场 derived/insights-v1 摘要),不碰 canonical Journal,也不
// 碰历史报告(§10.3;containment:候选一律验"在允许根下且非链接")。
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::insights {

// insights_home 下的落位(reports/ 与 latest.*)。
struct InsightsReportPaths {
    std::filesystem::path reports_dir;
    std::filesystem::path json_path;
    std::filesystem::path html_path;
    std::filesystem::path latest_json;
    std::filesystem::path latest_html;
};
InsightsReportPaths PlanInsightsReportPaths(const std::filesystem::path& insights_home,
                                            const std::string& file_stamp,
                                            const std::string& workspace_label,
                                            int since_days);

// 报告文件名的工作区短码:workspace key 的 basename 段(key 形如
// <basename>-<hash12>);非 [A-Za-z0-9._-] 的字符折成 '_',空给 "ws"。
std::string WorkspaceFileLabel(const std::string& workspace_key);

struct InsightsWriteResult {
    bool ok = false;
    std::string error_code;  // report.* 稳定码
    std::string message;
    InsightsReportPaths paths;
};
// 写 reports/<stamp>-<label>-<since>d.{json,html} 并原子替换 latest.*。
// json_text/html_text 由调用方备好(域内不重算);任一失败都不碰 latest。
InsightsWriteResult WriteInsightsReportFiles(const std::filesystem::path& insights_home,
                                              const std::string& file_stamp,
                                              const std::string& workspace_label,
                                              int since_days, const std::string& json_text,
                                              const std::string& html_text);

// 历史报告清单(按文件名字典序;只收 *.json/*.html 常规文件)。
struct InsightsReportFile {
    std::filesystem::path path;
    std::uintmax_t bytes = 0;
};
std::vector<InsightsReportFile> ListInsightsReports(const std::filesystem::path& insights_home);

// ---- clean --derived-only(§10.3:先列账,二次确认后才删) ----

struct InsightsCleanItem {
    std::filesystem::path path;
    std::filesystem::path root;  // 允许根(<session>/derived/<analyzer>);删前 containment 按它验
    std::uintmax_t bytes = 0;
    std::string kind;  // "derived-file"(derived/<analyzer> 层内的派生件)
};
struct InsightsCleanPlan {
    std::vector<InsightsCleanItem> items;  // 路径字典序
    std::uintmax_t total_bytes = 0;
    bool ok = true;
    std::string error;  // 枚举失败的人话(ok=false 时)
};
// 收各 sessions 根下 <session>/derived/<analyzer>/** 的派生文件。canonical
// Journal(main.jsonl 等)与历史报告都不在清理面。symlink 一律跳过并留在
// 账外(不跟链接,§15.6)。
InsightsCleanPlan PlanInsightsDerivedClean(
    const std::vector<std::filesystem::path>& sessions_roots);

struct InsightsCleanResult {
    std::int64_t deleted_files = 0;
    std::uintmax_t deleted_bytes = 0;
    std::vector<std::string> errors;  // 删不动的逐条人话
};
// 按 plan 删(只删 plan 里的文件;删前再验一遍常规文件,消失就跳过)。
InsightsCleanResult ApplyInsightsClean(const InsightsCleanPlan& plan);

// child(相对或绝对)是否真落在 root 之下(逐级验,防 .. 与链接逃逸;
// root 须是已存在的目录)。
bool IsPathContained(const std::filesystem::path& root,
                     const std::filesystem::path& child);

}  // namespace lubancode::insights
