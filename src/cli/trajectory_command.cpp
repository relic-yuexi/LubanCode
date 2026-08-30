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
#include "trajectory/replay.hpp"

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

int RunTrajectoryCommand(const TrajectoryCommandArgs& args) {
    if (args.verb != "verify" && args.verb != "replay" && args.verb != "harness-replay") {
        std::cerr << "用法: lubancode trajectory <verify|replay|harness-replay> <session-id>\n";
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
