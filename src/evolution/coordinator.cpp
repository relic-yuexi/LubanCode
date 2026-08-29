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

#include "evolution/adapters.hpp"
#include "evolution/drafter.hpp"
#include "evolution/eval.hpp"
#include "hooks/hash.hpp"
#include "platform/paths.hpp"

namespace lubancode::evolution {

namespace {

std::string PathToUtf8(const std::filesystem::path& path) {
    return lubancode::platform::PathToUtf8(path);
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
    char buffer[8]{};
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
    std::size_t header_count = 0;
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
            ++header_count;
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
                                           ObservationStore* observations)
    : root_(std::move(candidates_root)), observations_(observations), store_(root_) {}

std::expected<EvolutionCoordinator::ProposeResult, std::string>
EvolutionCoordinator::ProposeRecording(
    const skills::RecordingStatus& status, const std::vector<skills::RecordEvent>& events) {
    // ---- 拒绝门:被拒 fingerprint 的同类不再起草(契约:不死缠) ----
    const RecordingMaterial material{status, events};
    const std::vector<EvolutionObservation> observations = ObservationsFromRecording(material);
    if (observations.empty()) {
        return std::unexpected("录制件 \"" + status.id + "\" 没录完(缺 record_stop),起不出候选");
    }
    const EvolutionObservation& observation = observations.front();
    if (observations_ != nullptr) {
        const auto appended = observations_->Append(observation);
        if (!appended.has_value()) {
            return std::unexpected("观察落账失败: " + appended.error());
        }
        if (*appended == ObservationStore::AppendStatus::SuppressedRejected) {
            return std::unexpected("同类经验已被拒绝过(" + observation.fingerprint +
                                   "),不再起草;内容未变的同款不会重提");
        }
    }

    // ---- 起草(纯函数)与身份 ----
    const auto draft = DraftSkillCandidate(status, events);
    if (!draft.has_value()) {
        return std::unexpected(draft.error());
    }
    const std::string date = LocalDateCompact();
    const std::string candidate_id = NextCandidateId(root_, date);
    const std::filesystem::path candidate_dir = store_.CandidateDir(draft->package_id, candidate_id);

    // ---- 落盘:先 package/ 两份文本 ----
    const std::string skill_rel = "skills/" + draft->skill_slug + "/SKILL.md";
    if (!WriteFileBytes(candidate_dir / "package" / "package.yaml", draft->package_yaml) ||
        !WriteFileBytes(candidate_dir / "package" / lubancode::platform::Utf8ToPath(skill_rel),
                        draft->skill_markdown)) {
        return std::unexpected("写候选包失败: " + PathToUtf8(candidate_dir));
    }

    // ---- 复算整包哈希(照 Package 阶段 1 的盘点算法) ----
    const std::string content_hash = ComputeCandidateContentHash(candidate_dir / "package");
    if (content_hash.empty()) {
        return std::unexpected("复算候选整包哈希失败: " + PathToUtf8(candidate_dir / "package"));
    }

    // ---- 演化账(schema 1) ----
    EvolutionRecord record;
    record.candidate_id = candidate_id;
    record.package_id = draft->package_id;
    record.candidate_version = draft->package_version + "-candidate.1";
    record.parent = std::nullopt;  // 无父明写 null,不假装升级
    record.objective = draft->objective;
    record.sources.recording_ids = {status.id};
    record.generator = {"builtin", "skill-drafter", "evolution-stage2"};
    record.changes.components_added = {skill_rel};
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
    approval.tier = "content-only";
    approval.status = "awaiting_approval";
    approval.requested_at = IsoNowUtc();
    if (!WriteFileBytes(candidate_dir / "approval.json", SerializeApprovalRecord(approval))) {
        return std::unexpected("写批准账失败: " + PathToUtf8(candidate_dir / "approval.json"));
    }

    // ---- 评测计划(阶段 3:replay 指回来源录制,验收留人工;夹具与留出
    // 任务由用户/后续阶段补)与结果账(空文件) ----
    {
        nlohmann::json plan;
        plan["schema"] = 1;
        plan["candidate_id"] = candidate_id;
        plan["content_hash"] = content_hash;
        // 来源回放占位:任务是录制目标,验收是录制时的口述——确定性检查器
        // 判不了人话,/evolve test 会如实记 skipped + manual-acceptance。
        std::string acceptance_text;
        for (const skills::RecordEvent& event : events) {
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
        plan["replay"] = nlohmann::json::array({nlohmann::json{
            {"source_id", status.id},
            {"task", draft->objective},
            {"workspace", ""},
            {"acceptance", nlohmann::json::array({acceptance_text})},
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
    const auto state = AppendState(candidate_dir, CandidateState::Observed, CandidateState::Drafted,
                                   "user", "propose " + status.id);
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
        result.added.push_back(std::move(file));
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
    return result;
}

}  // namespace lubancode::evolution
