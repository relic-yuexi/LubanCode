// /usage 的执行体(Token 账本单 A2)。分派位在文件尾,纯函数在前——解析、
// 计价、渲染都不碰 IO,单测直接钉;IO 只有读 Journal、读价格表、打印。
#include "app/commands/usage_commands.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "accounting/cost_estimator.hpp"
#include "accounting/purpose.hpp"
#include "agent/model_router.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/terminal_frame.hpp"  // frame::*(TUI 排版批 5c:报告/分账表渲染段)
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "platform/console.hpp"  // GetScreenInfo:/usage 的框宽同一把尺
#include "runtime/trajectory_session.hpp"

namespace lubancode::app {
namespace {

namespace frame = lubancode::cli::frame;

using lubancode::cli::TermOut;

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

// TUI 排版批 5c(/usage)的公共小件,与批 4 insights_commands.cpp 同款:
// 渲染段只调 cli::frame::* 三助手;报告正文是既有硬编码中文,一字不添不改;
// tr()/trf() 的通知句按句内冒号拆两列(批 1 裁量 SentenceField)。

int UsageFrameWidth() {
    if (const auto info = lubancode::platform::GetScreenInfo()) {
        return info->width;
    }
    return 0;
}

std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

frame::Field SentenceField(const std::string& sentence,
                           frame::FieldAccent accent = frame::FieldAccent::None) {
    const std::size_t colon = sentence.find(':');
    if (colon == std::string::npos) {
        return frame::Field{"", sentence, accent};
    }
    return frame::Field{TrimAscii(sentence.substr(0, colon)), TrimAscii(sentence.substr(colon + 1)),
                        accent};
}

// 引导下文的尾冒号剥掉(批 2 裁量 3):标题化时 "按 model 分账(...):" 的尾
// 冒号是废话。全角冒号是三字节 UTF-8,不能当 char 字面量比(clang 报
// character too large),按字节串后缀比。
std::string StripTrailingColon(std::string text) {
    static const std::string kFullWidthColon = "\xef\xbc\x9a";  // 全角冒号
    while (true) {
        if (!text.empty() && text.back() == ':') {
            text.pop_back();
            continue;
        }
        if (text.size() >= kFullWidthColon.size() &&
            text.compare(text.size() - kFullWidthColon.size(), kFullWidthColon.size(),
                         kFullWidthColon) == 0) {
            text.resize(text.size() - kFullWidthColon.size());
            continue;
        }
        break;
    }
    return text;
}

// 反馈/错误通知:句子进键值对框(句内冒号拆列;错误走 error 语义色)。
void PrintNotice(const lubancode::cli::Theme& theme, const std::vector<std::string>& sentences,
                 frame::FieldAccent accent = frame::FieldAccent::None) {
    std::vector<frame::Field> fields;
    for (const std::string& sentence : sentences) {
        fields.push_back(SentenceField(sentence, accent));
    }
    for (const std::string& line :
         frame::RenderKeyValues({}, fields, theme, frame::Light(), UsageFrameWidth())) {
        TermOut() << line << "\n";
    }
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

std::vector<std::string> FormatUsageReport(const UsageReportModel& model,
                                           const lubancode::cli::Theme& theme, int width) {
    using lubancode::accounting::UsageTotals;
    std::vector<std::string> lines;
    const UsageTotals& totals = model.aggregate.totals;

    // 标题:场次 + 成色(未封口恒 provisional,§10.1)。
    std::string title = "Usage · " + model.session_id;
    if (model.provisional) {
        title += "(未封口 provisional)";
    }

    if (totals.requests_total == 0) {
        // 空账不猜:一句进键值对框(批 4 空态同款)。
        for (const std::string& line : frame::RenderKeyValues(
                 title,
                 {frame::Field{"", "这场 session 还没有模型请求账(Journal 里一笔没有)——不猜。"}},
                 theme, frame::Light(), width)) {
            lines.push_back(line);
        }
        return lines;
    }

    // 主报告:先收集 fields 再一把进键值对框(单子合同第 4 条);八节节名
    // 沿用旧排版标签,句子一字不改——旧平铺的手工补空对齐交给助手。
    std::vector<frame::Field> fields;
    // 覆盖:unknown 单列,不冒充(§14.3)。
    {
        std::ostringstream out;
        out << totals.requests_with_usage << "/" << totals.requests_total
            << " 笔有 provider usage";
        if (totals.requests_unknown > 0) {
            out << " · " << totals.requests_unknown << " 笔 unknown(未报,不折 0)";
        }
        if (totals.requests_retry > 0) {
            out << " · 重试 " << totals.requests_retry << " 笔(各记各账)";
        }
        fields.push_back(frame::Field{"覆盖", out.str()});
    }
    // 输入(§7.4)。
    {
        std::ostringstream out;
        out << lubancode::cli::FormatTokenCount(totals.input_tokens);
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
        fields.push_back(frame::Field{"输入", out.str()});
    }
    // 输出:reasoning 是子集,注明(§四.4)。
    {
        std::ostringstream out;
        out << lubancode::cli::FormatTokenCount(totals.output_tokens);
        if (totals.reasoning_tokens > 0) {
            out << " · 推理 " << lubancode::cli::FormatTokenCount(totals.reasoning_tokens)
                << "(已含在输出,不另加)";
        }
        fields.push_back(frame::Field{"输出", out.str()});
    }
    // 模型与用途:份额行,分母注明(§7.4"任何比例都要注明分母")。
    {
        const std::int64_t whole = totals.total_billed_shape_tokens;
        fields.push_back(frame::Field{"模型", ShareLine(model.aggregate.by_model, whole, 3) +
                                               "(按 input+output token 占比)"});
        fields.push_back(frame::Field{"用途", ShareLine(model.aggregate.by_purpose, whole, 4) +
                                               "(按 input+output token 占比)"});
    }
    // 费用(§6.3 四条线):没配表照样报 token,费用 not_priced。
    {
        std::ostringstream out;
        if (model.pricing.has_value()) {
            out << FormatMicrosAmount(totals.cost_micros, "$") << " · 价格表 "
                << model.pricing->id << " · 本地估算,非账单";
            if (totals.requests_priced < totals.requests_with_usage) {
                out << " · 命中 " << totals.requests_priced << "/"
                    << totals.requests_with_usage << " 笔(其余 not_priced)";
            }
        } else {
            out << "not_priced(" << model.pricing_note << ";token 照报,不估不猜)";
        }
        fields.push_back(frame::Field{"估算费用", out.str()});
    }
    // cache 观察:只报 observed,不判罪(§7.2)。
    {
        const auto& cache = model.aggregate.cache;
        std::ostringstream out;
        out << "epoch 重建 " << cache.expected_rebuild_events << " 次 · 疑似未命中 "
            << cache.unexpected_miss_candidates << " 笔(候选:TTL 过期/端不稳也长这模样) · 前缀改写 "
            << cache.append_only_breaks << " 笔";
        if (cache.epoch_unlabeled > 0) {
            out << " · 无 epoch 标注 " << cache.epoch_unlabeled << " 笔";
        }
        fields.push_back(frame::Field{"cache 观察", out.str()});
    }
    // 成色:账的来历说明,不混"异常"语气。
    {
        std::ostringstream out;
        out << model.aggregate.run_ids.size() << " 条 run";
        if (model.format == "v3") {
            out << " · v3 账";
        } else if (model.format == "v2") {
            out << " · v2 旧格式账";
        }
        if (model.aggregate.legacy_samples > 0) {
            out << " · v1 旧账 " << model.aggregate.legacy_samples << " 笔";
        }
        if (model.aggregate.incomplete_linkage_samples > 0) {
            out << " · 断链 " << model.aggregate.incomplete_linkage_samples << " 笔";
        }
        out << " · session status=" << model.status;
        fields.push_back(frame::Field{"成色", out.str()});
    }
    for (const std::string& line :
         frame::RenderKeyValues(title, fields, theme, frame::Light(), width)) {
        lines.push_back(line);
    }

    // --by 分账表(要哪张打哪张;份额分母同上)。列头是数据 schema 名
    //(批 1 裁量),share/req/cost 数值列右对齐;行内句子按旧粒度拆列,
    // 一字不改。
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
    if (table != nullptr && table_title != nullptr && !table->empty()) {
        std::vector<frame::TableColumn> columns;
        columns.push_back(frame::TableColumn{"label"});
        columns.push_back(frame::TableColumn{"share", 0, /*align_right=*/true});
        columns.push_back(frame::TableColumn{"req", 0, /*align_right=*/true});
        columns.push_back(frame::TableColumn{"tokens"});
        columns.push_back(frame::TableColumn{"cost", 0, /*align_right=*/true});
        std::vector<frame::TableRow> rows;
        for (const auto& row : *table) {
            const UsageTotals& t = row.totals;
            std::ostringstream req;
            req << t.requests_total << " 笔";
            if (t.requests_retry > 0) {
                req << "(重试 " << t.requests_retry << ")";
            }
            if (t.requests_unknown > 0) {
                req << "[" << t.requests_unknown << " 笔 unknown]";
            }
            std::ostringstream tokens;
            tokens << "输入 " << lubancode::cli::FormatTokenCount(t.total_input_tokens) << "(读 "
                   << lubancode::cli::FormatTokenCount(t.cache_read_tokens) << "/写 "
                   << lubancode::cli::FormatTokenCount(t.cache_creation_tokens) << ")"
                   << " · 输出 " << lubancode::cli::FormatTokenCount(t.output_tokens);
            rows.push_back(frame::TableRow{
                {row.label,
                 std::to_string(
                     SharePercent(t.total_billed_shape_tokens, totals.total_billed_shape_tokens)) +
                     "%",
                 req.str(), tokens.str(),
                 t.requests_priced > 0 ? FormatMicrosAmount(t.cost_micros, "$") : std::string()},
                {}});
        }
        for (const std::string& line : frame::RenderTable(
                 StripTrailingColon(std::string(table_title) +
                                    "(占比分母:input+output token):"),
                 columns, rows, theme, frame::Light(), width)) {
            lines.push_back(line);
        }
    }

    // 缺口点名:projector warnings 透传,最多 5 条,超了计数(空 key 续行,
    // 值列同栏)。
    if (!model.aggregate.warnings.empty()) {
        std::vector<frame::Field> warn_fields;
        const std::size_t shown = std::min<std::size_t>(model.aggregate.warnings.size(), 5);
        for (std::size_t i = 0; i < shown; ++i) {
            warn_fields.push_back(frame::Field{"", model.aggregate.warnings[i]});
        }
        if (model.aggregate.warnings.size() > shown) {
            std::ostringstream out;
            out << "…另有 " << (model.aggregate.warnings.size() - shown) << " 条";
            warn_fields.push_back(frame::Field{"", out.str()});
        }
        for (const std::string& line :
             frame::RenderKeyValues("缺口点名", warn_fields, theme, frame::Light(), width)) {
            lines.push_back(line);
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
        {"format", model.format},
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
        PrintNotice(*context.theme,
                    {lubancode::cli::tr("cmd.usage.unknown_arg") + " [" + parsed.bad_word + "]",
                     lubancode::cli::tr("cmd.usage.usage_line")},
                    frame::FieldAccent::Error);
        TermOut().flush();
        return;
    }
    if (!parsed.later_scope.empty()) {
        // day/week/workspace/all:A5 的 insights 管线管跨场汇总,本批明说不冒充。
        PrintNotice(*context.theme, {lubancode::cli::tr("cmd.usage.later_scope") + " [" +
                                         parsed.later_scope + "]"});
        TermOut().flush();
        return;
    }

    // ---- flag 关:Trajectory 一笔没记,账未开;降级给内存粗账并明示口径 ----
    if (context.trajectory == nullptr) {
        nlohmann::json fallback = nlohmann::json{
            {"schema", "lubancode.usage.report"},
            {"schema_version", 1},
            {"source", "memory_fallback"},
            {"note", "journal unavailable: memory ledger only"}};
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
        PrintNotice(*context.theme, {lubancode::cli::tr("cmd.usage.flag_off")},
                    frame::FieldAccent::Stats);
        if (context.memory_ledger != nullptr) {
            // 内存粗账是 ModelUsageLedger 的现成行,长正文不塞框(批 1 裁量 3)。
            for (const auto& line : context.memory_ledger->ReportLines()) {
                TermOut() << "  " << line << "\n";
            }
        }
        PrintNotice(*context.theme, {lubancode::cli::tr("cmd.usage.memory_caveat")},
                    frame::FieldAccent::Muted);
        TermOut().flush();
        return;
    }

    // ---- flag 开:Journal 实测账 ----
    std::filesystem::path session_dir;
    if (parsed.scope == ParsedUsageCommand::Scope::NamedSession) {
        session_dir = context.sessions_root / parsed.session_id;
        if (!std::filesystem::is_directory(session_dir)) {
            PrintNotice(*context.theme,
                        {lubancode::cli::trf("cmd.usage.session_not_found", parsed.session_id)},
                        frame::FieldAccent::Error);
            TermOut().flush();
            return;
        }
    } else {
        session_dir = context.trajectory->session_dir();
    }

    lubancode::accounting::SessionUsageRead read =
        lubancode::accounting::ReadSessionUsage(session_dir);
    if (!read.ok) {
        PrintNotice(*context.theme, {read.message}, frame::FieldAccent::Error);
        TermOut().flush();
        return;
    }

    UsageReportModel model;
    model.session_id = read.session_id;
    model.workspace_key = read.workspace_key;
    model.status = read.status;
    model.format = read.format;
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
    for (const auto& line : FormatUsageReport(model, *context.theme, UsageFrameWidth())) {
        TermOut() << line << "\n";
    }
    TermOut().flush();
}

// 命令分派注册制:/usage 的分派位。HC-06(材料收窄,第二小批)起只吃
// 窄材料——装材料(sessions_root 派生、内存粗账递手)在组合根折好,
// 正戏在上面。
CommandFlow HandleSlashUsage(const UsageCommandContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    HandleUsageCommand(parsed.args, ctx);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
