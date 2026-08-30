// TerminalSessionNoticeSink 的实现(骨架拆解反弹·问题 2):终端画法自
// TerminalSessionController::Run 的两处直接 TermOut 段逐字搬来——label
// 对照、exit 段、⎿ 缩进行、StdoutWriteMutex 的锁规矩全部照旧。
#include "app/session_notice.hpp"

#include <mutex>

#include "cli/console_input.hpp"   // StdoutWriteMutex(与原直接打印路同一把锁)
#include "cli/terminal_port.hpp"  // TermOut

namespace lubancode::app {

void TerminalSessionNoticeSink::Emit(const SessionNotice& notice) {
    const lubancode::cli::Theme& theme = theme_;
    std::lock_guard<std::mutex> stdout_lock(lubancode::cli::StdoutWriteMutex());
    switch (notice.kind) {
        case SessionNotice::Kind::BackgroundTaskDone: {
            const char* label = "已结束";
            switch (notice.status) {
                case lubancode::tools::BackgroundTaskStatus::Completed: label = "完成(退出码 0)"; break;
                case lubancode::tools::BackgroundTaskStatus::Failed: label = "失败"; break;
                case lubancode::tools::BackgroundTaskStatus::Stopped: label = "已停止"; break;
                case lubancode::tools::BackgroundTaskStatus::StopFailed: label = "停止失败"; break;
                default: break;
            }
            lubancode::cli::TermOut() << theme.stats << "[后台任务 #" << notice.task_id << " "
                                      << label << "]";
            if (notice.status != lubancode::tools::BackgroundTaskStatus::Completed) {
                lubancode::cli::TermOut() << " (exit "
                          << (notice.exit_code.has_value() ? std::to_string(*notice.exit_code) : "unknown")
                          << ")";
            }
            lubancode::cli::TermOut() << " " << notice.command << theme.reset << "\n";
            break;
        }
        case SessionNotice::Kind::SubagentCompletion: {
            lubancode::cli::TermOut() << theme.tool_line << notice.title << theme.reset << "\n";
            for (const auto& note : notice.notes) {
                lubancode::cli::TermOut() << theme.stats << "  ⎿ " << note << theme.reset << "\n";
            }
            lubancode::cli::TermOut().flush();
            break;
        }
    }
}

}  // namespace lubancode::app
