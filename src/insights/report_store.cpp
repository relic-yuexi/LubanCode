// 报告仓的实现:原子写、latest 替换、清单与 derived-only clean。
#include "insights/report_store.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "insights/derived_store.hpp"  // kDerivedAnalyzerDir
#include "platform/atomic_write.hpp"   // 统一原子写(审计 P1)
#include "platform/paths.hpp"          // ReplaceFileAtomically/PathToUtf8

namespace lubancode::insights {
namespace {

using lubancode::platform::PathToUtf8;

// 文本整写,统一走 platform::AtomicWriteFile(审计 P1:替掉自备的
// 固定 .tmp 协议;唯一临时名 + 平台原子替换 + 失败清理)。
bool WriteTextAtomic(const std::filesystem::path& target, const std::string& text,
                     std::string* error) {
    const auto written = lubancode::platform::AtomicWriteFile(target, text);
    if (!written.has_value()) {
        *error = written.error().message;
        return false;
    }
    return true;
}

}  // namespace

std::string WorkspaceFileLabel(const std::string& workspace_key) {
    // key = <root-basename>-<hash12>;取 basename 段,拍扁成文件名安全字符。
    std::string base = workspace_key;
    const auto dash = base.rfind('-');
    if (dash != std::string::npos && dash + 13 == base.size() && dash > 0) {
        base = base.substr(0, dash);
    }
    std::string label;
    for (const char c : base) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') {
            label += c;
        } else {
            label += '_';
        }
        if (label.size() >= 32) {
            break;
        }
    }
    return label.empty() ? "ws" : label;
}

InsightsReportPaths PlanInsightsReportPaths(const std::filesystem::path& insights_home,
                                            const std::string& file_stamp,
                                            const std::string& workspace_label,
                                            int since_days) {
    InsightsReportPaths paths;
    paths.reports_dir = insights_home / "reports";
    const std::string stem = file_stamp + "-" + workspace_label + "-" +
                             std::to_string(since_days) + "d";
    paths.json_path = paths.reports_dir / (stem + ".json");
    paths.html_path = paths.reports_dir / (stem + ".html");
    paths.latest_json = insights_home / "latest.json";
    paths.latest_html = insights_home / "latest.html";
    return paths;
}

InsightsWriteResult WriteInsightsReportFiles(const std::filesystem::path& insights_home,
                                              const std::string& file_stamp,
                                              const std::string& workspace_label,
                                              int since_days, const std::string& json_text,
                                              const std::string& html_text) {
    InsightsWriteResult result;
    result.paths = PlanInsightsReportPaths(insights_home, file_stamp, workspace_label,
                                           since_days);
    // 历史报告先落;latest 最后换(任一历史落盘失败就不动 latest,读者
    // 永远指到完整的一对)。
    std::string error;
    if (!WriteTextAtomic(result.paths.json_path, json_text, &error)) {
        result.error_code = "report.json_write_failed";
        result.message = error;
        return result;
    }
    if (!WriteTextAtomic(result.paths.html_path, html_text, &error)) {
        result.error_code = "report.html_write_failed";
        result.message = error;
        return result;
    }
    if (!WriteTextAtomic(result.paths.latest_json, json_text, &error)) {
        result.error_code = "report.latest_json_failed";
        result.message = error;
        return result;
    }
    if (!WriteTextAtomic(result.paths.latest_html, html_text, &error)) {
        result.error_code = "report.latest_html_failed";
        result.message = error;
        return result;
    }
    result.ok = true;
    return result;
}

std::vector<InsightsReportFile> ListInsightsReports(const std::filesystem::path& insights_home) {
    std::vector<InsightsReportFile> files;
    const std::filesystem::path reports_dir = insights_home / "reports";
    std::error_code ec;
    if (!std::filesystem::is_directory(reports_dir, ec)) {
        return files;
    }
    for (const auto& entry : std::filesystem::directory_iterator(reports_dir, ec)) {
        std::error_code file_ec;
        if (entry.is_regular_file(file_ec) && !file_ec) {
            const std::string name = entry.path().filename().string();
            if (!name.empty() && name.front() >= '0' && name.front() <= '9') {
                InsightsReportFile file;
                file.path = entry.path();
                file.bytes = entry.file_size(file_ec);
                if (!file_ec) {
                    files.push_back(std::move(file));
                }
            }
        }
    }
    std::sort(files.begin(), files.end(),
              [](const InsightsReportFile& a, const InsightsReportFile& b) {
                  return PathToUtf8(a.path) < PathToUtf8(b.path);
              });
    return files;
}

bool IsPathContained(const std::filesystem::path& root, const std::filesystem::path& child) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return false;
    }
    // 相对化后不许出现 ".."(链接在别处单独拦,这里先挡词法逃逸)。
    const std::filesystem::path rel = child.lexically_relative(root);
    if (rel.empty() || *rel.begin() == "..") {
        return rel.empty();  // child == root 本身:没有下级文件可谈
    }
    // 逐级验:root 之下到 child 之间的每一级都不得是链接(reparse point
    // 逃逸,§15.6)。child 自身由调用方按常规文件再验。
    std::filesystem::path probe = root;
    for (const auto& part : rel) {
        probe /= part;
        if (probe == child) {
            break;
        }
        std::error_code link_ec;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(probe, link_ec)) &&
            !link_ec) {
            return false;
        }
    }
    return true;
}

InsightsCleanPlan PlanInsightsDerivedClean(
    const std::vector<std::filesystem::path>& sessions_roots) {
    InsightsCleanPlan plan;
    for (const auto& root : sessions_roots) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) {
            continue;
        }
        for (const auto& session :
             std::filesystem::directory_iterator(root, ec)) {
            std::error_code session_ec;
            // symlink 的 session 目录不跟(§15.6 reparse point 逃逸)。
            if (std::filesystem::is_symlink(
                    std::filesystem::symlink_status(session.path(), session_ec)) &&
                !session_ec) {
                continue;
            }
            if (!session.is_directory(session_ec) || session_ec) {
                continue;
            }
            const std::filesystem::path derived =
                session.path() / "derived" / kDerivedAnalyzerDir;
            std::error_code derived_ec;
            if (!std::filesystem::is_directory(derived, derived_ec)) {
                continue;
            }
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(derived, derived_ec)) {
                std::error_code link_ec;
                // 链接一律不进清理面(枚举默认也不跟目录链接,这里再挡
                // 指向常规文件的链接)。
                if (std::filesystem::is_symlink(
                        std::filesystem::symlink_status(entry.path(), link_ec)) &&
                    !link_ec) {
                    continue;
                }
                std::error_code file_ec;
                if (entry.is_regular_file(file_ec) && !file_ec) {
                    InsightsCleanItem item;
                    item.path = entry.path();
                    item.root = derived;
                    item.bytes = entry.file_size(file_ec);
                    item.kind = "derived-file";  // derived/<analyzer> 层内的一切派生件
                    plan.total_bytes += item.bytes;
                    plan.items.push_back(std::move(item));
                }
            }
        }
        if (ec) {
            plan.ok = false;
            plan.error = "sessions 根枚举失败: " + PathToUtf8(root) + ": " + ec.message();
            return plan;
        }
    }
    std::sort(plan.items.begin(), plan.items.end(),
              [](const InsightsCleanItem& a, const InsightsCleanItem& b) {
                  return PathToUtf8(a.path) < PathToUtf8(b.path);
              });
    return plan;
}

InsightsCleanResult ApplyInsightsClean(const InsightsCleanPlan& plan) {
    InsightsCleanResult result;
    for (const auto& item : plan.items) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(item.path, ec) || ec) {
            continue;  // 已被人删了:不报错,账面少一份
        }
        // 删前 containment 再验一遍(允许根记在账上,不拿路径倒推)。
        if (!IsPathContained(item.root, item.path)) {
            result.errors.push_back("containment 未过,跳过: " + PathToUtf8(item.path));
            continue;
        }
        std::error_code remove_ec;
        if (std::filesystem::remove(item.path, remove_ec) && !remove_ec) {
            result.deleted_files += 1;
            result.deleted_bytes += item.bytes;
        } else {
            result.errors.push_back("删不动: " + PathToUtf8(item.path) + ": " +
                                    remove_ec.message());
        }
    }
    return result;
}

}  // namespace lubancode::insights
