// /insights(Token 账本单 A5):把 A4 的分析管线汇成跨会话/跨工作区的
// 总报告——工作区级汇总、五态场次覆盖账、finding 汇总与功能信号推荐面。
//
// 形态(§10.3,本批落全量):
//   /insights [--since 30d] [--sessions 200]   生成报告(默认当前 workspace)
//   /insights --all-workspaces                 跨仓(显式开关)
//   /insights --include-active                 未封口场读高水位(provisional)
//   /insights --json                           报告 JSON 也打到终端
//   /insights status                           报告仓与最近报告的账
//   /insights clean --derived-only             列账->确认->只删派生摘要
//   --model-review 属 A6、--open 属 A7(containment),敲了明说,本地报告照出。
//
// 口径三戒:只摆事实不说教;flag 关明说"账未开"(不猜不凑);报告七节
// 齐但零命中就写零命中,不硬凑 finding。中英文案成对(i18n)。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/commands/command_flow.hpp"
#include "cli/slash_commands.hpp"
#include "cli/theme.hpp"
#include "insights/insights_generate.hpp"
#include "insights/report_store.hpp"

namespace lubancode::runtime {
class TrajectorySessionLedger;
}

namespace lubancode::app {

// ---------------- 纯函数(单测钉住) ----------------

// /insights 的二级参数拆词(纯解析)。
struct ParsedInsightsCommand {
    enum class Mode { Generate, Status, Clean, Invalid };
    Mode mode = Mode::Generate;
    int since_days = 30;      // --since Nd(默认 30,§9.1)
    int max_sessions = 200;   // --sessions N(默认 200,§9.1)
    bool all_workspaces = false;
    bool include_active = false;
    bool json = false;
    bool show_paths = false;         // 默认只显 readable name 与 key 短码(§9.1)
    bool later_model_review = false;  // --model-review(A6,明说)
    bool later_open = false;          // --open(A7 containment,明说)
    bool invalid = false;
    bool clean_derived_only = false;  // clean 的 --derived-only
    std::string bad_word;
};
ParsedInsightsCommand ParseInsightsCommand(const std::string& args);

// 生成完毕的终端摘要(七节的紧凑面 + 报告路径)。行不带换行符。
std::vector<std::string> FormatInsightsDigestLines(
    const lubancode::insights::InsightsGenerateResult& result,
    const std::filesystem::path& json_path, const std::filesystem::path& html_path,
    bool show_paths);

// /insights status 的行(报告仓清单 + 最近报告账;纯渲染,数据由调用方递)。
std::vector<std::string> FormatInsightsStatusLines(
    const std::vector<lubancode::insights::InsightsReportFile>& reports,
    const std::string& latest_note, std::int64_t derived_summaries,
    const std::filesystem::path& insights_home);

// /insights clean 的列账行(将删文件与字节;二次确认前给用户看的面)。
std::vector<std::string> FormatInsightsCleanPlanLines(
    const lubancode::insights::InsightsCleanPlan& plan);

// ---------------- 执行(IO) ----------------

// 会话侧材料包(handler 从 SlashDispatchContext 装好)。
struct InsightsCommandContext {
    const lubancode::cli::Theme& theme;
    // flag 开的会话递账本(当前 workspace 的 sessions 根从这折);
    // nullptr = 轨迹没开,/insights 明说账未开。
    lubancode::runtime::TrajectorySessionLedger* trajectory = nullptr;
    std::optional<std::string> home_lubancode;
    // 时钟注入(测试注固定值得字节稳定的报告名);空 = 现取。
    std::string now_yyyymmdd;
    std::string generated_at;
    std::string file_stamp;
};

// /insights 主入口(IO 侧):解析 -> 生成/状态/清理 -> 渲染。
void HandleInsightsCommand(const std::string& args, const InsightsCommandContext& context);

// 命令分派注册制:/insights 的分派位。
struct SlashDispatchContext;
CommandFlow HandleSlashInsights(SlashDispatchContext& ctx,
                                const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
