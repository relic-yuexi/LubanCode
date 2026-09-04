// /insights 的执行体(Token 账本单 A5)。分派位在文件尾,纯函数在前
// ——解析、摘要渲染、列账都不碰 IO;IO 只有扫 Journal(走领域层管线)、
// 写报告仓、读确认与打印。
#include "app/commands/insights_commands.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派位用)
#include "app/commands/usage_commands.hpp"    // LoadPricingTable(价格表口径)
#include "cli/console_input.hpp"              // ReadLine:clean 的二次确认
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/terminal_port.hpp"
#include "insights/derived_store.hpp"  // kDerivedAnalyzerDir(status 的摘要计数)
#include "insights/html_renderer.hpp"
#include "insights/insights_health.hpp"
#include "insights/redaction.hpp"
#include "insights/report_store.hpp"
#include "platform/paths.hpp"
#include "trajectory/directory.hpp"  // ReadSessionJson(workspace readable name)

namespace lubancode::app {
namespace {

using lubancode::cli::TermOut;

std::string FormatBytes(std::uintmax_t bytes) {
    std::ostringstream out;
    if (bytes >= 1024 * 1024) {
        out << (bytes / (1024 * 1024)) << "." << (bytes / 1024 % 1024 * 10 / 1024) << " MiB";
    } else {
        out << (bytes / 1024) << " KiB";
    }
    return out.str();
}

// workspace.json 的 readable_name(读不到回落 key;只显名与 key 短码,
// 绝对路径默认不进终端,§9.1)。
std::string ReadWorkspaceReadableName(const std::filesystem::path& workspace_dir,
                                      const std::string& key) {
    std::error_code ec;
    const std::filesystem::path manifest = workspace_dir / "workspace.json";
    if (!std::filesystem::is_regular_file(manifest, ec)) {
        return key;
    }
    std::ifstream in(manifest, std::ios::binary);
    if (!in) {
        return key;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const auto parsed = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return key;
    }
    return parsed.value("readable_name", key);
}

// 当前 workspace 的 sessions 根与 ref(从轨迹账本折)。
std::optional<lubancode::insights::InsightsWorkspaceRef> CurrentWorkspaceRef(
    lubancode::runtime::TrajectorySessionLedger* trajectory) {
    if (trajectory == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path session_dir = trajectory->session_dir();
    const std::filesystem::path sessions_root = session_dir.parent_path();
    const std::filesystem::path workspace_dir = sessions_root.parent_path();
    if (sessions_root.empty() || workspace_dir.empty()) {
        return std::nullopt;
    }
    lubancode::insights::InsightsWorkspaceRef ref;
    ref.workspace_key = lubancode::platform::PathToUtf8(workspace_dir.filename());
    ref.readable_name = ReadWorkspaceReadableName(workspace_dir, ref.workspace_key);
    ref.sessions_root = sessions_root;
    return ref;
}

// --all-workspaces:trajectories 根下逐仓一枚(workspaces/<key>/sessions)。
std::vector<lubancode::insights::InsightsWorkspaceRef> AllWorkspaceRefs(
    lubancode::runtime::TrajectorySessionLedger* trajectory) {
    std::vector<lubancode::insights::InsightsWorkspaceRef> refs;
    const auto current = CurrentWorkspaceRef(trajectory);
    if (!current.has_value()) {
        return refs;
    }
    const std::filesystem::path workspaces_root =
        current->sessions_root.parent_path().parent_path();
    std::error_code ec;
    if (!std::filesystem::is_directory(workspaces_root, ec)) {
        refs.push_back(*current);
        return refs;
    }
    for (const auto& entry : std::filesystem::directory_iterator(workspaces_root, ec)) {
        std::error_code dir_ec;
        if (!entry.is_directory(dir_ec) || dir_ec) {
            continue;
        }
        const std::filesystem::path sessions = entry.path() / "sessions";
        std::error_code sessions_ec;
        if (!std::filesystem::is_directory(sessions, sessions_ec)) {
            continue;
        }
        lubancode::insights::InsightsWorkspaceRef ref;
        ref.workspace_key = lubancode::platform::PathToUtf8(entry.path().filename());
        ref.readable_name = ReadWorkspaceReadableName(entry.path(), ref.workspace_key);
        ref.sessions_root = sessions;
        refs.push_back(std::move(ref));
    }
    return refs;
}

std::string NowYyyymmdd(const InsightsCommandContext& context) {
    if (!context.now_yyyymmdd.empty()) {
        return context.now_yyyymmdd;
    }
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday);
    return buffer;
}

// UTC 时钟的两份写法:generated_at(ISO)与 file_stamp(文件名)。
struct ClockStamp {
    std::string generated_at;
    std::string file_stamp;
};
ClockStamp UtcStamp(const InsightsCommandContext& context) {
    if (!context.generated_at.empty() && !context.file_stamp.empty()) {
        return ClockStamp{context.generated_at, context.file_stamp};
    }
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    ClockStamp stamp;
    stamp.file_stamp = context.file_stamp;
    stamp.generated_at = context.generated_at;
    if (stamp.generated_at.empty()) {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                      utc.tm_min, utc.tm_sec);
        stamp.generated_at = buffer;
    }
    if (stamp.file_stamp.empty()) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d",
                      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                      utc.tm_min, utc.tm_sec);
        stamp.file_stamp = buffer;
    }
    return stamp;
}

std::filesystem::path InsightsHome(const InsightsCommandContext& context) {
    if (!context.home_lubancode.has_value()) {
        return std::filesystem::path();
    }
    return lubancode::platform::Utf8ToPath(*context.home_lubancode) / "insights";
}

}  // namespace

// ---------------- 纯函数 ----------------

ParsedInsightsCommand ParseInsightsCommand(const std::string& args) {
    ParsedInsightsCommand parsed;
    std::istringstream stream(args);
    std::string word;
    while (stream >> word) {
        if (word == "status") {
            parsed.mode = ParsedInsightsCommand::Mode::Status;
        } else if (word == "clean") {
            parsed.mode = ParsedInsightsCommand::Mode::Clean;
        } else if (word == "--derived-only") {
            parsed.clean_derived_only = true;
        } else if (word == "--all-workspaces") {
            parsed.all_workspaces = true;
        } else if (word == "--include-active") {
            parsed.include_active = true;
        } else if (word == "--json") {
            parsed.json = true;
        } else if (word == "--show-paths") {
            parsed.show_paths = true;
        } else if (word == "--model-review") {
            parsed.later_model_review = true;
        } else if (word == "--open") {
            parsed.later_open = true;
        } else if (word == "--since") {
            std::string value;
            if (!(stream >> value)) {
                parsed.invalid = true;
                parsed.bad_word = "--since";
                continue;
            }
            std::string digits = value;
            if (!digits.empty() && (digits.back() == 'd' || digits.back() == 'D')) {
                digits.pop_back();
            }
            bool ok = !digits.empty();
            for (const char c : digits) {
                ok = ok && c >= '0' && c <= '9';
            }
            if (!ok) {
                parsed.invalid = true;
                parsed.bad_word = value;
                continue;
            }
            parsed.since_days = std::stoi(digits);
        } else if (word == "--sessions") {
            std::string value;
            if (!(stream >> value)) {
                parsed.invalid = true;
                parsed.bad_word = "--sessions";
                continue;
            }
            bool ok = !value.empty();
            for (const char c : value) {
                ok = ok && c >= '0' && c <= '9';
            }
            if (!ok) {
                parsed.invalid = true;
                parsed.bad_word = value;
                continue;
            }
            parsed.max_sessions = std::stoi(value);
        } else {
            parsed.invalid = true;
            parsed.bad_word = word;
        }
    }
    return parsed;
}

std::vector<std::string> FormatInsightsDigestLines(
    const lubancode::insights::InsightsGenerateResult& result,
    const std::filesystem::path& json_path, const std::filesystem::path& html_path,
    bool show_paths) {
    using lubancode::cli::FormatTokenCount;
    std::vector<std::string> lines;
    const lubancode::insights::InsightsReport& report = result.report;
    const lubancode::insights::WorkspaceAggregate& agg = result.aggregate;

    std::string scope_text = report.scope.all_workspaces
                                 ? "全部工作区(" +
                                       std::to_string(result.extras.workspace_names.size()) +
                                       " 个)"
                                 : report.scope.workspace_key;
    lines.push_back("Insights · " + scope_text + " · " + report.scope.since + " 至 " +
                    report.scope.until);
    // 一 工作概览。
    {
        std::ostringstream out;
        out << "  概览        " << agg.sessions << " 场(micro " << agg.micro_sessions
            << ",usage 照收) · turns " << agg.turns << " · 工具 " << agg.tool_calls
            << " · 验证 " << agg.verifications << " · outcome: ";
        if (agg.outcome_counts.empty()) {
            out << "无评估";
        } else {
            for (std::size_t i = 0; i < agg.outcome_counts.size(); ++i) {
                if (i > 0) {
                    out << " · ";
                }
                out << agg.outcome_counts[i].outcome << " ×"
                    << agg.outcome_counts[i].sessions;
            }
        }
        lines.push_back(out.str());
    }
    // 二 Token 与 Cache。
    {
        std::ostringstream out;
        out << "  Token       覆盖 " << agg.requests_with_usage << "/" << agg.requests_total
            << " 笔有 provider usage";
        if (agg.requests_unknown > 0) {
            out << "(" << agg.requests_unknown << " 笔 unknown,不折 0)";
        }
        out << " · 输入 " << FormatTokenCount(agg.input_tokens) << "(读 "
            << FormatTokenCount(agg.cache_read_tokens);
        if (agg.cache_read_ratio_percent.has_value()) {
            out << " " << *agg.cache_read_ratio_percent << "%";
        } else if (agg.requests_with_usage > 0) {
            out << " 比例 unknown";
        }
        out << "/写 " << FormatTokenCount(agg.cache_creation_tokens) << ") · 输出 "
            << FormatTokenCount(agg.output_tokens);
        if (agg.reasoning_tokens > 0) {
            out << "(推理 " << FormatTokenCount(agg.reasoning_tokens) << ",已含)";
        }
        out << " · 费用 not_priced(逐笔看 /usage)";
        lines.push_back(out.str());
    }
    // 三 Prompt 构成。
    {
        std::ostringstream out;
        out << "  Prompt      " << agg.prompt_rollups.size() << " 条规则命中(汇总)";
        for (std::size_t i = 0; i < agg.prompt_rollups.size() && i < 3; ++i) {
            out << " · " << agg.prompt_rollups[i].finding_id;
        }
        lines.push_back(out.str());
    }
    // 四 摩擦点。
    {
        std::ostringstream out;
        out << "  摩擦        " << agg.frictions.size() << " 类(按场次计)";
        for (std::size_t i = 0; i < agg.frictions.size() && i < 4; ++i) {
            out << " · " << agg.frictions[i].category << " " << agg.frictions[i].sessions
                << " 场";
        }
        lines.push_back(out.str());
    }
    // 五 交互形状。
    {
        std::ostringstream out;
        out << "  交互形状    样本 " << agg.sample_sessions << " 场 · 有验证 "
            << agg.sessions_with_verification << " · 有 outcome " << agg.sessions_outcome_assessed
            << " · 取消 " << agg.sessions_cancelled << " 场(语义类不猜,不评人)";
        lines.push_back(out.str());
    }
    // 六 建议。
    {
        if (agg.signals.empty()) {
            lines.push_back("  建议        0 条(先决不满足就不出,不硬凑)");
        } else {
            std::ostringstream out;
            out << "  建议        " << agg.signals.size() << " 条";
            for (std::size_t i = 0; i < agg.signals.size() && i < 3; ++i) {
                out << " · " << agg.signals[i].signal_id << " "
                    << agg.signals[i].sessions << " 场";
            }
            lines.push_back(out.str());
        }
    }
    // 七 覆盖与限制。
    {
        std::ostringstream out;
        out << "  覆盖        found " << result.counts.found << " · verified "
            << result.counts.verified << " · analyzed " << result.counts.analyzed
            << "(fresh 复用 " << result.counts.reused << " / 重算 "
            << result.counts.written << ")";
        if (result.counts.pending > 0) {
            out << " · pending " << result.counts.pending << "(再跑一回继续收)";
        }
        out << " · excluded " << result.counts.excluded;
        lines.push_back(out.str());
        std::size_t shown = 0;
        for (const auto& entry : result.extras.excluded) {
            if (shown >= 3) {
                break;
            }
            std::string reason = entry.reason;
            if (reason.size() > 60) {
                reason = reason.substr(0, 60) + "…";
            }
            lines.push_back("    排除 " + entry.session_id + "(" + entry.status +
                            (reason.empty() ? "" : ": " + reason) + ")");
            shown += 1;
        }
        if (result.extras.excluded.size() > shown) {
            lines.push_back("    …另有 " + std::to_string(result.extras.excluded.size() - shown) +
                            " 场(明细在报告)");
        }
        lines.push_back("  限制        汇总层无逐模型/用途分账(逐笔在 /usage);active/"
                        "corrupt/incomplete 不进分母;model review off(A6)");
        if (!result.extras.derived_errors.empty()) {
            lines.push_back("  落盘缺口    " + std::to_string(result.extras.derived_errors.size()) +
                            " 条摘要写失败(本地报告照出,明细在报告限制节)");
        }
    }
    lines.push_back("  报告        " + lubancode::platform::PathToUtf8(json_path));
    lines.push_back("              " + lubancode::platform::PathToUtf8(html_path) +
                    "(自包含,零网络;latest.* 同步替换)");
    if (show_paths) {
        for (const auto& [key, name] : result.extras.workspace_names) {
            lines.push_back("  workspace   " + name + " · " + key);
        }
    }
    return lines;
}

std::vector<std::string> FormatInsightsStatusLines(
    const std::vector<lubancode::insights::InsightsReportFile>& reports,
    const std::string& latest_note, std::int64_t derived_summaries,
    const std::filesystem::path& insights_home) {
    std::vector<std::string> lines;
    lines.push_back("Insights 状态 · " + lubancode::platform::PathToUtf8(insights_home));
    lines.push_back("  最近报告    " +
                    (latest_note.empty() ? "还没有(跑一回 /insights 生成)" : latest_note));
    lines.push_back("  历史报告    " + std::to_string(reports.size()) +
                    " 份(保留;清理只走 /insights clean --derived-only,且不碰报告)");
    const std::size_t shown = std::min<std::size_t>(reports.size(), 8);
    for (std::size_t i = 0; i < shown; ++i) {
        lines.push_back("    " + reports[i].path.filename().string() + "  " +
                        FormatBytes(reports[i].bytes));
    }
    if (reports.size() > shown) {
        lines.push_back("    …另有 " + std::to_string(reports.size() - shown) + " 份");
    }
    lines.push_back("  会话摘要    " + std::to_string(derived_summaries) +
                    " 份长期摘要(派生,可删可重算)");
    return lines;
}

std::vector<std::string> FormatInsightsCleanPlanLines(
    const lubancode::insights::InsightsCleanPlan& plan) {
    std::vector<std::string> lines;
    if (plan.items.empty()) {
        lines.push_back("没有可清的派生摘要(derived/ 下是空的)。");
        return lines;
    }
    lines.push_back("将删 " + std::to_string(plan.items.size()) + " 个文件,共 " +
                    FormatBytes(plan.total_bytes) + "(只删会话摘要,不碰 Journal,不碰报告):");
    const std::size_t shown = std::min<std::size_t>(plan.items.size(), 10);
    for (std::size_t i = 0; i < shown; ++i) {
        lines.push_back("  " + plan.items[i].path.string() + "  " +
                        FormatBytes(plan.items[i].bytes));
    }
    if (plan.items.size() > shown) {
        lines.push_back("  …另有 " + std::to_string(plan.items.size() - shown) + " 个(明细略)");
    }
    return lines;
}

// ---------------- 执行(IO) ----------------

void HandleInsightsCommand(const std::string& args, const InsightsCommandContext& context) {
    using lubancode::cli::tr;
    using lubancode::cli::trf;
    const ParsedInsightsCommand parsed = ParseInsightsCommand(args);
    if (parsed.invalid) {
        TermOut() << context.theme.error << trf("cmd.insights.unknown_arg", parsed.bad_word)
                  << "\n"
                  << tr("cmd.insights.usage_line") << "\n"
                  << context.theme.reset << "\n";
        TermOut().flush();
        return;
    }
    if (parsed.mode == ParsedInsightsCommand::Mode::Clean && !parsed.clean_derived_only) {
        // §10.3:clean 只认 --derived-only;裸 clean 不进 dry-run(那门在
        // trajectory gc),明说用法。
        TermOut() << context.theme.error << tr("cmd.insights.clean_needs_flag") << "\n"
                  << context.theme.reset << "\n";
        TermOut().flush();
        return;
    }
    if (parsed.later_model_review) {
        TermOut() << tr("cmd.insights.later_model_review") << "\n";
    }
    if (parsed.later_open) {
        TermOut() << tr("cmd.insights.later_open") << "\n";
    }

    // ---- 账未开:明说,不猜(§口径三戒) ----
    if (context.trajectory == nullptr) {
        TermOut() << context.theme.stats << tr("cmd.insights.ledger_off") << "\n"
                  << context.theme.reset;
        TermOut().flush();
        return;
    }

    // ---- 范围解析:当前 workspace 默认,--all-workspaces 显式跨仓 ----
    std::vector<lubancode::insights::InsightsWorkspaceRef> workspaces;
    if (parsed.all_workspaces) {
        workspaces = AllWorkspaceRefs(context.trajectory);
    } else {
        auto current = CurrentWorkspaceRef(context.trajectory);
        if (current.has_value()) {
            workspaces.push_back(std::move(*current));
        }
    }
    std::vector<std::filesystem::path> sessions_roots;
    for (const auto& workspace : workspaces) {
        sessions_roots.push_back(workspace.sessions_root);
    }

    // ---- status:报告仓的账(不扫 Journal) ----
    if (parsed.mode == ParsedInsightsCommand::Mode::Status) {
        const std::filesystem::path home = InsightsHome(context);
        if (home.empty()) {
            TermOut() << context.theme.error << tr("cmd.insights.no_home") << "\n"
                      << context.theme.reset << "\n";
            TermOut().flush();
            return;
        }
        const auto reports = lubancode::insights::ListInsightsReports(home);
        std::string latest_note;
        {
            const std::filesystem::path latest = home / "latest.json";
            std::error_code ec;
            if (std::filesystem::is_regular_file(latest, ec)) {
                std::ifstream in(latest, std::ios::binary);
                std::ostringstream buffer;
                buffer << in.rdbuf();
                const auto parsed_json =
                    nlohmann::json::parse(buffer.str(), nullptr, false);
                if (!parsed_json.is_discarded() && parsed_json.is_object()) {
                    latest_note = "generated_at=" +
                                  parsed_json.value("generated_at", std::string("?")) +
                                  " · analyzer=" +
                                  parsed_json.value("analyzer_version", std::string("?"));
                } else {
                    latest_note = "latest.json 坏了(可删后重跑)";
                }
            }
        }
        std::int64_t derived_count = 0;
        for (const auto& root : sessions_roots) {
            std::error_code ec;
            if (!std::filesystem::is_directory(root, ec)) {
                continue;
            }
            for (const auto& session : std::filesystem::directory_iterator(root, ec)) {
                std::error_code dir_ec;
                if (!session.is_directory(dir_ec) || dir_ec) {
                    continue;
                }
                std::error_code file_ec;
                if (std::filesystem::is_regular_file(
                        session.path() / "derived" /
                            lubancode::insights::kDerivedAnalyzerDir / "session-summary.json",
                        file_ec)) {
                    derived_count += 1;
                }
            }
        }
        for (const auto& line :
             FormatInsightsStatusLines(reports, latest_note, derived_count, home)) {
            TermOut() << line << "\n";
        }
        TermOut().flush();
        return;
    }

    // ---- clean:先列账,确认后才删(§10.3) ----
    if (parsed.mode == ParsedInsightsCommand::Mode::Clean) {
        const lubancode::insights::InsightsCleanPlan plan =
            lubancode::insights::PlanInsightsDerivedClean(sessions_roots);
        if (!plan.ok) {
            TermOut() << context.theme.error << plan.error << "\n"
                      << context.theme.reset << "\n";
            TermOut().flush();
            return;
        }
        for (const auto& line : FormatInsightsCleanPlanLines(plan)) {
            TermOut() << line << "\n";
        }
        if (plan.items.empty()) {
            TermOut().flush();
            return;
        }
        const auto answer = lubancode::cli::ReadLine(tr("cmd.insights.clean_confirm"),
                                                    context.theme);
        if (!answer.has_value() || (*answer != "y" && *answer != "Y" && *answer != "yes" &&
                                    *answer != "是")) {
            TermOut() << tr("cmd.insights.clean_cancelled") << "\n";
            TermOut().flush();
            return;
        }
        const lubancode::insights::InsightsCleanResult clean_result =
            lubancode::insights::ApplyInsightsClean(plan);
        TermOut() << trf("cmd.insights.clean_done",
                         static_cast<std::int64_t>(clean_result.deleted_files),
                         FormatBytes(clean_result.deleted_bytes))
                  << "\n";
        for (const auto& error : clean_result.errors) {
            TermOut() << "  " << error << "\n";
        }
        TermOut().flush();
        return;
    }

    // ---- generate:管线 -> 报告仓 ----
    const std::filesystem::path home = InsightsHome(context);
    if (home.empty()) {
        TermOut() << context.theme.error << tr("cmd.insights.no_home") << "\n"
                  << context.theme.reset << "\n";
        TermOut().flush();
        return;
    }
    lubancode::insights::InsightsGenerateOptions options;
    options.since_days = parsed.since_days;
    options.max_sessions = parsed.max_sessions;
    options.include_active = parsed.include_active;
    options.now_yyyymmdd = NowYyyymmdd(context);
    const ClockStamp stamp = UtcStamp(context);
    options.generated_at = stamp.generated_at;

    lubancode::insights::InsightsGenerateResult result =
        lubancode::insights::GenerateInsightsReport(workspaces, options);
    if (!result.ok) {
        TermOut() << context.theme.error << result.message << "\n"
                  << context.theme.reset << "\n";
        TermOut().flush();
        return;
    }
    // 价格表口径如实进边界说明:配没配、为什么不贴价(汇总层无逐模型拆账)。
    {
        const LoadedPricing pricing = LoadPricingTable(context.home_lubancode);
        result.extras.pricing_note = pricing.table.has_value()
                                         ? "已配 " + pricing.note + ";汇总层无逐模型拆账,未贴价"
                                         : pricing.note + ";逐笔贴价在 /usage";
    }

    const std::string json_text = result.report.ToJson().dump(2) + "\n";
    const std::string html_text =
        lubancode::insights::RenderInsightsHtml(result.report, result.aggregate,
                                                result.extras);
    const std::string label = result.report.scope.all_workspaces
                                  ? "all"
                                  : lubancode::insights::WorkspaceFileLabel(
                                        result.report.scope.workspace_key);
    const lubancode::insights::InsightsWriteResult written =
        lubancode::insights::WriteInsightsReportFiles(home, stamp.file_stamp, label,
                                                      parsed.since_days, json_text,
                                                      html_text);
    if (!written.ok) {
        // §14.5:派生写失败即停,临时文件已清,Journal 不受影响。
        TermOut() << context.theme.error
                  << trf("cmd.insights.write_failed", written.message) << "\n"
                  << context.theme.reset << "\n";
        TermOut().flush();
        return;
    }
    if (parsed.json) {
        TermOut() << lubancode::insights::RedactSecrets(json_text) << "\n";
    }
    for (const auto& line :
         FormatInsightsDigestLines(result, written.paths.json_path,
                                   written.paths.html_path, parsed.show_paths)) {
        TermOut() << line << "\n";
    }
    TermOut().flush();
}

// 命令分派注册制:/insights 的分派位。只装材料,正戏在上面。
CommandFlow HandleSlashInsights(SlashDispatchContext& ctx,
                                const lubancode::cli::ParsedSlashCommand& parsed) {
    InsightsCommandContext context{*ctx.theme};
    context.trajectory = ctx.trajectory;
    context.home_lubancode =
        ctx.home_lubancode != nullptr ? *ctx.home_lubancode : std::optional<std::string>{};
    HandleInsightsCommand(parsed.args, context);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
