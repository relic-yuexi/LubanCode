// EvolutionCoordinator 的实现(自进化闭环阶段 2)。唯一写口:候选目录里
// 每一份文件的落笔都从这里走,别处不许碰。
#include "evolution/coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "agent/agent_definition.hpp"
#include "evolution/adapters.hpp"
#include "evolution/drafter.hpp"
#include "evolution/eval.hpp"
#include "hooks/hash.hpp"
#include "package/component.hpp"  // ParseMcpComponentYaml(diff 页读 MCP 草稿)
#include "platform/paths.hpp"
#include "workflow/parser.hpp"

namespace lubancode::evolution {

namespace {

std::string PathToUtf8(const std::filesystem::path& path) {
    return lubancode::platform::PathToUtf8(path);
}

// UTF-8 边界截断(命令层 Ellipsize 同款:从切口往回退,不吐残缺多字节)。
std::string TruncateUtf8(const std::string& text, std::size_t cap) {
    if (text.size() <= cap) {
        return text;
    }
    std::size_t end = cap;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end) + "…";
}

// 按行切(吞 \r;末行无换行也收)。
std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

// 本地日期 "YYYYMMDD"(候选 id 的日期段按本地时区)。
std::string LocalDateCompact() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d", &local);
    return buffer;
}

// UTC 的 ISO 8601("2026-08-28T09:30:00Z")。evolution/approval 的时刻字段
// 用它——跨机器对账不掺时区。
std::string IsoNowUtc() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

// 全仓按日编号:cand-<date>-NNN,NNN 取当日整个候选仓里的最大号 +1。
// (夹具 cand-20260828-001/002 即此口径——编号跨包递增。)
std::string NextCandidateId(const std::filesystem::path& root, const std::string& date) {
    const std::string prefix = "cand-" + date + "-";
    int max_seq = 0;
    std::error_code ec;
    if (std::filesystem::exists(root, ec)) {
        for (const auto& package_entry : std::filesystem::directory_iterator(root, ec)) {
            if (!package_entry.is_directory()) {
                continue;
            }
            std::error_code inner_ec;
            for (const auto& entry : std::filesystem::directory_iterator(package_entry.path(), inner_ec)) {
                const std::string name = PathToUtf8(entry.path().filename());
                if (name.rfind(prefix, 0) != 0) {
                    continue;
                }
                const std::string digits = name.substr(prefix.size());
                if (digits.empty() ||
                    digits.find_first_not_of("0123456789") != std::string::npos) {
                    continue;
                }
                max_seq = std::max(max_seq, std::atoi(digits.c_str()));
            }
        }
    }
    char buffer[16]{};  // %03d 最少三位,序号破千也不许截断(GCC -Wformat-truncation)
    std::snprintf(buffer, sizeof(buffer), "%03d", max_seq + 1);
    return prefix + buffer;
}

bool WriteFileBytes(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << content;
    return file.good();
}

bool AppendFileLine(const std::filesystem::path& path, const std::string& line) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file.is_open()) {
        return false;
    }
    file << line << "\n";
    return file.good();
}

std::optional<std::string> ReadFileText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// 状态账追加一笔(先查迁移表,非法即拒——写口只有这里,规矩也只有这里)。
std::expected<CandidateStateEntry, std::string> AppendState(const std::filesystem::path& candidate_dir,
                                                            CandidateState from,
                                                            CandidateState to,
                                                            const std::string& actor,
                                                            const std::string& reason,
                                                            const std::string& fingerprint = std::string()) {
    if (!IsValidCandidateTransition(from, to)) {
        return std::unexpected("非法迁移: " + ToString(from) + " -> " + ToString(to));
    }
    // seq = 既有行数 + 1(坏行也占一行号,只求单调,不求无洞)。
    std::int64_t seq = 1;
    if (const auto text = ReadFileText(candidate_dir / "state.jsonl"); text.has_value()) {
        seq = static_cast<std::int64_t>(std::count(text->begin(), text->end(), '\n')) + 1;
    }
    CandidateStateEntry entry;
    entry.seq = seq;
    entry.from = from;
    entry.to = to;
    entry.actor = actor;
    entry.reason = reason;
    entry.at = IsoNowUtc();
    entry.fingerprint = fingerprint;
    if (!AppendFileLine(candidate_dir / "state.jsonl", SerializeStateEntry(entry))) {
        return std::unexpected("写状态账失败: " + PathToUtf8(candidate_dir / "state.jsonl"));
    }
    return entry;
}

// SKILL 正文摘要:frontmatter 之后按节标题收要点行,每节至多两行,全文
// 至多 30 行——diff 页看形状,不看全文。节多在后面的(排错)也要露头,
// 所以行帽放宽,不掐尾巴。
std::string SummarizeSkillBody(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    std::string current_header;
    std::vector<std::string> lines;
    bool in_frontmatter = false;
    bool first_line = true;
    while (std::getline(stream, line) && lines.size() < 30) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (first_line && line == "---") {
            in_frontmatter = true;
            first_line = false;
            continue;
        }
        first_line = false;
        if (in_frontmatter) {
            if (line == "---") {
                in_frontmatter = false;
            }
            continue;
        }
        if (line.rfind("## ", 0) == 0) {
            current_header = line.substr(3);
            lines.push_back("节 " + current_header);
            continue;
        }
        if (line.empty() || current_header.empty()) {
            continue;
        }
        // 同一节只留前两条非空正文行。
        std::size_t same_section = 0;
        for (std::size_t i = lines.size(); i > 0; --i) {
            if (lines[i - 1].rfind("节 ", 0) == 0) {
                break;
            }
            ++same_section;
        }
        if (same_section < 2) {
            std::string text = line;
            if (text.size() > 60) {
                // 与命令层 Ellipsize 同一口径:从第 60 字节往回退,退掉被切
                // 半个的多字节字符,不吐残缺 UTF-8。
                std::size_t end = 60;
                while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
                    --end;
                }
                text = text.substr(0, end) + "…";
            }
            lines.push_back("  " + text);
        }
    }
    std::string out;
    for (const std::string& item : lines) {
        out += item + "\n";
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 评测(阶段 3)。写口规矩不变:eval-results.jsonl 只追加,state.jsonl 迁移
// 只走 AppendState。静态门全绿才 drafted->validated;五道门入账才
// validated->evaluated(评测有 fail 也算"跑完",失败记在账上,不挡迁移)。
// ---------------------------------------------------------------------------

// 从候选目录直接装一份盘点(CI 按目录评测,不经候选仓扫描)。
std::optional<CandidateSummary> LoadCandidateAt(const std::filesystem::path& candidate_dir) {
    const auto record_text = ReadFileText(candidate_dir / "evolution.json");
    if (!record_text.has_value()) {
        return std::nullopt;  // 残缺目录,不算候选
    }
    auto record = ParseEvolutionRecord(*record_text);
    if (!record.has_value()) {
        return std::nullopt;
    }
    CandidateSummary summary;
    summary.package_id = record->package_id;
    summary.candidate_id = record->candidate_id;
    summary.dir = candidate_dir;
    summary.state = CandidateStore::ReadState(candidate_dir);
    summary.record = record;
    if (const auto text = ReadFileText(candidate_dir / "approval.json"); text.has_value()) {
        summary.approval = ParseApprovalRecord(*text);
    }
    summary.content_hash = ComputeCandidateContentHash(candidate_dir / "package");
    return summary;
}

std::expected<EvolutionCoordinator::TestReport, std::string> EvolutionCoordinator::Test(
    const std::string& candidate_id, const TestOptions& options) {
    const auto found = store_.Find(candidate_id);
    if (!found.has_value()) {
        return std::unexpected("找不到候选 \"" + candidate_id + "\"(先 /evolve list 看)");
    }
    return TestDir(found->dir, options);
}

std::expected<EvolutionCoordinator::TestReport, std::string> EvolutionCoordinator::TestDir(
    const std::filesystem::path& candidate_dir, const TestOptions& options) {
    const auto found = LoadCandidateAt(candidate_dir);
    if (!found.has_value()) {
        return std::unexpected("目录不是候选(缺 evolution.json 或解析不过): " +
                               PathToUtf8(candidate_dir));
    }
    const CandidateSummary& summary = *found;
    if (summary.content_hash.empty()) {
        return std::unexpected("候选缺 package/ 目录,复算不出内容哈希: " +
                               PathToUtf8(candidate_dir));
    }
    CandidateState state = summary.state;
    if (IsTerminalCandidateState(state)) {
        return std::unexpected("候选 \"" + summary.candidate_id + "\" 已在终态 " + ToString(state) +
                               ",不再评测(账保留,不删)");
    }

    TestReport report;
    report.candidate_id = summary.candidate_id;
    report.package_id = summary.package_id;
    report.content_hash = summary.content_hash;
    report.candidate_dir = candidate_dir;
    report.state_before = ToString(state);

    // ---- 计划:hash 对不上即作废(契约),不静默拿旧计划评新内容 ----
    std::optional<EvalPlan> plan;
    if (const auto plan_text = ReadFileText(candidate_dir / "eval-plan.json");
        plan_text.has_value()) {
        auto parsed = ParseEvalPlan(*plan_text);
        if (parsed.has_value()) {
            if (parsed->content_hash != summary.content_hash) {
                return std::unexpected("eval-plan.json 绑定的内容哈希与当前候选对不上(内容变"
                                       "过,计划作废;重做候选,或按新哈希重写计划)");
            }
            if (parsed->candidate_id != summary.candidate_id) {
                return std::unexpected("eval-plan.json 的 candidate_id 与候选对不上: " +
                                       parsed->candidate_id);
            }
            plan = std::move(*parsed);
            report.plan_loaded = true;
        } else {
            report.plan_error = parsed.error();
        }
    } else {
        report.plan_error = "缺 eval-plan.json(propose 会落一份最小计划)";
    }

    std::vector<EvalResultLine> to_append;

    // ---- 门一/门二(静态):Package doctor + 组件原生 validator + 密钥/绝对路径扫描 ----
    {
        const auto started = std::chrono::steady_clock::now();
        const StaticGateResult static_result = RunStaticGate(candidate_dir / "package");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        report.static_gate = static_result;
        EvalResultLine line;
        line.gate = "static";
        line.task_id = "static";
        line.candidate_id = summary.candidate_id;
        line.content_hash = summary.content_hash;
        line.outcome = static_result.pass() ? "pass" : "fail";
        line.metrics.success_rate = static_result.pass() ? 1.0 : 0.0;
        line.metrics.acceptance_rate = static_result.pass() ? 1.0 : 0.0;
        line.metrics.wall_clock_ms = elapsed;
        line.findings = static_result.findings;
        for (std::size_t i = 0; i < static_result.errors.size() && i < 8; ++i) {
            line.notes.push_back(static_result.errors[i]);
        }
        line.unverified.push_back("compat-range");  // doctor 没喂当前版本,兼容范围没查
        line.complexity = ComputeComplexityCost(candidate_dir / "package");  // 阶段 5:复杂度代价
        line.recorded_at = IsoNowUtc();
        to_append.push_back(std::move(line));
    }

    // ---- 门三/门四(来源回放 + 留出):确定性检查代跑,不起真模型 ----
    if (plan.has_value()) {
        for (const EvalTask& task : plan->replay) {
            TaskRunResult run = RunEvalTask("replay", task, summary.candidate_id,
                                            summary.content_hash, candidate_dir, *plan);
            report.fixture_missing_any = report.fixture_missing_any || run.fixture_missing;
            to_append.push_back(std::move(run.line));
        }
        for (const EvalTask& task : plan->holdout) {
            TaskRunResult run = RunEvalTask("holdout", task, summary.candidate_id,
                                            summary.content_hash, candidate_dir, *plan);
            report.fixture_missing_any = report.fixture_missing_any || run.fixture_missing;
            to_append.push_back(std::move(run.line));
        }
    }

    // ---- 门五(基线对照):父版或裸 Agent 的确定性指标账 ----
    if (plan.has_value()) {
        EvalResultLine line;
        line.gate = "baseline";
        line.task_id = "baseline";
        line.candidate_id = summary.candidate_id;
        line.content_hash = summary.content_hash;
        line.baseline_ref = plan->baseline_ref;
        line.recorded_at = IsoNowUtc();

        std::optional<BaselineFixture> fixture;
        if (!plan->baseline_fixture.empty()) {
            const std::filesystem::path fixture_path =
                candidate_dir / lubancode::platform::Utf8ToPath(plan->baseline_fixture);
            if (const auto text = ReadFileText(fixture_path); text.has_value()) {
                fixture = ParseBaselineFixture(*text);
                if (!fixture.has_value()) {
                    line.notes.push_back("基线夹具解析不过: " + plan->baseline_fixture);
                }
            } else {
                line.notes.push_back("基线夹具缺失: " + plan->baseline_fixture);
                report.fixture_missing_any = true;
                line.unverified.push_back("fixture-missing");
            }
        } else {
            line.unverified.push_back("baseline-metrics");  // 没附指标账,代价对照缺
        }

        // CI 的 --baseline <dir>:给父版包补静态对照与哈希对账。
        if (options.baseline_package_dir.has_value()) {
            const std::filesystem::path& baseline_dir = *options.baseline_package_dir;
            const StaticGateResult baseline_static = RunStaticGate(baseline_dir);
            CheckResult check;
            check.kind = "baseline-static";
            check.detail = PathToUtf8(baseline_dir);
            check.pass = baseline_static.pass();
            if (!check.pass) {
                std::string note = "父版静态门未过";
                if (!baseline_static.errors.empty()) {
                    note += ": " + baseline_static.errors.front();
                }
                check.note = note;
            }
            line.checks.push_back(std::move(check));
            if (summary.record.has_value() && summary.record->parent.has_value()) {
                const std::string parent_hash =
                    ComputeCandidateContentHash(baseline_dir);
                if (!parent_hash.empty()) {
                    if (parent_hash == summary.record->parent->content_hash) {
                        line.notes.push_back("父版哈希对上(" + parent_hash.substr(0, 19) + ")");
                    } else {
                        line.notes.push_back("父版哈希对不上:演化账记 " +
                                             summary.record->parent->content_hash.substr(0, 19) +
                                             ",盘上是 " + parent_hash.substr(0, 19));
                    }
                }
            }
        }

        // 对照判词:候选本次的确定性结果 vs 基线指标账。判不了(无夹具/
        // 候选无可执行检查)就 skipped,不硬判。
        int rate_rows = 0;
        double success_sum = 0.0;
        double acceptance_sum = 0.0;
        for (const EvalResultLine& appended : to_append) {
            if ((appended.gate == "replay" || appended.gate == "holdout") &&
                appended.outcome != "skipped") {
                success_sum += appended.metrics.success_rate;
                acceptance_sum += appended.metrics.acceptance_rate;
                ++rate_rows;
            }
        }
        if (fixture.has_value()) {
            line.metrics = fixture->metrics;
            line.unverified = fixture->unverified;
            if (!fixture->task_id.empty()) {
                line.task_id = fixture->task_id;
            }
            if (rate_rows == 0) {
                line.outcome = "skipped";
                line.notes.push_back("候选侧没有可执行检查,对照无从判");
            } else {
                const double success = success_sum / rate_rows;
                const double acceptance = acceptance_sum / rate_rows;
                const bool not_worse = success >= fixture->metrics.success_rate &&
                                       acceptance >= fixture->metrics.acceptance_rate;
                line.outcome = not_worse ? "pass" : "fail";
                if (!not_worse) {
                    char rates[96]{};
                    std::snprintf(rates, sizeof(rates), "%.2f 对 %.2f", success,
                                  fixture->metrics.success_rate);
                    line.notes.push_back(std::string("候选确定性结果低于基线(成功率 ") + rates +
                                         ")");
                }
            }
        } else {
            line.outcome = "skipped";
            const bool fixture_broken = std::find(line.unverified.begin(), line.unverified.end(),
                                                  "fixture-missing") != line.unverified.end();
            line.notes.push_back(fixture_broken ? "基线夹具缺失" :
                                                  "基线没附确定性指标账,只做静态对照");
        }
        to_append.push_back(std::move(line));
    }

    // ---- 落账:只追加,seq 续着账面编(坏行也占号,只求单调) ----
    {
        std::int64_t seq = 0;
        if (const auto text = ReadFileText(candidate_dir / "eval-results.jsonl");
            text.has_value()) {
            for (const char c : *text) {
                if (c == '\n') {
                    ++seq;
                }
            }
        }
        for (EvalResultLine& line : to_append) {
            line.seq = ++seq;
            if (!AppendFileLine(candidate_dir / "eval-results.jsonl", SerializeEvalResultLine(line))) {
                return std::unexpected("写评测账失败: " +
                                       PathToUtf8(candidate_dir / "eval-results.jsonl"));
            }
            report.appended.push_back(line);
        }
    }

    // ---- 迁移:静态门全绿 -> validated;五道门入账 -> evaluated ----
    if (report.static_gate.pass() && state == CandidateState::Drafted) {
        const auto moved = AppendState(candidate_dir, CandidateState::Drafted,
                                       CandidateState::Validated, "user",
                                       "静态门全绿(doctor + 密钥/绝对路径扫描)");
        if (!moved.has_value()) {
            return std::unexpected(moved.error());
        }
        state = CandidateState::Validated;
        report.transitioned_validated = true;
    }
    if (state == CandidateState::Validated && report.plan_loaded) {
        const auto moved = AppendState(candidate_dir, CandidateState::Validated,
                                       CandidateState::Evaluated, "user",
                                       "评测五道门跑完,结果入账(账只追加)");
        if (!moved.has_value()) {
            return std::unexpected(moved.error());
        }
        state = CandidateState::Evaluated;
        report.transitioned_evaluated = true;
    }
    report.state_after = ToString(state);

    // ---- 汇总与退出码 ----
    report.run_summary = SummarizeEvalLedger(report.appended);
    report.ledger_summary =
        SummarizeEvalLedger(LoadEvalResults(candidate_dir / "eval-results.jsonl"));
    report.ledger_summary.has_holdout = report.ledger_summary.has_holdout ||
                                        (plan.has_value() && !plan->holdout.empty());
    if (plan.has_value()) {
        report.ledger_summary.baseline_kind = plan->baseline_kind;
    }
    report.exit_code = EvalExitCode(report.run_summary, report.plan_loaded,
                                    report.fixture_missing_any);
    return report;
}

EvolutionCoordinator::EvolutionCoordinator(std::filesystem::path candidates_root,
                                           ObservationStore* observations,
                                           std::filesystem::path store_root)
    : root_(std::move(candidates_root)),
      observations_(observations),
      store_(root_),
      // store 根缺省从候选仓的姊妹目录推:<home>/package-candidates 的
      // 旁边就是 <home>/package-store(README"晋升、灰度与回滚"的钉子)。
      versions_(store_root.empty() ? root_.parent_path() / "package-store" : std::move(store_root)) {}

// ---------------------------------------------------------------------------
// 批准、安装与回滚(阶段 4)。迁状态的笔只有这里;store 机械走 VersionStore。
// ---------------------------------------------------------------------------

// 批准页材料:身份、来源、改动、评测、权限、安装与回滚(README §十清单)。
std::expected<EvolutionCoordinator::ApprovalBrief, std::string>
EvolutionCoordinator::BuildApprovalBrief(const std::string& candidate_id) {
    const auto found = store_.Find(candidate_id);
    if (!found.has_value()) {
        return std::unexpected("找不到候选 \"" + candidate_id + "\"(先 /evolve list 看)");
    }
    if (!found->record.has_value()) {
        return std::unexpected("候选 \"" + candidate_id + "\" 的演化账读不出,批不得");
    }
    const EvolutionRecord& record = *found->record;
    const std::vector<EvalResultLine> rows = LoadEvalResults(found->dir / "eval-results.jsonl");
    if (rows.empty()) {
        return std::unexpected("评测账是空的:先 /evolve test " + candidate_id);
    }

    ApprovalBrief brief;
    brief.candidate_id = found->candidate_id;
    brief.package_id = found->package_id;
    brief.candidate_version = record.candidate_version;
    brief.content_hash = found->content_hash;
    if (record.parent.has_value()) {
        brief.parent_line = "父版 " + record.parent->version + " " + record.parent->content_hash;
    } else {
        brief.parent_line = "(无父版,与空对照)";
    }
    const auto join_sources = [](const std::vector<std::string>& ids, const char* label,
                                 std::vector<std::string>& out) {
        for (const std::string& id : ids) {
            out.push_back(std::string(label) + " " + id);
        }
    };
    join_sources(record.sources.run_ids, "run", brief.source_lines);
    join_sources(record.sources.goal_ids, "goal", brief.source_lines);
    join_sources(record.sources.recording_ids, "recording", brief.source_lines);
    join_sources(record.sources.memory_ids, "memory", brief.source_lines);
    join_sources(record.sources.user_feedback_ids, "user-feedback", brief.source_lines);
    brief.components_added = record.changes.components_added;
    brief.components_changed = record.changes.components_changed;
    brief.components_removed = record.changes.components_removed;
    brief.permissions_added = record.changes.permissions_added;
    brief.tools_added = record.changes.tools_added;
    brief.tier = "content-only";  // code-bearing 在 Approve 的门上明拒,进不了页
    brief.eval_summary = SummarizeEvalLedger(rows);
    // 复杂度代价(阶段 5):评测账带了照账;没带(旧候选)从盘上现盘。
    if (brief.eval_summary.has_value() && brief.eval_summary->complexity.has_value()) {
        brief.complexity = brief.eval_summary->complexity;
    } else {
        const ComplexityCost on_disk = ComputeComplexityCost(found->dir / "package");
        if (!on_disk.shape.empty()) {
            brief.complexity = on_disk;
        }
    }
    for (const EvalResultLine& row : rows) {
        if ((row.gate == "replay" || row.gate == "holdout") && !row.task_id.empty()) {
            // 任务样例:同 gate 同任务只记头一回(重跑评测账只追加)。
            std::string line = row.gate + " " + row.task_id;
            if (std::find(brief.eval_task_ids.begin(), brief.eval_task_ids.end(), line) ==
                brief.eval_task_ids.end()) {
                brief.eval_task_ids.push_back(std::move(line));
            }
        }
    }
    brief.install_dir_utf8 =
        PathToUtf8(versions_.VersionDir(found->package_id, "<版本>")) + "(package.yaml 的稳定版号)";
    if (record.parent.has_value()) {
        brief.rollback_target_line =
            "父版 " + record.parent->version + "(/evolve rollback " + found->package_id + ")";
    } else {
        brief.rollback_target_line =
            "无父版:/evolve rollback " + found->package_id + " 即撤下(版本与账保留)";
    }
    return brief;
}

std::expected<EvolutionCoordinator::ApproveResult, std::string> EvolutionCoordinator::Approve(
    const std::string& candidate_id) {
    const auto found = store_.Find(candidate_id);
    if (!found.has_value()) {
        return std::unexpected("找不到候选 \"" + candidate_id + "\"(先 /evolve list 看)");
    }
    const std::filesystem::path& candidate_dir = found->dir;
    CandidateState state = found->state;
    if (IsTerminalCandidateState(state)) {
        return std::unexpected("候选 \"" + candidate_id + "\" 已在终态 " + ToString(state) +
                               ",不再迁移(账保留,不删)");
    }
    if (state == CandidateState::Staged || state == CandidateState::Canary ||
        state == CandidateState::Active) {
        return std::unexpected("候选 \"" + candidate_id + "\" 已是 " + ToString(state) +
                               ";灰度走 /evolve use,晋升走 /evolve promote");
    }
    if (state == CandidateState::Observed || state == CandidateState::Drafted ||
        state == CandidateState::Validated) {
        return std::unexpected("候选 \"" + candidate_id + "\" 还在 " + ToString(state) +
                               ",先 /evolve test " + candidate_id + " 把五道门跑完");
    }
    if (!found->record.has_value() || !found->approval.has_value()) {
        return std::unexpected("候选 \"" + candidate_id + "\" 的演化账或批准账读不出,批不得");
    }

    // ---- 门一:哈希绑定。批准只认当前 content hash;文件变过即拒批。 ----
    if (found->content_hash.empty()) {
        return std::unexpected("候选缺 package/ 目录,复算不出内容哈希: " +
                               PathToUtf8(candidate_dir));
    }
    if (found->content_hash != found->approval->content_hash) {
        return std::unexpected(
            "内容变过:批准账绑的是 " + found->approval->content_hash.substr(0, 19) + ",当前复算是 " +
            found->content_hash.substr(0, 19) +
            "。旧评测与旧批准一并作废——重做候选(改一字即新候选),或按新哈希重写计划再 /evolve test");
    }
    const std::vector<EvalResultLine> rows = LoadEvalResults(candidate_dir / "eval-results.jsonl");
    if (rows.empty()) {
        return std::unexpected("评测账是空的:先 /evolve test " + candidate_id);
    }
    for (const EvalResultLine& row : rows) {
        if (row.content_hash != found->content_hash) {
            return std::unexpected("评测账绑的是旧哈希 " + row.content_hash.substr(0, 19) +
                                   ",当前候选是 " + found->content_hash.substr(0, 19) +
                                   "。旧评测作废——重做候选,或按新哈希重写计划再 /evolve test");
        }
    }

    // ---- 门二:档位分类。content-only 直接可批;code-bearing 首版明拒,
    //      指路 Package trust 流程(与信任门单各管各的账)。 ----
    if (found->approval->tier == "native-or-core-patch") {
        return std::unexpected("native Plugin / core patch 不走自动晋升:交人工审查与发布流程"
                               "(LubanCode core patch 永远不进 /evolve approve)");
    }
    {
        const EvolutionRecord& record = *found->record;
        bool code_bearing = !record.changes.tools_added.empty() ||
                            !record.changes.permissions_added.empty();
        if (!code_bearing) {
            lubancode::package::PackageCandidate probe;
            probe.scope = lubancode::package::PackageScope::Dev;
            probe.package_root = candidate_dir / "package";
            probe.dir_name = "package";
            const lubancode::package::PackageInventory inventory =
                lubancode::package::BuildPackageInventory(probe);
            code_bearing = inventory.code_bearing() || !inventory.plugins.empty() ||
                           !inventory.mcp_servers.empty();
        }
        if (code_bearing) {
            return std::unexpected(
                "候选是 code-bearing 档(带 Plugin/MCP 或新工具/权限差异),首版不走 "
                "/evolve approve 自动晋升:代码草稿请走 Package trust(/package trust "
                "<包id>)与人工审查线——人读完 plugin.json/runner.py 或 mcp.yaml/server.py "
                "再批信任,挂载另有沙箱与整包事务把守(与信任门单各管各的账)");
        }
    }

    // ---- 材料(批准页;失败也把材料备齐不了就不动状态) ----
    auto brief_result = BuildApprovalBrief(candidate_id);
    if (!brief_result.has_value()) {
        return std::unexpected(brief_result.error());
    }
    ApprovalBrief brief = std::move(*brief_result);

    // ---- 门三:提交批准页(evaluated -> awaiting_approval) ----
    if (state == CandidateState::Evaluated) {
        const auto moved = AppendState(candidate_dir, CandidateState::Evaluated,
                                       CandidateState::AwaitingApproval, "user",
                                       "评测材料齐备,提交批准页");
        if (!moved.has_value()) {
            return std::unexpected(moved.error());
        }
        state = CandidateState::AwaitingApproval;
    }

    // ---- 门四:装 store(staging 复算哈希 + 静态门 + 原子落;失败停在
    //      awaiting_approval,重批即恢复) ----
    const auto installed =
        versions_.Install(candidate_dir / "package", found->candidate_id, found->content_hash);
    if (!installed.has_value()) {
        return std::unexpected(installed.error());
    }

    // ---- 批准账:approved + decision 齐(只认这一枚哈希) ----
    ApprovalRecord approval = *found->approval;
    approval.tier = "content-only";
    approval.status = "approved";
    ApprovalDecision decision;
    decision.decided_by = "user";
    decision.decision = "approved";
    decision.decided_at = IsoNowUtc();
    decision.reason = "用户批准;内容哈希复算一致,staging 静态门过";
    decision.fingerprint = std::string();
    approval.decision = decision;
    if (!WriteFileBytes(candidate_dir / "approval.json", SerializeApprovalRecord(approval))) {
        return std::unexpected("写批准账失败: " + PathToUtf8(candidate_dir / "approval.json"));
    }

    // ---- 状态:awaiting_approval -> staged ----
    const auto moved = AppendState(candidate_dir, CandidateState::AwaitingApproval,
                                   CandidateState::Staged, "user",
                                   "用户批准,内容哈希复算一致,原子落 version store");
    if (!moved.has_value()) {
        return std::unexpected(moved.error());
    }

    ApproveResult result;
    result.brief = std::move(brief);
    result.installed_version = installed->version;
    result.version_dir = installed->version_dir;
    result.already_present = installed->already_present;
    return result;
}

std::expected<EvolutionCoordinator::UseResult, std::string> EvolutionCoordinator::Use(
    const std::string& candidate_id) {
    const auto found = store_.Find(candidate_id);
    if (!found.has_value()) {
        return std::unexpected("找不到候选 \"" + candidate_id + "\"(先 /evolve list 看)");
    }
    const CandidateState state = found->state;
    if (state != CandidateState::Staged) {
        if (state == CandidateState::Canary) {
            return std::unexpected("候选 \"" + candidate_id +
                                   "\" 已在 canary;晋升走 /evolve promote,撤走走 /evolve rollback");
        }
        return std::unexpected("候选 \"" + candidate_id + "\" 在 " + ToString(state) +
                               ",点名 canary 须先 /evolve approve 落到 staged");
    }
    // 装架时的版本(package.yaml 的稳定版号)。
    const auto manifest_text = ReadFileText(found->dir / "package" / "package.yaml");
    if (!manifest_text.has_value()) {
        return std::unexpected("候选缺 package.yaml: " + PathToUtf8(found->dir / "package"));
    }
    const auto manifest = lubancode::package::ParsePackageManifest(*manifest_text);
    if (!manifest.has_value()) {
        return std::unexpected("候选根清单解析不过: " + manifest.error().Format());
    }
    // 已有别的 canary 占着(同包另一版本):先处理它,账不混。
    if (const auto channels = versions_.LoadChannels(found->package_id); channels.has_value()) {
        if (channels->canary.has_value() &&
            channels->canary->candidate_id != found->candidate_id) {
            return std::unexpected("包 \"" + found->package_id + "\" 已有 canary 版本 " +
                                   channels->canary->version + "(候选 " +
                                   channels->canary->candidate_id +
                                   ");先 /evolve promote 或 /evolve rollback 收走它");
        }
    }
    const auto canary = versions_.SetCanary(found->package_id, manifest->version.text);
    if (!canary.has_value()) {
        return std::unexpected(canary.error());
    }
    const auto moved = AppendState(found->dir, CandidateState::Staged, CandidateState::Canary,
                                   "user", "点名 canary(新会话生效,旧任务钉旧快照)");
    if (!moved.has_value()) {
        return std::unexpected(moved.error());
    }
    UseResult result;
    result.package_id = found->package_id;
    result.version = canary->version;
    result.version_dir = versions_.VersionDir(found->package_id, canary->version);
    return result;
}

std::expected<EvolutionCoordinator::PromoteResult, std::string> EvolutionCoordinator::Promote(
    const std::string& candidate_id) {
    const auto found = store_.Find(candidate_id);
    if (!found.has_value()) {
        return std::unexpected("找不到候选 \"" + candidate_id + "\"(先 /evolve list 看)");
    }
    const CandidateState state = found->state;
    if (state != CandidateState::Canary) {
        return std::unexpected("候选 \"" + candidate_id + "\" 在 " + ToString(state) +
                               ";晋升只从 canary 起步(先 /evolve approve 再 /evolve use)");
    }
    // canary 指针须正指这只候选的版本——账要对得上。
    const auto channels = versions_.LoadChannels(found->package_id);
    if (!channels.has_value() || !channels->canary.has_value()) {
        return std::unexpected("包 \"" + found->package_id +
                               "\" 的 canary 指针账读不出,晋升无从对账");
    }
    if (channels->canary->candidate_id != found->candidate_id) {
        return std::unexpected("canary 指针指在候选 " + channels->canary->candidate_id +
                               ",不是 \"" + candidate_id + "\";先收走它(/evolve rollback)");
    }
    const auto promoted = versions_.PromoteToActive(found->package_id);
    if (!promoted.has_value()) {
        return std::unexpected(promoted.error());
    }
    const auto moved = AppendState(found->dir, CandidateState::Canary, CandidateState::Active,
                                   "user", "样本足够,canary -> active(新会话起用新版)");
    if (!moved.has_value()) {
        return std::unexpected(moved.error());
    }
    PromoteResult result;
    result.package_id = found->package_id;
    result.version = promoted->version;
    result.version_dir = versions_.VersionDir(found->package_id, promoted->version);
    return result;
}

std::expected<EvolutionCoordinator::RollbackResult, std::string> EvolutionCoordinator::Rollback(
    const std::string& package_id, const std::string& version) {
    const auto channels = versions_.LoadChannels(package_id);
    if (!channels.has_value()) {
        return std::unexpected("store 里没有包 \"" + package_id +
                               "\" 的账(先 /evolve list 看候选,/evolve approve 落架)");
    }
    std::string from_version;
    std::string from_candidate;
    if (channels->active.has_value()) {
        from_version = channels->active->version;
        from_candidate = channels->active->candidate_id;
    } else if (channels->canary.has_value()) {
        from_version = channels->canary->version;
        from_candidate = channels->canary->candidate_id;
    } else {
        return std::unexpected("包 \"" + package_id + " 没有 active 也没有 canary,没有可回的版本");
    }

    // ---- 解回滚目标:给了版本用版本;没给切父版(演化账 parent);无父撤下 ----
    std::string target = version;
    std::string reason;
    if (!version.empty()) {
        reason = "切回指定版本 " + version;
    } else {
        const auto from = store_.Find(from_candidate);
        if (from.has_value() && from->record.has_value() && from->record->parent.has_value()) {
            target = from->record->parent->version;
            reason = "切回父版 " + target + "(候选 " + from_candidate + " 的演化账)";
        } else {
            reason = "无父版可回,撤下(active/canary 清空;版本与账保留)";
        }
    }

    const auto switched = versions_.RollbackTo(package_id, target, reason);
    if (!switched.has_value()) {
        return std::unexpected(switched.error());
    }

    // ---- 状态迁移:这只包名下在 canary/active 的候选,一枚枚落 rolled_back ----
    RollbackResult result;
    result.package_id = package_id;
    result.from_version = from_version;
    if (switched->has_value()) {
        result.to_version = (*switched)->version;
    }
    for (const CandidateSummary& candidate : store_.LoadAll()) {
        if (candidate.package_id != package_id) {
            continue;
        }
        if (candidate.state != CandidateState::Canary && candidate.state != CandidateState::Active) {
            continue;
        }
        const auto moved = AppendState(candidate.dir, candidate.state, CandidateState::RolledBack,
                                       "user", reason);
        if (!moved.has_value()) {
            return std::unexpected(moved.error());
        }
        result.rolled_back_candidates.push_back(candidate.candidate_id);
    }
    return result;
}

std::expected<EvolutionCoordinator::ProposeResult, std::string>
EvolutionCoordinator::ProposeRecording(
    const skills::RecordingStatus& status, const std::vector<skills::RecordEvent>& events) {
    ClusterTaskMaterial material;
    material.status = status;
    material.events = events;
    return ProposeFromCluster({std::move(material)});
}

std::expected<EvolutionCoordinator::ProposeResult, std::string>
EvolutionCoordinator::ProposeFromCluster(const std::vector<ClusterTaskMaterial>& tasks) {
    if (tasks.empty()) {
        return std::unexpected("簇是空的,起不出候选");
    }
    // ---- 拒绝门:被拒 fingerprint 的同类不再起草(契约:不死缠) ----
    std::vector<EvolutionObservation> observations;
    observations.reserve(tasks.size());
    for (const ClusterTaskMaterial& task : tasks) {
        const RecordingMaterial material{task.status, task.events};
        const std::vector<EvolutionObservation> made = ObservationsFromRecording(material);
        if (made.empty()) {
            return std::unexpected("录制件 \"" + task.status.id +
                                   "\" 没录完(缺 record_stop),起不出候选");
        }
        observations.push_back(made.front());
    }
    const EvolutionObservation& observation = observations.front();
    for (const EvolutionObservation& other : observations) {
        if (other.fingerprint != observation.fingerprint) {
            return std::unexpected("簇内指纹不一致(\"" + observation.source_id + "\" 对 \"" +
                                   other.source_id + "\"),不是同类经验,起不出组合候选");
        }
    }
    if (observations_ != nullptr) {
        for (const EvolutionObservation& item : observations) {
            const auto appended = observations_->Append(item);
            if (!appended.has_value()) {
                return std::unexpected("观察落账失败: " + appended.error());
            }
            if (&item == &observation &&
                *appended == ObservationStore::AppendStatus::SuppressedRejected) {
                return std::unexpected("同类经验已被拒绝过(" + observation.fingerprint +
                                       "),不再起草;内容未变的同款不会重提");
            }
        }
    }

    // ---- 起草(纯函数,两把尺在里头)与身份 ----
    const auto draft = DraftEvolutionCandidate(tasks);
    if (!draft.has_value()) {
        return std::unexpected(draft.error());
    }
    const std::string date = LocalDateCompact();
    const std::string candidate_id = NextCandidateId(root_, date);
    const std::filesystem::path candidate_dir = store_.CandidateDir(draft->package_id, candidate_id);

    // ---- 落盘:先 package/ 全部文本 ----
    const std::string skill_rel = "skills/" + draft->skill_slug + "/SKILL.md";
    std::string workflow_rel;
    std::string agent_rel;
    std::string plugin_rel;  // plugins/<id>/plugin.json(代码档草稿的组件账)
    if (!WriteFileBytes(candidate_dir / "package" / "package.yaml", draft->package_yaml) ||
        !WriteFileBytes(candidate_dir / "package" / lubancode::platform::Utf8ToPath(skill_rel),
                        draft->skill_markdown)) {
        return std::unexpected("写候选包失败: " + PathToUtf8(candidate_dir));
    }
    if (!draft->workflow_id.empty()) {
        workflow_rel = "workflows/" + draft->workflow_id + "/workflow.yaml";
        if (!WriteFileBytes(candidate_dir / "package" / lubancode::platform::Utf8ToPath(workflow_rel),
                            draft->workflow_yaml)) {
            return std::unexpected("写候选包 workflow 失败: " + PathToUtf8(candidate_dir));
        }
    }
    if (draft->with_agent && !draft->agent_name.empty()) {
        agent_rel = "agents/" + draft->agent_name + ".yaml";
        if (!WriteFileBytes(candidate_dir / "package" / lubancode::platform::Utf8ToPath(agent_rel),
                            draft->agent_yaml)) {
            return std::unexpected("写候选包 Agent 失败: " + PathToUtf8(candidate_dir));
        }
    }
    // 阶段 6:代码档草稿三件(plugin.json + runner 脚手架 + 依赖清单)。
    // 草稿只落候选区——不进挂载事务、不自动启用;静态门过不了就地降档
    // (下一段),与组合件同一条路。
    if (draft->with_plugin_draft && !draft->plugin_id.empty()) {
        const std::string base = "plugins/" + draft->plugin_id + "/";
        plugin_rel = base + "plugin.json";
        const std::string runner_rel = base + "runner.py";
        const std::string req_rel = base + "requirements.txt";
        if (!WriteFileBytes(candidate_dir / "package" / lubancode::platform::Utf8ToPath(plugin_rel),
                            draft->plugin_json) ||
            !WriteFileBytes(candidate_dir / "package" / lubancode::platform::Utf8ToPath(runner_rel),
                            draft->plugin_runner) ||
            !WriteFileBytes(candidate_dir / "package" / lubancode::platform::Utf8ToPath(req_rel),
                            draft->plugin_requirements)) {
            return std::unexpected("写候选包插件草稿失败: " + PathToUtf8(candidate_dir));
        }
    }
    std::string mcp_rel;  // mcp/<id>/mcp.yaml(MCP 草稿的组件账;与 Plugin 草稿互斥)
    if (draft->with_mcp_draft && !draft->mcp_id.empty()) {
        const std::string base = "mcp/" + draft->mcp_id + "/";
        mcp_rel = base + "mcp.yaml";
        const std::string server_rel = base + "server.py";
        const std::string req_rel = base + "requirements.txt";
        if (!WriteFileBytes(candidate_dir / "package" / lubancode::platform::Utf8ToPath(mcp_rel),
                            draft->mcp_yaml) ||
            !WriteFileBytes(candidate_dir / "package" / lubancode::platform::Utf8ToPath(server_rel),
                            draft->mcp_server) ||
            !WriteFileBytes(candidate_dir / "package" / lubancode::platform::Utf8ToPath(req_rel),
                            draft->mcp_requirements)) {
            return std::unexpected("写候选包 MCP 草稿失败: " + PathToUtf8(candidate_dir));
        }
    }

    // ---- 组合件/代码件静态门(阶段 5/6):AnalyzePackage + 扫描过了一遍才
    //      认。过不了就地降回 Skill-only——删掉 workflows/、agents/、
    //      plugins/ 与 mcp/,诊断进状态账与 ProposeResult,不硬塞。 ----
    std::string shape = !plugin_rel.empty() || !mcp_rel.empty()
                            ? "code-draft"
                            : (workflow_rel.empty() && agent_rel.empty() ? "skill-only" : "combination");
    std::string downgrade_note;
    if (shape != "skill-only") {
        const StaticGateResult combo_gate = RunStaticGate(candidate_dir / "package");
        if (!combo_gate.pass()) {
            std::string diagnostics;
            for (std::size_t i = 0;
                 i < combo_gate.errors.size() && i < combo_gate.findings.size() + 3 && i < 5; ++i) {
                if (!diagnostics.empty()) {
                    diagnostics += ";";
                }
                diagnostics += TruncateUtf8(combo_gate.errors[i], 160);
            }
            if (diagnostics.empty() && !combo_gate.findings.empty()) {
                diagnostics = "扫描发现 " + std::to_string(combo_gate.findings.size()) + " 处";
            }
            std::error_code ec;
            std::filesystem::remove_all(candidate_dir / "package" / "workflows", ec);
            std::filesystem::remove_all(candidate_dir / "package" / "agents", ec);
            std::filesystem::remove_all(candidate_dir / "package" / "plugins", ec);
            std::filesystem::remove_all(candidate_dir / "package" / "mcp", ec);
            workflow_rel.clear();
            agent_rel.clear();
            plugin_rel.clear();
            mcp_rel.clear();
            shape = "skill-only";
            downgrade_note = "草稿件过不了静态门(引用闭合/canonical 名/越界/安全扫描),已降回 "
                             "Skill-only:" +
                             (diagnostics.empty() ? "见 /evolve test 静态门" : diagnostics);
        }
    }

    // ---- 复算整包哈希(照 Package 阶段 1 的盘点算法;降档后按最终形状算) ----
    const std::string content_hash = ComputeCandidateContentHash(candidate_dir / "package");
    if (content_hash.empty()) {
        return std::unexpected("复算候选整包哈希失败: " + PathToUtf8(candidate_dir / "package"));
    }

    // ---- 组件清单(演化账与 ProposeResult 共用;照降档后的最终形状) ----
    std::vector<std::string> components = {skill_rel};
    if (!workflow_rel.empty()) {
        components.push_back(workflow_rel);
    }
    if (!agent_rel.empty()) {
        components.push_back(agent_rel);
    }
    if (!plugin_rel.empty()) {
        components.push_back(plugin_rel);
    }
    if (!mcp_rel.empty()) {
        components.push_back(mcp_rel);
    }

    // ---- 演化账(schema 1) ----
    EvolutionRecord record;
    record.candidate_id = candidate_id;
    record.package_id = draft->package_id;
    record.candidate_version = draft->package_version + "-candidate.1";
    record.parent = std::nullopt;  // 无父明写 null,不假装升级
    record.objective = draft->objective;
    record.sources.recording_ids = draft->recording_ids;
    if (shape == "combination") {
        record.generator = {"builtin", "combo-drafter", "evolution-stage5"};
    } else if (shape == "code-draft" && !mcp_rel.empty()) {
        record.generator = {"builtin", "mcp-drafter", "evolution-stage6-mcp"};
    } else if (shape == "code-draft") {
        record.generator = {"builtin", "plugin-drafter", "evolution-stage6"};
    } else {
        record.generator = {"builtin", "skill-drafter",
                             downgrade_note.empty()
                                 ? "evolution-stage2"
                                 : (draft->code_signal.eligible ? "evolution-stage6-downgraded"
                                                                : "evolution-stage5-downgraded")};
    }
    record.changes.components_added = components;
    if (shape == "code-draft") {
        // 代码档:权限差异与工具差异单列(一条一权,env 只记名不记值)——
        // 批准页与 diff 页照这份账亮,approve 的档位门照它明拒。
        record.changes.permissions_added = draft->permissions_added;
        record.changes.tools_added = draft->tools_added;
    }
    record.created_at = IsoNowUtc();
    if (!WriteFileBytes(candidate_dir / "evolution.json", SerializeEvolutionRecord(record))) {
        return std::unexpected("写演化账失败: " + PathToUtf8(candidate_dir / "evolution.json"));
    }

    // ---- 批准账(未决) ----
    ApprovalRecord approval;
    approval.candidate_id = candidate_id;
    approval.package_id = draft->package_id;
    approval.candidate_version = record.candidate_version;
    approval.content_hash = content_hash;
    // 代码档草稿如实落档:approve 的档位门照 tier 明拒自动晋升,指路
    // Package trust 与人工审查线(阶段 4 语义不动)。
    approval.tier = shape == "code-draft" ? "process-plugin-or-mcp" : "content-only";
    approval.status = "awaiting_approval";
    approval.requested_at = IsoNowUtc();
    if (!WriteFileBytes(candidate_dir / "approval.json", SerializeApprovalRecord(approval))) {
        return std::unexpected("写批准账失败: " + PathToUtf8(candidate_dir / "approval.json"));
    }

    // ---- 评测计划与结果账(空文件)----
    // 评测分家(阶段 5):候选包里的 workflow 不进评测执行——workflow 组件
    // 只做静态校验与来源回放的夹具。组合候选的 replay 验收带可执行检查器
    // (file_exists/file_contains 查包形状),人工口述照列;Skill-only 照
    // 阶段 3 的旧形状(验收留人工,确定性判不了就如实 skipped)。
    {
        std::string acceptance_text;
        for (const skills::RecordEvent& event : tasks.front().events) {
            if (event.type == skills::kEventRecordStart) {
                const auto it = event.data.find("acceptance");
                if (it != event.data.end() && it->is_string()) {
                    acceptance_text = it->get<std::string>();
                }
            }
        }
        if (acceptance_text.empty()) {
            acceptance_text = "按录制口述人工验收";
        }
        nlohmann::json acceptance = nlohmann::json::array({acceptance_text});
        if (shape == "combination") {
            acceptance.push_back(nlohmann::json{{"kind", "file_exists"},
                                                {"path", "package" + std::string("/") + skill_rel},
                                                {"note", "SKILL 在包里"}});
            if (!workflow_rel.empty()) {
                acceptance.push_back(nlohmann::json{
                    {"kind", "file_exists"}, {"path", "package/" + workflow_rel},
                    {"note", "workflow 组件在(只做静态校验与夹具,评测不起它)"}});
                acceptance.push_back(nlohmann::json{
                    {"kind", "file_contains"}, {"path", "package/" + workflow_rel},
                    {"text", "id: " + draft->workflow_id}, {"note", "workflow id 与目录一致"}});
            }
            if (!agent_rel.empty()) {
                acceptance.push_back(nlohmann::json{
                    {"kind", "file_exists"}, {"path", "package/" + agent_rel},
                    {"note", "Agent 组件在(tools.allow 照观察到的实际面)"}});
            }
        }
        if (shape == "code-draft") {
            // 代码档草稿的评测只有静态检查:草稿永不执行(零进程铁律),
            // acceptance 的 kind 白名单里没有(也不许有)command 项——
            // 草稿要真跑起来,只能走 Package trust 与人工审查后的挂载路。
            acceptance.push_back(nlohmann::json{{"kind", "file_exists"},
                                                {"path", "package" + std::string("/") + skill_rel},
                                                {"note", "SKILL 在包里"}});
            if (!plugin_rel.empty()) {
                const std::string plugin_dir_rel =
                    plugin_rel.substr(0, plugin_rel.rfind('/'));  // plugins/<id>
                acceptance.push_back(nlohmann::json{{"kind", "file_exists"},
                                                    {"path", "package/" + plugin_rel},
                                                    {"note", "插件清单在(草稿件,零执行)"}});
                acceptance.push_back(nlohmann::json{{"kind", "json_parses"},
                                                    {"path", "package/" + plugin_rel},
                                                    {"note", "plugin.json 可解析"}});
                acceptance.push_back(nlohmann::json{
                    {"kind", "file_contains"}, {"path", "package/" + plugin_rel},
                    {"text", "\"kind\": \"process\""},
                    {"note", "runtime 只认 process(native 一律不生成)"}});
                acceptance.push_back(nlohmann::json{
                    {"kind", "file_exists"}, {"path", "package/" + plugin_dir_rel + "/runner.py"},
                    {"note", "runner 脚手架在(未实现占位,人工补)"}});
                acceptance.push_back(nlohmann::json{
                    {"kind", "file_exists"}, {"path", "package/" + plugin_dir_rel + "/requirements.txt"},
                    {"note", "依赖清单在(草稿零依赖)"}});
            }
            if (!mcp_rel.empty()) {
                const std::string mcp_dir_rel = mcp_rel.substr(0, mcp_rel.rfind('/'));  // mcp/<id>
                acceptance.push_back(nlohmann::json{{"kind", "file_exists"},
                                                    {"path", "package/" + mcp_rel},
                                                    {"note", "mcp.yaml 在(草稿件,零执行)"}});
                acceptance.push_back(nlohmann::json{
                    {"kind", "file_contains"}, {"path", "package/" + mcp_rel},
                    {"text", "transport: stdio"},
                    {"note", "transport 只认 stdio(与官方 mcp 组件同款)"}});
                acceptance.push_back(nlohmann::json{
                    {"kind", "file_exists"}, {"path", "package/" + mcp_dir_rel + "/server.py"},
                    {"note", "server 脚手架在(未实现占位,人工补)"}});
                acceptance.push_back(nlohmann::json{
                    {"kind", "file_exists"}, {"path", "package/" + mcp_dir_rel + "/requirements.txt"},
                    {"note", "依赖清单在(草稿零依赖)"}});
            }
        }
        nlohmann::json plan;
        plan["schema"] = 1;
        plan["candidate_id"] = candidate_id;
        plan["content_hash"] = content_hash;
        plan["replay"] = nlohmann::json::array({nlohmann::json{
            {"source_id", tasks.front().status.id},
            {"task", draft->objective},
            {"workspace", shape == "skill-only" ? std::string("") : std::string(".")},
            {"acceptance", acceptance},
        }});
        plan["holdout"] = nlohmann::json::array();
        plan["baseline"] = {
            {"kind", "bare-agent"},
            {"ref", "default-agent"},
            {"metrics", nlohmann::json::array({"success_rate", "acceptance_rate", "tool_calls",
                                               "tokens", "wall_clock_ms", "permission_prompts",
                                               "workspace_writes"})},
        };
        plan["budget"] = {{"max_tool_calls", 40}, {"max_tokens", 200000}, {"timeout_ms", 600000}};
        if (!WriteFileBytes(candidate_dir / "eval-plan.json", plan.dump(2) + "\n")) {
            return std::unexpected("写评测计划失败: " + PathToUtf8(candidate_dir / "eval-plan.json"));
        }
    }
    if (!WriteFileBytes(candidate_dir / "eval-results.jsonl", std::string())) {
        return std::unexpected("写评测账失败: " + PathToUtf8(candidate_dir / "eval-results.jsonl"));
    }

    // ---- 状态账首行:observed -> drafted。写到这里候选才算齐 ----
    std::string reason = "propose " + tasks.front().status.id;
    if (shape == "combination") {
        reason += "(组合候选:簇 " + std::to_string(tasks.size()) + " 场,两把尺过门)";
    }
    if (shape == "code-draft") {
        reason += "(代码档草稿:簇 " + std::to_string(draft->code_signal.tasks_wanting) +
                  " 场同求工具 " + draft->code_signal.wanted_tool +
                  (mcp_rel.empty() ? std::string("(process Plugin 路)")
                                   : ("等 " + std::to_string(draft->code_signal.wanted_tools.size()) +
                                      " 件(MCP server 路)")) +
                  ";零进程零挂载,不自动启用,指路 Package trust 人工审查)";
    }
    if (!downgrade_note.empty()) {
        reason += "(降档: " + TruncateUtf8(downgrade_note, 240) + ")";
    }
    const auto state = AppendState(candidate_dir, CandidateState::Observed, CandidateState::Drafted,
                                   "user", reason);
    if (!state.has_value()) {
        return std::unexpected(state.error());
    }

    ProposeResult result;
    result.package_id = draft->package_id;
    result.candidate_id = candidate_id;
    result.candidate_version = record.candidate_version;
    result.content_hash = content_hash;
    result.candidate_dir = candidate_dir;
    result.skill_rel_path = skill_rel;
    result.shape = shape;
    result.component_paths = components;
    result.cluster_size = static_cast<int>(tasks.size());
    result.agent_drafted = !agent_rel.empty();
    result.downgrade_note = downgrade_note;
    result.code_draft = shape == "code-draft";
    result.mcp_draft = result.code_draft && !mcp_rel.empty();
    result.wanted_tool = result.code_draft ? draft->code_signal.wanted_tool : std::string();
    result.wanted_tools = result.mcp_draft ? draft->code_signal.wanted_tools
                                           : std::vector<std::string>();
    result.permissions_added = record.changes.permissions_added;
    result.tools_added = record.changes.tools_added;
    return result;
}

std::expected<EvolutionCoordinator::RejectResult, std::string> EvolutionCoordinator::Reject(
    const std::string& candidate_id, const std::string& reason) {
    const auto found = store_.Find(candidate_id);
    if (!found.has_value()) {
        return std::unexpected("找不到候选 \"" + candidate_id + "\"(先 /evolve list 看)");
    }
    const CandidateState current = found->state;
    if (IsTerminalCandidateState(current)) {
        return std::unexpected("候选 \"" + candidate_id + "\" 已在终态 " + ToString(current) +
                               ",不再迁移");
    }

    // ---- 去重指纹:优先来源观察的同类指纹 ----
    std::string fingerprint;
    if (observations_ != nullptr && found->record.has_value()) {
        for (const std::string& recording_id : found->record->sources.recording_ids) {
            const std::string observation_id =
                MakeObservationId(ObservationSource::Recording, recording_id);
            if (const auto observation = observations_->Find(observation_id); observation.has_value()) {
                fingerprint = observation->fingerprint;
                break;
            }
        }
    }
    if (fingerprint.empty()) {
        // 观察账不在(或来源已被清账):退回内容指纹,至少同内容不再劝。
        fingerprint = "rej|" + found->package_id + "|" + found->content_hash;
    }

    // ---- 状态账:任意非终态 -> rejected ----
    const auto state = AppendState(found->dir, current, CandidateState::Rejected, "user",
                                   reason.empty() ? "用户拒绝" : reason, fingerprint);
    if (!state.has_value()) {
        return std::unexpected(state.error());
    }

    // ---- 批准账:status=rejected,decision 留全 ----
    ApprovalRecord approval = found->approval.value_or(ApprovalRecord{});
    approval.schema = 1;
    approval.candidate_id = found->candidate_id;
    approval.package_id = found->package_id;
    if (approval.candidate_version.empty() && found->record.has_value()) {
        approval.candidate_version = found->record->candidate_version;
    }
    if (approval.content_hash.empty()) {
        approval.content_hash = found->content_hash;
    }
    if (approval.tier.empty()) {
        approval.tier = "content-only";
    }
    if (approval.requested_at.empty() && found->record.has_value()) {
        approval.requested_at = found->record->created_at;
    }
    approval.status = "rejected";
    ApprovalDecision decision;
    decision.decided_by = "user";
    decision.decision = "rejected";
    decision.decided_at = IsoNowUtc();
    decision.reason = reason;
    decision.fingerprint = fingerprint;
    approval.decision = decision;
    if (!WriteFileBytes(found->dir / "approval.json", SerializeApprovalRecord(approval))) {
        return std::unexpected("写批准账失败: " + PathToUtf8(found->dir / "approval.json"));
    }

    // ---- 观察账:拒绝指纹入账,同类不再进观察、不再被劝 ----
    if (observations_ != nullptr) {
        const auto marked = observations_->MarkRejected(fingerprint, reason);
        if (!marked.has_value()) {
            return std::unexpected("写拒绝指纹账失败: " + marked.error());
        }
    }

    RejectResult result;
    result.fingerprint = fingerprint;
    result.candidate_dir = found->dir;
    return result;
}

std::expected<EvolutionCoordinator::DiffResult, std::string> EvolutionCoordinator::Diff(
    const std::string& candidate_id) {
    const auto found = store_.Find(candidate_id);
    if (!found.has_value()) {
        return std::unexpected("找不到候选 \"" + candidate_id + "\"(先 /evolve list 看)");
    }
    DiffResult result;
    result.candidate_id = found->candidate_id;
    result.package_id = found->package_id;

    // 基线:有父比父版,无父与空对照(阶段 2 候选一律无父)。
    if (found->record.has_value() && found->record->parent.has_value()) {
        result.baseline = "父版 " + found->record->package_id + "@" + found->record->parent->version +
                          "(" + found->record->parent->content_hash + ")";
    } else {
        result.baseline = "(无父版,与空对照)";
    }

    // 新增文件:无父即全量;逐文件单算 sha256(整包哈希另在 content_hash)。
    const std::filesystem::path package_dir = found->dir / "package";
    std::vector<std::string> rel_files;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(package_dir, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file()) {
            continue;
        }
        std::string rel = PathToUtf8(it->path().lexically_relative(package_dir));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        rel_files.push_back(rel);
    }
    std::sort(rel_files.begin(), rel_files.end());
    for (const std::string& rel : rel_files) {
        DiffFile file;
        file.rel = rel;
        if (const auto text = ReadFileText(package_dir / lubancode::platform::Utf8ToPath(rel));
            text.has_value()) {
            file.size = text->size();
            file.hash = "sha256:" + hooks::Sha256Hex(*text);
        }
        file.is_skill = rel.rfind("skills/", 0) == 0 && rel.size() >= 9 + 7 &&
                        rel.compare(rel.size() - 9, 9, "/SKILL.md") == 0;
        if (file.is_skill) {
            file.kind = "skill";
        } else if (rel.rfind("workflows/", 0) == 0 && rel.size() > 14 &&
                   rel.compare(rel.size() - 14, 14, "/workflow.yaml") == 0) {
            file.kind = "workflow";
        } else if (rel.rfind("agents/", 0) == 0 && rel.size() > 5 &&
                   rel.compare(rel.size() - 5, 5, ".yaml") == 0) {
            file.kind = "agent";
        } else if (rel.rfind("plugins/", 0) == 0 && rel.size() > 19 &&
                   rel.compare(rel.size() - 11, 11, "plugin.json") == 0) {
            file.kind = "plugin";
        } else if (rel.rfind("mcp/", 0) == 0 && rel.size() > 13 &&
                   rel.compare(rel.size() - 8, 8, "mcp.yaml") == 0) {
            file.kind = "mcp_server";
        } else if (rel.rfind("plugins/", 0) == 0 || rel.rfind("mcp/", 0) == 0) {
            file.kind = "code";
        } else if (rel == "package.yaml") {
            file.kind = "manifest";
        } else {
            file.kind = "other";
        }
        result.added.push_back(std::move(file));
    }

    // 分档形状:照盘上现状说,不猜——代码件草稿(Plugin 或 MCP)在就是
    // 代码档,其次组合档。
    for (const DiffFile& file : result.added) {
        if (file.kind == "plugin" || file.kind == "mcp_server") {
            result.shape = "code-draft";
            break;
        }
    }
    if (result.shape.empty()) {
        for (const DiffFile& file : result.added) {
            if (file.kind == "workflow" || file.kind == "agent") {
                result.shape = "combination";
                break;
            }
        }
    }
    if (result.shape.empty()) {
        result.shape = "skill-only";
    }

    // SKILL 正文摘要:包里第一份 skills/*/SKILL.md。
    for (const DiffFile& file : result.added) {
        if (!file.is_skill) {
            continue;
        }
        if (const auto text = ReadFileText(package_dir / lubancode::platform::Utf8ToPath(file.rel));
            text.has_value()) {
            result.skill_summary = SummarizeSkillBody(*text);
        }
        break;
    }

    // ---- 阶段 5:组合件的摘要(diff 页分档展示) ----
    for (const DiffFile& file : result.added) {
        if (file.kind != "workflow") {
            continue;
        }
        const auto text = ReadFileText(package_dir / lubancode::platform::Utf8ToPath(file.rel));
        if (!text.has_value()) {
            continue;
        }
        const auto definition = lubancode::workflow::LoadWorkflowDefinition(
            package_dir / lubancode::platform::Utf8ToPath(file.rel));
        if (!definition.has_value()) {
            result.workflow_summary = "(workflow 解析不过: " +
                                      (definition.error().empty()
                                           ? std::string("无诊断")
                                           : definition.error().front().message) +
                                      ")";
            continue;
        }
        std::string chain;
        for (const auto& node : definition->nodes) {
            if (node.kind != lubancode::workflow::NodeKind::Tool) {
                continue;
            }
            if (!chain.empty()) {
                chain += " -> ";
            }
            chain += node.tool.empty() ? node.id : node.tool;
        }
        std::size_t failure_count = 0;
        for (const std::string& line : SplitLines(*text)) {
            const std::size_t at = line.find("#   - ");
            if (at == std::string::npos) {
                continue;
            }
            result.workflow_failures.push_back(TruncateUtf8(line.substr(at + 6), 88));
            ++failure_count;
        }
        result.workflow_summary = chain.empty() ? "(无工具节点)"
                                                : chain + "(入口 " + definition->entry + ")";
        if (failure_count > 0) {
            result.workflow_summary += ";已知失败路 " + std::to_string(failure_count) + " 处";
        }
        break;  // 摘要只做第一份 workflow
    }
    for (const DiffFile& file : result.added) {
        if (file.kind != "agent") {
            continue;
        }
        const auto text = ReadFileText(package_dir / lubancode::platform::Utf8ToPath(file.rel));
        if (!text.has_value()) {
            continue;
        }
        const auto parsed =
            lubancode::agent::ParseAgentDefinitionYaml(*text, "agents/" + file.rel);
        if (!parsed.definition.has_value()) {
            result.agent_summary = "(Agent 解析不过)";
            break;
        }
        std::string face;
        for (const std::string& tool : parsed.definition->tools.allow) {
            if (!face.empty()) {
                face += ", ";
            }
            face += tool;
        }
        result.agent_summary = file.rel + ":工具面 " +
                               std::to_string(parsed.definition->tools.allow.size()) + " 件(" +
                               TruncateUtf8(face, 96) + ")";
        if (!parsed.definition->skills_preload.empty()) {
            result.agent_summary += ";预装 Skill " + parsed.definition->skills_preload.front();
        }
        break;
    }

    // ---- 阶段 6:代码档草稿摘要与权限差异(diff 页如实亮,approve 仍明拒)----
    for (const DiffFile& file : result.added) {
        if (file.kind != "plugin") {
            continue;
        }
        const auto text = ReadFileText(package_dir / lubancode::platform::Utf8ToPath(file.rel));
        if (!text.has_value()) {
            break;
        }
        const nlohmann::json manifest = nlohmann::json::parse(*text, nullptr, false);
        if (manifest.is_discarded() || !manifest.is_object()) {
            result.plugin_summary = file.rel + ":plugin.json 解析不过(草稿带病,见静态门)";
            break;
        }
        // 手改过的草稿什么形状都可能有,逐字段防着读,不赌类型。
        const nlohmann::json empty_object = nlohmann::json::object();
        const nlohmann::json empty_array = nlohmann::json::array();
        const nlohmann::json runtime =
            manifest.contains("runtime") && manifest.at("runtime").is_object()
                ? manifest.at("runtime")
                : empty_object;
        std::string command;
        if (runtime.contains("command") && runtime.at("command").is_string()) {
            command = runtime.at("command").get<std::string>();
        }
        const nlohmann::json args =
            runtime.contains("args") && runtime.at("args").is_array() ? runtime.at("args") : empty_array;
        std::string tool_names;
        int tool_count = 0;
        const nlohmann::json tools =
            manifest.contains("tools") && manifest.at("tools").is_array() ? manifest.at("tools")
                                                                           : empty_array;
        for (const nlohmann::json& tool : tools) {
            if (!tool.is_object() || !tool.contains("name") || !tool.at("name").is_string()) {
                continue;
            }
            ++tool_count;
            tool_names += (tool_names.empty() ? "" : ", ") + tool.at("name").get<std::string>();
        }
        const nlohmann::json perms =
            manifest.contains("permissions") && manifest.at("permissions").is_object()
                ? manifest.at("permissions")
                : nlohmann::json::object();
        bool network = false;
        if (perms.contains("network") && perms.at("network").is_boolean()) {
            network = perms.at("network").get<bool>();
        }
        std::string env_names;
        if (perms.is_object() && perms.contains("env") && perms.at("env").is_array()) {
            for (const nlohmann::json& name : perms.at("env")) {
                if (name.is_string()) {
                    env_names += (env_names.empty() ? "" : ", ") + name.get<std::string>();
                }
            }
        }
        std::string args_note;
        for (const nlohmann::json& arg : args) {
            if (arg.is_string()) {
                args_note += (args_note.empty() ? "" : " ") + arg.get<std::string>();
            }
        }
        result.plugin_summary = file.rel + ":process 命令 " + command + " " + TruncateUtf8(args_note, 48) +
                                ";工具 " + std::to_string(tool_count) + " 件(" + TruncateUtf8(tool_names, 72) +
                                ");网络 " + (network ? "开" : "关") + ";env 名 " +
                                (env_names.empty() ? "(无)" : env_names) +
                                ";草稿未实现,零执行零挂载";
        break;
    }
    // MCP 草稿摘要:mcp.yaml 用它自己的 parser 读(手改过的草稿逐字段防着
    // 读,不赌类型);工具账照演化账的 tools_added(mcp.yaml 里没有工具
    // 清单——工具是起服后 tools/list 报的,草稿的已知面在演化账)。
    for (const DiffFile& file : result.added) {
        if (file.kind != "mcp_server") {
            continue;
        }
        const auto text = ReadFileText(package_dir / lubancode::platform::Utf8ToPath(file.rel));
        if (!text.has_value()) {
            break;
        }
        const auto parsed = lubancode::package::ParseMcpComponentYaml(*text, package_dir);
        if (!parsed.has_value()) {
            result.mcp_summary =
                file.rel + ":mcp.yaml 解析不过(草稿带病,见静态门)";
            break;
        }
        std::string args_note;
        for (const std::string& arg : parsed->args) {
            args_note += (args_note.empty() ? " " : "") + arg;
        }
        std::string tools_note = "(演化账未记)";
        if (found->record.has_value() && !found->record->changes.tools_added.empty()) {
            tools_note = std::to_string(found->record->changes.tools_added.size()) + " 件(";
            for (std::size_t i = 0; i < found->record->changes.tools_added.size() && i < 4; ++i) {
                tools_note += (i > 0 ? ", " : "") +
                              found->record->changes.tools_added[i].substr(
                                  found->record->changes.tools_added[i].rfind("__") + 2);
            }
            tools_note += ")";
        }
        result.mcp_summary = file.rel + ":stdio server 命令 " + parsed->command +
                             TruncateUtf8(args_note, 56) + ";工具 " + tools_note + ";网络 " +
                             (parsed->network_allowed ? "开" : "关") +
                             ";草稿未实现,零执行零挂载";
        break;
    }
    if (found->record.has_value()) {
        const EvolutionRecordChanges& changes = found->record->changes;
        for (const std::string& tool : changes.tools_added) {
            result.permission_lines.push_back("新工具 " + tool);
        }
        for (const std::string& perm : changes.permissions_added) {
            result.permission_lines.push_back("新权限 " + perm);
        }
    }
    return result;
}

}  // namespace lubancode::evolution
