#include "insights/integrity_gate.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"
#include "trajectory/directory.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/schema.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"

namespace lubancode::insights {

const char* SessionGateStatusName(SessionGateStatus status) {
    switch (status) {
        case SessionGateStatus::Analyzed: return "analyzed";
        case SessionGateStatus::Active: return "active";
        case SessionGateStatus::Incomplete: return "incomplete";
        case SessionGateStatus::Corrupt: return "corrupt";
        case SessionGateStatus::Missing: return "missing";
        case SessionGateStatus::Unsupported: return "unsupported";
    }
    return "unknown";
}

bool SessionGateReport::sealed() const {
    if (format == "v3") {
        // v3 场无 session.json 可翻:封口唯一事实是主账 session.ended
        //(与删除门 T15-A 同一口径)。
        for (const auto& facts : v3_facts) {
            if (!facts.is_subagent) {
                return facts.sealed;
            }
        }
        return false;
    }
    return session_status == "closed";
}

namespace {

bool DirExists(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::exists(dir, ec) && std::filesystem::is_directory(dir, ec);
}

// 一条 stream 解析成信封(按行序)。坏行/坏信封 → nullopt。
std::optional<std::vector<trajectory::EventEnvelope>> ParseStream(
    const std::filesystem::path& path) {
    const auto lines = trajectory::ReadJournalLines(path);
    if (!lines.has_value()) {
        return std::nullopt;
    }
    std::vector<trajectory::EventEnvelope> envelopes;
    envelopes.reserve(lines->size());
    for (const auto& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded()) {
            return std::nullopt;
        }
        trajectory::EventEnvelope envelope;
        if (trajectory::ParseAndValidateEventLine(parsed, &envelope).has_value()) {
            return std::nullopt;
        }
        envelopes.push_back(std::move(envelope));
    }
    return envelopes;
}

// ---- v3 半场(T14)----

// v3 主账验卷不过时的分型:截断尾 = Incomplete(崩溃中断),其余坏账
// = Corrupt。判据用 VerifyV3File 的 truncated_tail(与 v2 老路同款分型
// 口径;ReadV3Ledger 只回错误文本,不回截断标志)。
SessionGateReport GateV3Unreadable(const std::filesystem::path& session_dir,
                                   const std::filesystem::path& stream,
                                   const std::string& error_text) {
    SessionGateReport report;
    report.format = "v3";
    report.session_id = session_dir.filename().string();
    const auto verify = trajectory::v3::VerifyV3File(stream);
    if (verify.truncated_tail) {
        report.status = SessionGateStatus::Incomplete;
        report.error_code = "gate.incomplete";
        report.message = "v3 主账尾行截断(崩溃中断),整间排除: " +
                         platform::PathToUtf8(stream.filename());
        report.notes.push_back("gate.v3_truncated: " + platform::PathToUtf8(stream));
        return report;
    }
    report.status = SessionGateStatus::Corrupt;
    report.error_code = "gate.v3_ledger_unreadable";
    report.message = "v3 主账验卷不过,整间排除: " + error_text;
    report.notes.push_back("gate.v3_corrupt: " + platform::PathToUtf8(stream) + ": " +
                           error_text);
    return report;
}

// v3 半场:ProbeV3SessionStream 已判 V3Stream。装领域读模型、分型、
// 子账 partial 点名。返回 nullopt = 不是 v3 场,调用方走 v2 老路。
std::optional<SessionGateReport> GateSessionV3(const std::filesystem::path& session_dir) {
    const auto probe = trajectory::v3::ProbeV3SessionStream(session_dir);
    switch (probe.status) {
        case trajectory::v3::V3StreamProbe::Status::V3Stream:
            break;  // 下面走 v3
        case trajectory::v3::V3StreamProbe::Status::FormatConflict:
        case trajectory::v3::V3StreamProbe::Status::NotV3Schema: {
            SessionGateReport report;
            report.status = SessionGateStatus::Unsupported;
            report.error_code = probe.status == trajectory::v3::V3StreamProbe::Status::
                                                    FormatConflict
                                      ? "gate.session_format_conflict"
                                      : "gate.session_format_unsupported";
            report.message = "不认的账格式,不迁旧档: " + probe.detail;
            report.session_id = session_dir.filename().string();
            report.notes.push_back("gate.session_format_unsupported: " + probe.detail);
            return report;
        }
        case trajectory::v3::V3StreamProbe::Status::BadFirstLine:
        case trajectory::v3::V3StreamProbe::Status::EmptyFirstLine: {
            // 首行读不出 = 坏账(不是"没有这场"):Corrupt 分型,理由点名。
            SessionGateReport report;
            report.status = SessionGateStatus::Corrupt;
            report.error_code = "gate.v3_first_line_unreadable";
            report.message = "v3 主账首行读不出: " + probe.detail;
            report.session_id = session_dir.filename().string();
            report.notes.push_back("gate.v3_first_line_unreadable: " + probe.detail);
            return report;
        }
        case trajectory::v3::V3StreamProbe::Status::NotSessionDir:
        case trajectory::v3::V3StreamProbe::Status::V2Layout:
        case trajectory::v3::V3StreamProbe::Status::StreamMissing:
        default:
            return std::nullopt;  // 走 v2 老路(含目录不存在 → Missing)
    }

    // 主账验卷 + 树遍历 + 读模型(T00 读面,不另写 JSONL 扫描器)。
    const V3FactsRead facts = CollectV3SessionFacts(session_dir, probe.stream);
    if (!facts.ok) {
        return GateV3Unreadable(session_dir, probe.stream, facts.message);
    }

    SessionGateReport report;
    report.format = "v3";
    report.session_id = facts.sessions.front().session_id;
    report.v3_facts = std::move(facts.sessions);
    for (const auto& warning : facts.warnings) {
        report.notes.push_back("gate.v3_partial: " + warning);
    }
    // 各账的缺口(missing_blob/重复 owner/迟到观察…)透传点名;主账缺口
    // 属 partial,子账同款——不跳整场、不记零风险(T14)。
    for (const auto& session : report.v3_facts) {
        for (const auto& note : session.notes) {
            report.notes.push_back("gate.v3_partial: " + session.session_id + ": " + note);
        }
        report.stream_terminal_hashes[session.session_id + ":" + session.run_id] =
            session.terminal_hash;
    }
    report.session_status = report.sealed() ? "closed" : "active";
    if (report.sealed()) {
        report.status = SessionGateStatus::Analyzed;
    } else {
        report.status = SessionGateStatus::Active;
        report.error_code = "gate.active";
        report.message = "v3 session 未封口(session.ended 不在账):读已验高水位,成色 provisional";
    }
    return report;
}

}  // namespace

SessionGateReport GateSession(const std::filesystem::path& session_dir) {
    // ---- 格式分派(T14):先探 v3;认不出/旧布局走 v2 老路。----
    if (const auto v3 = GateSessionV3(session_dir); v3.has_value()) {
        return *v3;
    }

    SessionGateReport report;
    report.format = "v2";
    report.session_id = session_dir.filename().string();
    if (!DirExists(session_dir)) {
        report.format.clear();
        report.status = SessionGateStatus::Missing;
        report.error_code = "gate.session_missing";
        report.message = "没有这场 session:" + session_dir.string();
        return report;
    }
    if (const auto manifest = trajectory::ReadSessionJson(session_dir)) {
        report.session_id = manifest->session_id;
        report.workspace_key = manifest->workspace_key;
        report.session_status = manifest->status;
    } else {
        report.session_status = "unknown";
        report.notes.push_back("gate.session_manifest_missing: session.json 读不到");
    }

    // 链 + 父子边:trajectory 的同一只 verifier(不另写一套)。
    const trajectory::SessionVerifyReport verify = trajectory::VerifySessionDir(session_dir);
    if (!verify.ok) {
        report.error_code = verify.error_code.empty() ? "verify.failed" : verify.error_code;
        report.message = verify.message.empty() ? "session 验账没过" : verify.message;
        for (const auto& stream : verify.streams) {
            if (!stream.ok) {
                report.notes.push_back("gate.stream_rejected: " + stream.relative_path + ": " +
                                       stream.error_code);
            }
        }
        for (const auto& edge : verify.child_edges) {
            if (!edge.error_code.empty()) {
                report.notes.push_back("gate.child_edge_rejected: " + edge.child_run_id + ": " +
                                       edge.error_code);
            }
        }
    }

    // 逐文件再验一遍拿 truncated_tail 与高水位事件(VerifySessionDir 的
    // 报告不装事件体;分析器要吃)。
    bool truncated = false;
    // stream 清单与 session_usage_reader 同一套(只有 main/subagents/
    // workflows 三族;goals/loops 归 P0 后续,不在分析范围,不冒充)。
    const auto streams_files = [&]() -> std::vector<std::filesystem::path> {
        std::vector<std::filesystem::path> files;
        const auto push = [&](const std::filesystem::path& p) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(p, ec)) {
                files.push_back(p);
            }
        };
        push(session_dir / "main.jsonl");
        std::error_code ec;
        const std::filesystem::path subagents = session_dir / "subagents";
        if (std::filesystem::is_directory(subagents, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(subagents, ec)) {
                if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
                    files.push_back(entry.path());
                }
            }
        }
        const std::filesystem::path workflows = session_dir / "workflows";
        if (std::filesystem::is_directory(workflows, ec)) {
            for (const auto& run : std::filesystem::directory_iterator(workflows, ec)) {
                if (!run.is_directory()) {
                    continue;
                }
                push(run.path() / "workflow.jsonl");
                const std::filesystem::path nodes = run.path() / "nodes";
                if (std::filesystem::is_directory(nodes, ec)) {
                    for (const auto& node : std::filesystem::directory_iterator(nodes, ec)) {
                        if (node.is_regular_file() && node.path().extension() == ".jsonl") {
                            files.push_back(node.path());
                        }
                    }
                }
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }();

    // 分型(§16.3/A4):VerifySessionDir 只报"没过";截断(incomplete)还是
    // 坏账(corrupt)看逐文件的 truncated_tail 与 verify 的失败码。纯截断
    // 判 incomplete;链断/坏行/版本混写/父子边不对一律 corrupt——不出残账。
    bool corrupt = false;
    bool verify_truncated = false;
    for (const auto& stream : verify.streams) {
        if (stream.ok) {
            continue;
        }
        if (stream.error_code.find("truncat") != std::string::npos) {
            verify_truncated = true;
        } else {
            corrupt = true;
        }
    }
    for (const auto& edge : verify.child_edges) {
        if (!edge.error_code.empty()) {
            corrupt = true;
        }
    }
    for (const auto& path : streams_files) {
        const auto journal = trajectory::VerifyJournalFile(path);
        if (!journal.ok) {
            if (journal.truncated_tail) {
                truncated = true;
                report.notes.push_back("gate.stream_truncated: " + path.filename().string());
            } else {
                corrupt = true;
                report.notes.push_back("gate.stream_corrupt: " + path.filename().string() +
                                       ": " + journal.error_code);
            }
        }
    }
    truncated = truncated || verify_truncated;

    if (corrupt) {
        report.status = SessionGateStatus::Corrupt;
        if (report.error_code.empty()) {
            report.error_code = "gate.corrupt";
            report.message = "session 有坏账(hash 链/坏行/父子边没过),整间排除";
        }
        return report;
    }
    if (truncated) {
        report.status = SessionGateStatus::Incomplete;
        report.error_code = "gate.incomplete";
        report.message = "session 有 stream 尾行截断(崩溃中断),整间排除";
        return report;
    }

    // 链全过:装事件(高水位),按 run_id 字典序。
    for (const auto& path : streams_files) {
        auto envelopes = ParseStream(path);
        if (!envelopes.has_value()) {
            // VerifyJournalFile 过了但逐行解析失败:两边口径不一致也算坏账,
            // 不出半间残账。
            report.status = SessionGateStatus::Corrupt;
            report.error_code = "gate.stream_unparseable";
            report.message = "stream 验过却解析不动:" + path.filename().string();
            report.streams.clear();
            return report;
        }
        const std::string run_id =
            envelopes->empty() ? path.stem().string() : envelopes->front().run_id;
        if (!envelopes->empty()) {
            report.stream_terminal_hashes[run_id] = envelopes->back().event_hash;
        }
        report.streams.emplace_back(run_id, std::move(*envelopes));
    }
    if (report.sealed()) {
        report.status = SessionGateStatus::Analyzed;
    } else {
        report.status = SessionGateStatus::Active;
        report.error_code = "gate.active";
        report.message = "session 未封口:读已提交高水位,成色 provisional";
    }
    return report;
}

}  // namespace lubancode::insights
