// /doctor insights 的实现。只读检查:真 IO 只有派生目录探写、latest.json
// 读回与摘要扫描(§10.4 的清单逐条;不重建报告、不调模型)。
#include "insights/insights_health.hpp"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "insights/derived_store.hpp"
#include "insights/html_renderer.hpp"
#include "platform/paths.hpp"

namespace lubancode::insights {
namespace {

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::string();
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

std::vector<InsightsHealthLine> CheckInsightsHealth(const InsightsHealthInput& input) {
    std::vector<InsightsHealthLine> lines;
    const auto add = [&](const std::string& name, char status, const std::string& detail) {
        InsightsHealthLine line;
        line.name = name;
        line.status = status;
        line.detail = detail;
        lines.push_back(std::move(line));
    };

    // 1/2 轨迹与 reader 口径(P0-6:开关已删,账恒开)。
    add("trajectory", ' ', "Journal 恒开,唯一事实账");
    add("reader", ' ', "Trajectory event schema v2(usage owner)+ v1 legacy read;混写 stream 拒收");

    // 3 派生目录权限与磁盘。
    if (input.insights_home.has_value()) {
        std::error_code ec;
        std::filesystem::create_directories(*input.insights_home, ec);
        const std::filesystem::path probe = *input.insights_home / ".write-probe";
        bool writable = false;
        {
            std::ofstream out(probe, std::ios::binary | std::ios::trunc);
            writable = out.good();
        }
        std::filesystem::remove(probe, ec);
        const auto space = std::filesystem::space(*input.insights_home, ec);
        std::ostringstream detail;
        detail << (writable ? "可写" : "不可写") << " · "
               << lubancode::platform::PathToUtf8(*input.insights_home);
        if (!ec && space.available > 0) {
            detail << " · 余量 " << (space.available / (1024 * 1024)) << " MiB";
        } else {
            detail << " · 余量 unknown";
        }
        add("derived 目录", writable ? ' ' : 'x', detail.str());
    } else {
        add("derived 目录", '!', "主目录解析不到,报告无处落(/insights 不可用)");
    }

    // 4 最近 analyzer 版本与最后成功报告。
    if (input.insights_home.has_value()) {
        const std::string latest =
            ReadTextFile(*input.insights_home / "latest.json");
        if (latest.empty()) {
            add("最近报告", '!', "还没有成功报告(跑一回 /insights 生成)");
        } else {
            const auto parsed = nlohmann::json::parse(latest, nullptr, false);
            if (parsed.is_discarded()) {
                add("最近报告", 'x', "latest.json 不是合法 JSON(可删后重跑 /insights)");
            } else {
                const std::string generated = parsed.value("generated_at", std::string());
                const std::string analyzer = parsed.value("analyzer_version", std::string());
                std::ostringstream detail;
                detail << "generated_at=" << (generated.empty() ? "?" : generated)
                       << " · analyzer=" << (analyzer.empty() ? "?" : analyzer);
                if (analyzer != kInsightsAnalyzerVersion) {
                    detail << "(现行 " << kInsightsAnalyzerVersion
                           << ",旧报告留着,页眉标旧版本)";
                }
                add("最近报告", ' ', detail.str());
            }
        }
    }

    // 5 stale/corrupt 摘要数(按范围扫;只看派生层,不重验 Journal)。
    {
        std::int64_t stale = 0;
        std::int64_t corrupt = 0;
        std::int64_t total = 0;
        std::int64_t sessions = 0;
        std::optional<std::int64_t> bound;
        if (const auto now_days = YyyymmddToDays(input.now_yyyymmdd);
            now_days.has_value()) {
            bound = *now_days - input.since_days;
        }
        for (const auto& workspace : input.workspaces) {
            std::error_code ec;
            if (!std::filesystem::is_directory(workspace.sessions_root, ec)) {
                continue;
            }
            for (const auto& entry :
                 std::filesystem::directory_iterator(workspace.sessions_root, ec)) {
                std::error_code dir_ec;
                if (!entry.is_directory(dir_ec) || dir_ec) {
                    continue;
                }
                // 日期窗(与生成侧同口径:读得出日期且在窗外的跳过)。
                if (bound.has_value()) {
                    const auto days = YyyymmddToDays(
                        entry.path().filename().string().substr(0, 8));
                    if (days.has_value() && *days < *bound) {
                        continue;
                    }
                }
                sessions += 1;
                const DerivedReadResult read = ReadExistingSessionSummary(entry.path());
                if (!read.exists) {
                    continue;  // 没摘要不算 stale(还没分析过)
                }
                total += 1;
                if (!read.parse_ok) {
                    corrupt += 1;
                } else if (read.summary.analyzer_version != kInsightsAnalyzerVersion) {
                    stale += 1;
                }
            }
        }
        std::ostringstream detail;
        detail << "扫描 " << sessions << " 场 · 有摘要 " << total << " · stale " << stale
               << " · 坏 JSON " << corrupt
               << "(stale 下轮 /insights 自动重算;坏账整间排除)";
        add("摘要账", corrupt > 0 ? '!' : ' ', detail.str());
    }

    // 6 价格表。
    if (input.pricing_loaded) {
        add("价格表", ' ', input.pricing_note + "(报告层暂不贴价;逐笔在 /usage)");
    } else {
        add("价格表", '!', (input.pricing_note.empty() ? "未配价格表" : input.pricing_note) +
                              " —— token 照报,费用 not_priced");
    }

    // 7 HTML renderer 自检。
    {
        const std::string failure = InsightsHtmlSelfCheck();
        if (failure.empty()) {
            add("HTML renderer", ' ', "自检过(CSP default-src 'none'/转义/七节锚点/零外链)");
        } else {
            add("HTML renderer", 'x', failure);
        }
    }

    // 8 model review 默认档。
    add("model review", ' ', "默认关闭;显式 --model-review 属后续批次 A6,发送前另过确认门");

    return lines;
}

}  // namespace lubancode::insights
