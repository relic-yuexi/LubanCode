#include "trajectory/metrics.hpp"

#include <sstream>

#include "trajectory/directory.hpp"

namespace lubancode::trajectory {
namespace {

StreamHealth CheckStream(const std::string& label, const std::filesystem::path& path) {
    StreamHealth health;
    health.label = label;
    health.path = path;
    std::error_code ec;
    health.exists = std::filesystem::exists(path, ec) && !ec;
    if (!health.exists) {
        return health;
    }
    health.verify = VerifyJournalFile(path);
    health.run_terminal = ScanStreamFacts(path).run_terminal;
    return health;
}

void ScanDirForStreams(const std::filesystem::path& dir, const std::string& prefix,
                       std::vector<StreamHealth>* out) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec) || file_ec || entry.path().extension() != ".jsonl") {
            continue;
        }
        const std::string label = prefix + entry.path().stem().string();
        out->push_back(CheckStream(label, entry.path()));
    }
}

}  // namespace

SessionDoctorReport BuildSessionDoctorReport(const std::filesystem::path& session_dir) {
    SessionDoctorReport report;
    report.session_id = session_dir.filename().string();

    if (const auto manifest = ReadSessionJson(session_dir); manifest.has_value()) {
        report.manifest_readable = true;
        if (const auto status = SessionStatusFromName(manifest->status); status.has_value()) {
            report.status = *status;
        }
    }

    report.streams.push_back(CheckStream("main", session_dir / "main.jsonl"));
    ScanDirForStreams(session_dir / "subagents", "agent:", &report.streams);
    ScanDirForStreams(session_dir / "goals", "goal:", &report.streams);
    ScanDirForStreams(session_dir / "loops", "loop:", &report.streams);

    std::error_code ec;
    const std::filesystem::path workflows_dir = session_dir / "workflows";
    if (std::filesystem::exists(workflows_dir, ec)) {
        for (const auto& run : std::filesystem::directory_iterator(workflows_dir, ec)) {
            if (ec) break;
            std::error_code dir_ec;
            if (!run.is_directory(dir_ec) || dir_ec) {
                continue;
            }
            const std::string wf_id = run.path().filename().string();
            const std::filesystem::path main_stream = run.path() / "workflow.jsonl";
            if (std::filesystem::exists(main_stream, ec)) {
                report.streams.push_back(CheckStream("workflow:" + wf_id, main_stream));
            }
            ScanDirForStreams(run.path() / "nodes", "node:" + wf_id + "/", &report.streams);
        }
    }

    for (const auto& stream : report.streams) {
        if (stream.exists && !stream.run_terminal) {
            ++report.unterminated_stream_count;
        }
    }

    report.capacity = ScanSessionCapacity(session_dir);
    return report;
}

WorkspaceDoctorReport BuildWorkspaceDoctorReport(const std::filesystem::path& trajectories_root,
                                                 const std::filesystem::path& workspace_dir,
                                                 const std::string& workspace_key,
                                                 const std::optional<std::string>& active_session_id,
                                                 std::vector<std::string> recent_errors) {
    WorkspaceDoctorReport report;
    report.workspace_key = workspace_key;
    report.trajectories_root = trajectories_root;
    report.recent_errors = std::move(recent_errors);

    std::error_code ec;
    const std::filesystem::space_info space = std::filesystem::space(trajectories_root, ec);
    if (!ec) {
        report.disk_space_known = true;
        report.disk_free_bytes = static_cast<std::uint64_t>(space.available);
        report.disk_total_bytes = static_cast<std::uint64_t>(space.capacity);
    } else {
        report.notes.push_back("磁盘余量查询失败: " + ec.message());
    }

    if (active_session_id.has_value() && !active_session_id->empty()) {
        const std::filesystem::path session_dir = workspace_dir / "sessions" / *active_session_id;
        report.active_session = BuildSessionDoctorReport(session_dir);
    }
    return report;
}

std::vector<std::string> FormatWorkspaceDoctorReport(const WorkspaceDoctorReport& report) {
    std::vector<std::string> lines;
    lines.push_back("workspace: " + report.workspace_key);
    if (report.disk_space_known) {
        std::ostringstream oss;
        oss << "磁盘余量: " << (report.disk_free_bytes / (1024 * 1024)) << " MiB / "
            << (report.disk_total_bytes / (1024 * 1024)) << " MiB";
        lines.push_back(oss.str());
    } else {
        lines.push_back("磁盘余量: 未知");
    }
    lines.push_back("队列高水位: " + std::to_string(report.queue_high_water_mark) +
                    "(单写者同步提交,恒 <=1)");
    if (!report.active_session.has_value()) {
        lines.push_back("active session: 无");
    } else {
        const auto& session = *report.active_session;
        lines.push_back("active session: " + session.session_id + "  status=" +
                        SessionStatusName(session.status));
        for (const auto& stream : session.streams) {
            std::ostringstream oss;
            oss << "  [" << stream.label << "] ";
            if (!stream.exists) {
                oss << "不存在";
            } else if (!stream.verify.ok) {
                oss << "verify 失败: " << stream.verify.error_code
                    << (stream.verify.truncated_tail ? " (尾行截断)" : "");
            } else {
                oss << "ok  events=" << stream.verify.events
                    << (stream.run_terminal ? "  terminal" : "  未收口");
            }
            lines.push_back(oss.str());
        }
        std::ostringstream cap;
        cap << "容量: journal=" << session.capacity.journal_bytes
            << "B  blobs=" << session.capacity.referenced_blob_bytes
            << "B  rebuildable=" << session.capacity.rebuildable_bytes
            << "B  derived=" << session.capacity.derived_bytes << "B";
        lines.push_back(cap.str());
        lines.push_back("未收口 stream 数: " + std::to_string(session.unterminated_stream_count));
    }
    if (report.recent_errors.empty()) {
        lines.push_back("最近 I/O 错误: 无");
    } else {
        lines.push_back("最近 I/O 错误:");
        for (const auto& error : report.recent_errors) {
            lines.push_back("  " + error);
        }
    }
    for (const auto& note : report.notes) {
        lines.push_back("note: " + note);
    }
    return lines;
}

bool HasDiskReserve(const std::filesystem::path& target_dir, std::uint64_t reserve_bytes) {
    std::error_code ec;
    std::filesystem::path probe = target_dir;
    // 目标目录可能尚未创建(create-new 之前先探路);沿路径向上找一层已
    // 存在的祖先来问文件系统余量,找不到任何存在的祖先就保守判"不足"。
    while (!probe.empty() && !std::filesystem::exists(probe, ec)) {
        const auto parent = probe.parent_path();
        if (parent == probe) {
            break;
        }
        probe = parent;
    }
    if (probe.empty() || !std::filesystem::exists(probe, ec) || ec) {
        return false;
    }
    const std::filesystem::space_info space = std::filesystem::space(probe, ec);
    if (ec) {
        return false;
    }
    return static_cast<std::uint64_t>(space.available) >= reserve_bytes;
}

}  // namespace lubancode::trajectory
