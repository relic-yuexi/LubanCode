#include "insights/integrity_gate.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "trajectory/directory.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/schema.hpp"

namespace lubancode::insights {

const char* SessionGateStatusName(SessionGateStatus status) {
    switch (status) {
        case SessionGateStatus::Analyzed: return "analyzed";
        case SessionGateStatus::Active: return "active";
        case SessionGateStatus::Incomplete: return "incomplete";
        case SessionGateStatus::Corrupt: return "corrupt";
        case SessionGateStatus::Missing: return "missing";
    }
    return "unknown";
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

}  // namespace

SessionGateReport GateSession(const std::filesystem::path& session_dir) {
    SessionGateReport report;
    report.session_id = session_dir.filename().string();
    if (!DirExists(session_dir)) {
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
