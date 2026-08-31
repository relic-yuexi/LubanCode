// /usage 的执行体(Token 账本单 A2)。分派位在文件尾,纯函数在前——解析、
// 计价、渲染都不碰 IO,单测直接钉;IO 只有读 Journal、读价格表、打印。
#include "app/commands/usage_commands.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "accounting/cost_estimator.hpp"
#include "accounting/purpose.hpp"
#include "agent/model_router.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派位用)
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/terminal_port.hpp"
#include "runtime/trajectory_session.hpp"

namespace lubancode::app {
namespace {

// 四舍五入的整数百分比;分母 <= 0 给 0(调用方先判 unknown)。
int SharePercent(std::int64_t part, std::int64_t whole) {
    if (whole <= 0) {
        return 0;
    }
    return static_cast<int>((part * 200 + whole) / (whole * 2));
}

// "用途/模型"行的份额摘要:最多 max_entries 个 "label P%",其余折进"…共 N 类"。
std::string ShareLine(const std::vector<lubancode::accounting::UsageBreakdown>& rows,
                      std::int64_t whole, std::size_t max_entries) {
    if (rows.empty()) {
        return "0";
    }
    // 份额降序,同份字典序——人先看大头。
    std::vector<const lubancode::accounting::UsageBreakdown*> sorted;
    sorted.reserve(rows.size());
    for (const auto& row : rows) {
        sorted.push_back(&row);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto* a, const auto* b) {
                  const std::int64_t va = a->totals.total_billed_shape_tokens;
                  const std::int64_t vb = b->totals.total_billed_shape_tokens;
                  if (va != vb) {
                      return va > vb;
                  }
                  return a->label < b->label;
              });
    std::ostringstream line;
    std::size_t shown = 0;
    for (const auto* row : sorted) {
        if (shown >= max_entries) {
            break;
        }
        if (shown > 0) {
            line << " · ";
        }
        line << row->label << " " << SharePercent(row->totals.total_billed_shape_tokens, whole)
             << "%";
        ++shown;
    }
    if (sorted.size() > shown) {
        line << " · …共 " << sorted.size() << " 类";
    }
    return line.str();
}

}  // namespace

// ---------------- 纯函数 ----------------

ParsedUsageCommand ParseUsageCommand(const std::string& args) {
    ParsedUsageCommand parsed;
    std::istringstream stream(args);
    std::string word;
    while (stream >> word) {
        if (word == "session") {
            if (!(stream >> parsed.session_id)) {
                parsed.invalid = true;
                parsed.bad_word = "session";
            }
            parsed.scope = ParsedUsageCommand::Scope::NamedSession;
        } else if (word == "--json") {
            parsed.json = true;
        } else if (word == "--by") {
            std::string dimension;
            if (!(stream >> dimension)) {
                parsed.invalid = true;
                parsed.bad_word = "--by";
                continue;
            }
            if (dimension == "model") {
                parsed.by = ParsedUsageCommand::By::Model;
            } else if (dimension == "purpose") {
                parsed.by = ParsedUsageCommand::By::Purpose;
            } else if (dimension == "run") {
                parsed.by = ParsedUsageCommand::By::Run;
            } else if (dimension == "outcome") {
                parsed.by = ParsedUsageCommand::By::Outcome;
            } else {
                parsed.invalid = true;
                parsed.bad_word = dimension;
            }
        } else if (word == "day" || word == "week" || word == "workspace" || word == "all") {
            // 跨场汇总属后续批次(§16:A5 insights 管线):明说,不冒充。
            parsed.later_scope = word;
        } else {
            parsed.invalid = true;
            parsed.bad_word = word;
        }
    }
    return parsed;
}

std::string FormatMicrosAmount(std::int64_t micros, const std::string& currency) {
    // 全程整数拼装:整数部 + "." + 6 位小数(micros 本就是 1e-6 单位,一位
    // 不多一位不少,不四舍五入、不进 float)。
    const bool negative = micros < 0;
    const std::int64_t magnitude = negative ? -micros : micros;
    const std::int64_t whole = magnitude / 1'000'000;
    const std::int64_t frac = magnitude % 1'000'000;
    std::ostringstream out;
    if (negative) {
        out << "-";
    }
    if (!currency.empty()) {
        out << currency;
    }
    out << whole << '.' << (frac / 100'000) << (frac / 10'000 % 10) << (frac / 1'000 % 10)
        << (frac / 100 % 10) << (frac / 10 % 10) << (frac % 10);
    return out.str();
}

void ApplyCostEstimates(std::vector<lubancode::accounting::UsageSample>& samples,
                        const std::optional<lubancode::accounting::PricingTable>& table) {
    if (!table.has_value()) {
        for (auto& sample : samples) {
            sample.cost = lubancode::accounting::CostEstimate{};
        }
        return;
    }
    const std::string& session_id = samples.empty() ? std::string() : samples.front().session_id;
    // session id 头 8 位是 YYYYMMDD,折成价格表要的 "YYYY-MM-DD";形状不对
    // 按空算(生效日比较跳过,按表价计)。
    std::string request_day;
    if (session_id.size() >= 8 && session_id.find('-') == 8) {
        const std::string yyyymmdd = session_id.substr(0, 8);
        bool digits = true;
        for (const char c : yyyymmdd) {
            digits = digits && c >= '0' && c <= '9';
        }
        if (digits) {
            request_day = yyyymmdd.substr(0, 4) + "-" + yyyymmdd.substr(4, 2) + "-" +
                          yyyymmdd.substr(6, 2);
        }
    }
    for (auto& sample : samples) {
        if (!sample.usage.has_value()) {
            sample.cost = lubancode::accounting::CostEstimate{};
            continue;
        }
        sample.cost = lubancode::accounting::EstimateCost(*sample.usage, &*table, sample.provider,
                                                          sample.model, request_day);
    }
}

std::vector<std::string> FormatUsageReport(const UsageReportModel& model) {
    using lubancode::accounting::UsageTotals;
    std::vector<std::string> lines;
    const UsageTotals& totals = model.aggregate.totals;

    // 标题:场次 + 成色(未封口恒 provisional,§10.1)。
    std::string title = "Usage · " + model.session_id;
    if (model.provisional) {
        title += "(未封口 provisional)";
    }
    lines.push_back(std::move(title));

    if (totals.requests_total == 0) {
        lines.push_back("  这场 session 还没有模型请求账(Journal 里一笔没有)——不猜。");
        return lines;
    }

    // 覆盖:unknown 单列,不冒充(§14.3)。
    {
        std::ostringstream out;
        out << "  覆盖        " << totals.requests_with_usage << "/" << totals.requests_total
            << " 笔有 provider usage";
        if (totals.requests_unknown > 0) {
            out << " · " << totals.requests_unknown << " 笔 unknown(未报,不折 0)";
        }
        if (totals.requests_retry > 0) {
            out << " · 重试 " << totals.requests_retry << " 笔(各记各账)";
        }
        lines.push_back(out.str());
    }
    // 输入(§7.4)。
    {
        std::ostringstream out;
        out << "  输入        " << lubancode::cli::FormatTokenCount(totals.input_tokens);
        if (const auto ratio = totals.cache_read_ratio_percent()) {
            out << " · cache 读 " << lubancode::cli::FormatTokenCount(totals.cache_read_tokens)
                << "(" << *ratio << "%)";
        } else if (totals.requests_with_usage > 0) {
            out << " · cache 读 " << lubancode::cli::FormatTokenCount(totals.cache_read_tokens)
                << "(比例 unknown:实测输入为 0)";
        }
        out << " · cache 写 "
            << lubancode::cli::FormatTokenCount(totals.cache_creation_tokens);
        out << " · 合计输入 " << lubancode::cli::FormatTokenCount(totals.total_input_tokens);
        lines.push_back(out.str());
    }
    // 输出:reasoning 是子集,注明(§四.4)。
    {
        std::ostringstream out;
        out << "  输出        " << lubancode::cli::FormatTokenCount(totals.output_tokens);
        if (totals.reasoning_tokens > 0) {
            out << " · 推理 " << lubancode::cli::FormatTokenCount(totals.reasoning_tokens)
                << "(已含在输出,不另加)";
        }
        lines.push_back(out.str());
    }
    // 模型与用途:份额行,分母注明(§7.4"任何比例都要注明分母")。
    {
        const std::int64_t whole = totals.total_billed_shape_tokens;
        lines.push_back("  模型        " + ShareLine(model.aggregate.by_model, whole, 3) +
                        "(按 input+output token 占比)");
        lines.push_back("  用途        " + ShareLine(model.aggregate.by_purpose, whole, 4) +
                        "(按 input+output token 占比)");
    }
    // 费用(§6.3 四条线):没配表照样报 token,费用 not_priced。
    {
        std::ostringstream out;
        if (model.pricing.has_value()) {
            out << "  估算费用    " << FormatMicrosAmount(totals.cost_micros, "$") << " · 价格表 "
                << model.pricing->id << " · 本地估算,非账单";
            if (totals.requests_priced < totals.requests_with_usage) {
                out << " · 命中 " << totals.requests_priced << "/"
                    << totals.requests_with_usage << " 笔(其余 not_priced)";
            }
        } else {
            out << "  估算费用    not_priced(" << model.pricing_note
                << ";token 照报,不估不猜)";
        }
        lines.push_back(out.str());
    }
    // cache 观察:只报 observed,不判罪(§7.2)。
    {
        const auto& cache = model.aggregate.cache;
        std::ostringstream out;
        out << "  cache 观察  epoch 重建 " << cache.expected_rebuild_events << " 次 · 疑似未命中 "
            << cache.unexpected_miss_candidates << " 笔(候选:TTL 过期/端不稳也长这模样) · 前缀改写 "
            << cache.append_only_breaks << " 笔";
        if (cache.epoch_unlabeled > 0) {
            out << " · 无 epoch 标注 " << cache.epoch_unlabeled << " 笔";
        }
        lines.push_back(out.str());
    }
    // 成色:账的来历说明,不混"异常"语气。
    {
        std::ostringstream out;
        out << "  成色        " << model.aggregate.run_ids.size() << " 条 run";
        if (model.aggregate.legacy_samples > 0) {
            out << " · v1 旧账 " << model.aggregate.legacy_samples << " 笔";
        }
        if (model.aggregate.incomplete_linkage_samples > 0) {
            out << " · 断链 " << model.aggregate.incomplete_linkage_samples << " 笔";
        }
        out << " · session status=" << model.status;
        lines.push_back(out.str());
    }

    // --by 分账表(要哪张打哪张;份额分母同上)。
    const std::vector<lubancode::accounting::UsageBreakdown>* table = nullptr;
    const char* table_title = nullptr;
    switch (model.by) {
        case ParsedUsageCommand::By::Model:
            table = &model.aggregate.by_model;
            table_title = "按 model 分账";
            break;
        case ParsedUsageCommand::By::Purpose:
            table = &model.aggregate.by_purpose;
            table_title = "按 purpose 分账";
            break;
        case ParsedUsageCommand::By::Run:
            table = &model.aggregate.by_run;
            table_title = "按 run 分账";
            break;
        case ParsedUsageCommand::By::Outcome:
            table = &model.aggregate.by_outcome;
            table_title = "按 outcome 分账";
            break;
        case ParsedUsageCommand::By::None:
            break;
    }
    if (table != nullptr && table_title != nullptr) {
        lines.push_back(std::string("  ") + table_title + "(占比分母:input+output token):");
        for (const auto& row : *table) {
            const UsageTotals& t = row.totals;
            std::ostringstream out;
            out << "    " << row.label << "  " << SharePercent(t.total_billed_shape_tokens,
                                                               totals.total_billed_shape_tokens)
                << "% · " << t.requests_total << " 笔";
            if (t.requests_retry > 0) {
                out << "(重试 " << t.requests_retry << ")";
            }
            if (t.requests_unknown > 0) {
                out << "[" << t.requests_unknown << " 笔 unknown]";
            }
            out << " · 输入 " << lubancode::cli::FormatTokenCount(t.total_input_tokens) << "(读 "
                << lubancode::cli::FormatTokenCount(t.cache_read_tokens) << "/写 "
                << lubancode::cli::FormatTokenCount(t.cache_creation_tokens) << ")"
                << " · 输出 " << lubancode::cli::FormatTokenCount(t.output_tokens);
            if (t.requests_priced > 0) {
                out << " · " << FormatMicrosAmount(t.cost_micros, "$");
            }
            lines.push_back(out.str());
        }
    }

    // 缺口点名:projector warnings 透传,最多 5 条,超了计数。
    if (!model.aggregate.warnings.empty()) {
        lines.push_back("  缺口点名");
        const std::size_t shown = std::min<std::size_t>(model.aggregate.warnings.size(), 5);
        for (std::size_t i = 0; i < shown; ++i) {
            lines.push_back("    " + model.aggregate.warnings[i]);
        }
        if (model.aggregate.warnings.size() > shown) {
            std::ostringstream out;
            out << "    …另有 " << (model.aggregate.warnings.size() - shown) << " 条";
            lines.push_back(out.str());
        }
    }
    return lines;
}

nlohmann::json BuildUsageReportJson(const UsageReportModel& model) {
    nlohmann::json pricing;
    if (model.pricing.has_value()) {
        pricing = nlohmann::json{{"id", model.pricing->id},
                                 {"currency", model.pricing->currency},
                                 {"effective_from", model.pricing->effective_from}};
    } else {
        pricing = nlohmann::json{{"note", model.pricing_note}};
    }
    const char* by = nullptr;
    switch (model.by) {
        case ParsedUsageCommand::By::Model: by = "model"; break;
        case ParsedUsageCommand::By::Purpose: by = "purpose"; break;
        case ParsedUsageCommand::By::Run: by = "run"; break;
        case ParsedUsageCommand::By::Outcome: by = "outcome"; break;
        case ParsedUsageCommand::By::None: by = ""; break;
    }
    return nlohmann::json{
        {"schema", "lubancode.usage.report"},
        {"schema_version", 1},
        {"session_id", model.session_id},
        {"workspace_key", model.workspace_key},
        {"session_status", model.status},
        {"provisional", model.provisional},
        {"pricing", pricing},
        {"by", by},
        {"aggregate", model.aggregate.ToJson()}};
}

// ---------------- 执行(IO) ----------------

LoadedPricing LoadPricingTable(const std::optional<std::string>& home_lubancode) {
    LoadedPricing loaded;
    loaded.note = "未配价格表";
    if (!home_lubancode.has_value()) {
        return loaded;
    }
    const std::filesystem::path path =
        std::filesystem::path(*home_lubancode) / "pricing.json";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return loaded;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        loaded.note = "价格表打不开(" + path.filename().string() + "),按未配处理";
        return loaded;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const auto parsed = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (parsed.is_discarded()) {
        loaded.note = "价格表不是合法 JSON(" + path.filename().string() + "),按未配处理";
        return loaded;
    }
    std::string error;
    auto table = lubancode::accounting::PricingTable::FromJsonStrict(parsed, &error);
    if (!table.has_value()) {
        loaded.note = "价格表不合合同(" + error + "),按未配处理";
        return loaded;
    }
    loaded.table = std::move(table);
    loaded.note = loaded.table->id.empty() ? path.filename().string() : loaded.table->id;
    return loaded;
}

void HandleUsageCommand(const std::string& args, const UsageCommandContext& context) {
    using lubancode::cli::TermOut;
    const ParsedUsageCommand parsed = ParseUsageCommand(args);
    if (parsed.invalid) {
        TermOut() << context.theme.error << lubancode::cli::tr("cmd.usage.unknown_arg") << " ["
                  << parsed.bad_word << "]\n"
                  << lubancode::cli::tr("cmd.usage.usage_line") << "\n"
                  << context.theme.reset << "\n";
        TermOut().flush();
        return;
    }
    if (!parsed.later_scope.empty()) {
        // day/week/workspace/all:A5 的 insights 管线管跨场汇总,本批明说不冒充。
        TermOut() << lubancode::cli::tr("cmd.usage.later_scope") << " [" << parsed.later_scope
                  << "]\n";
        TermOut().flush();
        return;
    }

    // ---- flag 关:Trajectory 一笔没记,账未开;降级给内存粗账并明示口径 ----
    if (context.trajectory == nullptr) {
        nlohmann::json fallback = nlohmann::json{
            {"schema", "lubancode.usage.report"},
            {"schema_version", 1},
            {"source", "memory_fallback"},
            {"note", "features.trajectory off: no journal, memory ledger only"}};
        if (context.memory_ledger != nullptr) {
            nlohmann::json roles = nlohmann::json::array();
            for (const auto& [role, entry] : context.memory_ledger->by_role()) {
                roles.push_back(nlohmann::json{{"role", lubancode::agent::ToString(role)},
                                               {"calls", entry.calls},
                                               {"input_tokens", entry.input_tokens},
                                               {"output_tokens", entry.output_tokens},
                                               {"last_model", entry.last_model},
                                               {"usage_reported", entry.reported}});
            }
            fallback["roles"] = std::move(roles);
        }
        if (parsed.json) {
            TermOut() << fallback.dump(2) << "\n";
            TermOut().flush();
            return;
        }
        TermOut() << context.theme.stats << lubancode::cli::tr("cmd.usage.flag_off") << "\n"
                  << context.theme.reset;
        if (context.memory_ledger != nullptr) {
            for (const auto& line : context.memory_ledger->ReportLines()) {
                TermOut() << "  " << line << "\n";
            }
        }
        TermOut() << lubancode::cli::tr("cmd.usage.memory_caveat") << "\n";
        TermOut().flush();
        return;
    }

    // ---- flag 开:Journal 实测账 ----
    std::filesystem::path session_dir;
    if (parsed.scope == ParsedUsageCommand::Scope::NamedSession) {
        session_dir = context.sessions_root / parsed.session_id;
        if (!std::filesystem::is_directory(session_dir)) {
            TermOut() << context.theme.error
                      << lubancode::cli::trf("cmd.usage.session_not_found", parsed.session_id)
                      << "\n"
                      << context.theme.reset << "\n";
            TermOut().flush();
            return;
        }
    } else {
        session_dir = context.trajectory->session_dir();
    }

    lubancode::accounting::SessionUsageRead read =
        lubancode::accounting::ReadSessionUsage(session_dir);
    if (!read.ok) {
        TermOut() << context.theme.error << read.message << "\n" << context.theme.reset << "\n";
        TermOut().flush();
        return;
    }

    UsageReportModel model;
    model.session_id = read.session_id;
    model.workspace_key = read.workspace_key;
    model.status = read.status;
    // active session 恒未封口;指定 session 看 session.json(§14.2:未封口
    // 读高水位,标 provisional)。
    model.provisional = parsed.scope == ParsedUsageCommand::Scope::ActiveSession || !read.sealed();
    const LoadedPricing pricing = LoadPricingTable(context.home_lubancode);
    model.pricing = pricing.table;
    model.pricing_note = pricing.note;
    ApplyCostEstimates(read.samples, pricing.table);
    model.aggregate = lubancode::accounting::AggregateUsage(read.samples);
    model.aggregate.warnings = std::move(read.warnings);
    model.by = parsed.by;

    if (parsed.json) {
        TermOut() << BuildUsageReportJson(model).dump(2) << "\n";
        TermOut().flush();
        return;
    }
    for (const auto& line : FormatUsageReport(model)) {
        TermOut() << line << "\n";
    }
    TermOut().flush();
}

// 命令分派注册制:/usage 的分派位。只装材料,正戏在上面。
CommandFlow HandleSlashUsage(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    UsageCommandContext context{*ctx.theme};
    context.trajectory = ctx.trajectory;
    if (ctx.trajectory != nullptr) {
        // sessions_root = active session 目录的上一层(sessions/);指定
        // session 在同一 workspace 下找(跨 workspace 是 /usage all 的事)。
        context.sessions_root = ctx.trajectory->session_dir().parent_path();
    }
    context.memory_ledger =
        ctx.model_router != nullptr ? &ctx.model_router->ledger() : nullptr;
    context.home_lubancode =
        ctx.home_lubancode != nullptr ? *ctx.home_lubancode : std::optional<std::string>{};
    HandleUsageCommand(parsed.args, context);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
