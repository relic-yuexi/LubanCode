// `lubancode trajectory` 子命令实现(P0-3)。合同见 trajectory_command.hpp。
#include "cli/trajectory_command.hpp"

#include <filesystem>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "platform/paths.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/harness.hpp"
#include "trajectory/metrics.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/safety.hpp"
#include "trajectory/training_exporter.hpp"
#include "trajectory/usage_gc.hpp"

namespace lubancode::cli {

namespace {

std::filesystem::path DefaultTrajectoriesRoot() {
    const auto home = config::HomeLubancodeDir();
    if (!home.has_value()) {
        return {};
    }
    return tools::Utf8ToPath(*home) / "trajectories";
}

// 在 workspaces/<key>/sessions/ 下找 <session_id> 的目录。找不到给空。
std::filesystem::path FindSessionDir(const std::filesystem::path& root, const std::string& session_id) {
    std::error_code ec;
    const auto workspaces = root / "workspaces";
    if (!std::filesystem::is_directory(workspaces, ec)) {
        return {};
    }
    for (const auto& workspace : std::filesystem::directory_iterator(workspaces, ec)) {
        const auto candidate = workspace.path() / "sessions" / platform::Utf8ToPath(session_id);
        if (std::filesystem::is_directory(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

}  // namespace

// §12.2 容量/CI 档:usage / gc / doctor 吃 workspace-key(单段名,先过
// 安全校验再拼路径)。workspaces/<key> 找不到给空。
std::filesystem::path FindWorkspaceDir(const std::filesystem::path& root, const std::string& key) {
    if (!trajectory::IsSafeSingleSegment(key)) {
        return {};
    }
    std::error_code ec;
    const auto candidate = root / "workspaces" / platform::Utf8ToPath(key);
    if (std::filesystem::is_directory(candidate, ec)) {
        return candidate;
    }
    return {};
}

// usage:workspace 四笔容量账逐 session 报(只报账,不动文件系统)。
int RunUsageReport(const std::filesystem::path& workspace_dir, const std::string& key) {
    const auto report = trajectory::ScanWorkspaceUsage(workspace_dir / "sessions", key);
    std::cout << "workspace " << key << " —— " << report.sessions.size() << " 场 session, 共 "
              << (report.total_bytes / (1024 * 1024)) << " MiB\n";
    for (const auto& session : report.sessions) {
        std::cout << "  " << session.session_id << "  journal=" << session.journal_bytes << "B"
                  << "  blobs=" << session.referenced_blob_bytes << "B"
                  << "  rebuildable=" << session.rebuildable_bytes << "B"
                  << "  derived=" << session.derived_bytes << "B"
                  << "  文件 " << session.total_files() << " 个\n";
    }
    std::cout << "canonical JSONL 与 artifacts/ 引用 blob 不在自动清理面;"
                 "删它们只走显式的 session delete。\n";
    return 0;
}

// gc:§12.2 定死次序 temp→index→checkpoint→derived;--derived-only 才真删,
// 默认 dry-run 只报账。
int RunGc(const std::filesystem::path& workspace_dir, const std::string& key, bool derived_only) {
    const auto sessions_dir = workspace_dir / "sessions";
    std::error_code ec;
    if (!std::filesystem::is_directory(sessions_dir, ec)) {
        std::cerr << "workspace " << key << " 没有 sessions 目录\n";
        return 1;
    }
    int exit_code = 0;
    for (const auto& entry : std::filesystem::directory_iterator(sessions_dir, ec)) {
        if (ec) break;
        std::error_code dir_ec;
        if (!entry.is_directory(dir_ec) || dir_ec) {
            continue;
        }
        const auto result = trajectory::RunSessionGc(
            entry.path(), derived_only ? trajectory::GcScope::DerivedOnly
                                       : trajectory::GcScope::DryRun);
        std::cout << "  " << entry.path().filename().string() << "  可回收 "
                  << (result.plan.reclaimable_bytes / 1024) << " KiB("
                  << result.plan.items.size() << " 项)";
        if (result.applied) {
            std::cout << "  已删 " << (result.deleted_bytes / 1024) << " KiB / "
                      << result.deleted_files << " 个文件";
            if (!result.errors.empty()) {
                std::cout << "  失败 " << result.errors.size() << " 项";
                exit_code = 2;
            }
        }
        std::cout << "\n";
        for (const auto& error : result.errors) {
            std::cout << "    [!!] " << error << "\n";
        }
    }
    if (!derived_only) {
        std::cout << "dry-run 只报账;真清加 --derived-only。\n";
    }
    return exit_code;
}

// doctor:与 /doctor trajectory 同一份只读聚合(metrics.hpp 的共用格式)。
int RunDoctor(const std::filesystem::path& workspace_dir, const std::string& key) {
    const auto report = trajectory::BuildWorkspaceDoctorReport(
        workspace_dir.parent_path(), workspace_dir, key, std::nullopt, {});
    for (const std::string& line : trajectory::FormatWorkspaceDoctorReport(report)) {
        std::cout << line << "\n";
    }
    return 0;
}

// export/export-workspace(P0-5 §十一/§十四):Journal 只读投影成 training-v1
// 数据集。trajectory 关的会话没账可导——报空退 1,不造假。
int RunExport(const std::filesystem::path& root, const TrajectoryCommandArgs& args) {
    if (args.format != "training-v1") {
        std::cerr << "只认 --format training-v1,不认 \"" << args.format << "\"\n";
        return 1;
    }
    const trajectory::TrainingExportOptions options;  // 默认档:reasoning 剔除、
    // blob 回读 1 MiB 帽、写盘 16 MiB 磁盘门(§11.6/§12.2)。
    const auto report = args.verb == "export"
                            ? trajectory::ExportSessionTrainingV1(
                                  FindSessionDir(root, args.session_id), options)
                            : trajectory::ExportWorkspaceTrainingV1(
                                  FindWorkspaceDir(root, args.session_id), options);
    if (!report.ok()) {
        std::cerr << "training-v1 导出未过(" << report.error_code << "): " << report.message
                  << "\n";
        return report.error_code == "export.no_session_dir" ||
                       report.error_code == "export.no_streams"
                   ? 1
                   : 2;
    }
    std::cout << "training-v1 导出完成 —— " << report.episodes << " 枚 episode / "
              << report.streams << " 份 stream\n";
    for (const char* route : {"success", "failure", "partial", "excluded"}) {
        const auto it = report.counts.find(route);
        std::cout << "  " << route << ".jsonl  " << (it != report.counts.end() ? it->second : 0)
                  << " 枚\n";
    }
    if (!report.exclusion_reasons.empty()) {
        std::cout << "  排除缘由:\n";
        for (const auto& [reason, count] : report.exclusion_reasons) {
            std::cout << "    " << reason << "  ×" << count << "\n";
        }
    }
    std::cout << "  落盘 " << platform::PathToUtf8(report.export_dir)
              << (args.verb == "export-workspace" ? "(逐 session 各自落,此为末场;manifest.json 记 "
                                                     "config hash、过滤规则与逐文件 sha256)"
                                                  : "(manifest.json 记 config hash、过滤规则与逐文件 "
                                                    "sha256)")
              << "(派生物,可删可重算)\n";
    return 0;
}

int RunTrajectoryCommand(const TrajectoryCommandArgs& args) {
    if (args.verb == "export" || args.verb == "export-workspace") {
        if (args.session_id.empty()) {
            std::cerr << "缺 id: lubancode trajectory " << args.verb
                      << (args.verb == "export" ? " <session-id>" : " <workspace-key>")
                      << " --format training-v1\n";
            return 1;
        }
        const auto root = args.trajectories_root.empty() ? DefaultTrajectoriesRoot()
                                                         : tools::Utf8ToPath(args.trajectories_root);
        if (root.empty()) {
            std::cerr << "找不到主目录,轨迹账无处寻\n";
            return 1;
        }
        return RunExport(root, args);
    }
    if (args.verb == "usage" || args.verb == "gc" || args.verb == "doctor") {
        if (args.session_id.empty()) {
            std::cerr << "缺 workspace key: lubancode trajectory " << args.verb
                      << " <workspace-key>\n";
            return 1;
        }
        const auto root = args.trajectories_root.empty() ? DefaultTrajectoriesRoot()
                                                         : tools::Utf8ToPath(args.trajectories_root);
        if (root.empty()) {
            std::cerr << "找不到主目录,轨迹账无处寻\n";
            return 1;
        }
        const auto workspace_dir = FindWorkspaceDir(root, args.session_id);
        if (workspace_dir.empty()) {
            std::cerr << "找不到 workspace " << args.session_id << "(单段名,不带路径)\n";
            return 1;
        }
        if (args.verb == "usage") {
            return RunUsageReport(workspace_dir, args.session_id);
        }
        if (args.verb == "gc") {
            return RunGc(workspace_dir, args.session_id, args.gc_derived_only);
        }
        return RunDoctor(workspace_dir, args.session_id);
    }
    if (args.verb != "verify" && args.verb != "replay" && args.verb != "harness-replay") {
        std::cerr << "用法: lubancode trajectory "
                     "<verify|replay|harness-replay|usage|gc|doctor|export|export-workspace> "
                     "<session-id|workspace-key>\n";
        return 1;
    }
    if (args.session_id.empty()) {
        std::cerr << "缺 session id: lubancode trajectory " << args.verb << " <session-id>\n";
        return 1;
    }
    const auto root = args.trajectories_root.empty() ? DefaultTrajectoriesRoot()
                                                     : tools::Utf8ToPath(args.trajectories_root);
    if (root.empty()) {
        std::cerr << "找不到主目录,轨迹账无处寻\n";
        return 1;
    }
    const auto session_dir = FindSessionDir(root, args.session_id);
    if (session_dir.empty()) {
        std::cerr << "找不到 session " << args.session_id << "(在 " << platform::PathToUtf8(root)
                  << " 的 workspaces/*/sessions/ 下没有)\n";
        return 1;
    }

    if (args.verb == "verify") {
        const auto report = trajectory::VerifySessionDir(session_dir);
        std::cout << "session " << args.session_id << " —— "
                  << (report.ok ? "verify 通过" : "verify 未过(" + report.error_code + ")") << "\n";
        for (const auto& stream : report.streams) {
            std::cout << "  " << (stream.ok ? "[ok] " : "[!!] ") << stream.relative_path << "  "
                      << stream.events << " 事件"
                      << (stream.run_terminal ? "  终态 " + stream.terminal_kind
                                              : std::string("  未收口"))
                      << (!stream.ok && !stream.error_code.empty() ? "  " + stream.error_code
                                                                   : std::string())
                      << "\n";
        }
        for (const auto& edge : report.child_edges) {
            std::cout << "  " << (edge.error_code.empty() ? "[ok] " : "[!!] ") << "edge "
                      << edge.parent_run_id << " -> " << edge.child_run_id
                      << (edge.background_spawn ? "(后台)" : "")
                      << "  子终态 hash " << edge.child_terminal_hash.substr(0, 12)
                      << (!edge.error_code.empty() ? "  " + edge.error_code : std::string()) << "\n";
        }
        return report.ok ? 0 : 2;
    }

    const auto stream = session_dir / "main.jsonl";
    if (!std::filesystem::exists(stream)) {
        std::cerr << "session 目录里没有 main.jsonl\n";
        return 1;
    }
    if (args.verb == "replay") {
        const auto fold = trajectory::FoldStreamReplay(stream);
        if (!fold.ok()) {
            std::cerr << "exact replay 未过(" << fold.error_code << "): " << fold.message << "\n";
            return 2;
        }
        const auto& state = fold.state;
        std::cout << "exact replay —— state hash "
                  << trajectory::ComputeReplayStateHash(state).substr(0, 16) << "…\n";
        std::cout << "  折叠 " << state.integrity.events_folded << " 事件"
                  << "  turn " << state.turns.size() << "  请求步 " << state.requests.size()
                  << "  工具 " << state.tools.size() << "  证据 " << state.evidence.size() << "\n";
        std::cout << "  对话投影 " << state.effective_conversation.size() << " 条"
                  << "  悬空工具 " << state.integrity.dangling_tools
                  << (state.integrity.unknown_side_effects ? "  (含未知副作用)" : "") << "\n";
        std::cout << "  run " << (state.run_terminal_state.empty() ? "未收口" : state.run_terminal_state)
                  << "  start_reason=" << state.start_reason << "\n";
        return 0;
    }

    // harness-replay:录制桩重放,divergence 立即报。
    const auto report = trajectory::RunHarnessReplay(stream);
    if (!report.ok) {
        std::cerr << "harness replay 未过(" << report.error_code << ")";
        if (report.divergence.has_value()) {
            std::cerr << "  divergence@" << report.divergence->stage << "/"
                      << report.divergence->step_id << ": " << report.divergence->reason
                      << "  期望 " << report.divergence->expected.substr(0, 16)
                      << "  实得 " << report.divergence->actual.substr(0, 16);
        }
        std::cerr << "\n";
        return 2;
    }
    std::cout << "harness replay —— 模型步 " << report.model_steps_consumed << "  工具步 "
              << report.tool_steps_consumed << "  state hash "
              << report.replay_state_hash.substr(0, 16) << "…\n";
    return 0;
}

}  // namespace lubancode::cli
