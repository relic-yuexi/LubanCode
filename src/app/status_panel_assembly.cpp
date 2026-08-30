// BuildStatusPanelData 的实现(骨架拆解反弹·问题 2):字段折法自
// TerminalSessionController::Run 的循环顶逐字搬来,行为一字未改;空指针
// 字段按"该段收起"跳过(单测方便,controller 在场时全非空)。
#include "app/status_panel_assembly.hpp"

#include <filesystem>

#include "app/commands/background_commands.hpp"  // BuildBackgroundStatusSegment
#include "app/commands/goal_commands.hpp"        // BuildGoalLoopStatusSegment
#include "cli/context_tracker.hpp"
#include "cli/format_utils.hpp"  // BuildCacheNote
#include "cli/i18n.hpp"
#include "cli/record_command.hpp"  // RecorderStatusMarker
#include "config/config.hpp"      // EnvironmentOverridesActiveProvider/ToolCallingMode
#include "platform/paths.hpp"     // CurrentDirUtf8
#include "runtime/plan_mode.hpp"  // CollaborationMode
#include "runtime/session_runtime.hpp"
#include "runtime/worktree.hpp"  // WorktreeSession/CurrentGitBranch
#include "tools/background_tasks.hpp"

namespace lubancode::app {

lubancode::cli::StatusPanelData BuildStatusPanelData(const StatusPanelInputs& inputs,
                                                     lubancode::config::ToolCallingMode tool_calling) {
    lubancode::cli::StatusPanelData status_data;
    // status panel 每圈都重取 cwd 与 Git 分支。/worktree、run_command
    // 切目录/分支,或队列紧接着发下一条时,都不会挂着上一帧的旧值。
    if (inputs.current_model != nullptr) {
        status_data.model = *inputs.current_model;
    }
    status_data.cwd = lubancode::platform::CurrentDirUtf8();
    status_data.git_branch =
        lubancode::cli::CurrentGitBranch(std::filesystem::current_path());
    if (inputs.worktree_session != nullptr) {
        status_data.worktree = inputs.worktree_session->active_name();
    }
    if (inputs.config_result != nullptr && inputs.active_provider != nullptr) {
        status_data.provider =
            lubancode::config::EnvironmentOverridesActiveProvider(
                inputs.config_result->config, inputs.config_result->sources,
                inputs.config_result->config.active_provider)
                ? "env override / unbound"
                : *inputs.active_provider;
    }
    if (inputs.current_think != nullptr) {
        status_data.effort = *inputs.current_think;
    }
    if (inputs.context_tracker != nullptr) {
        const lubancode::cli::ContextTracker& tracker = *inputs.context_tracker;
        status_data.context_percent = tracker.UsagePercent();
        status_data.used_tokens = static_cast<long long>(tracker.current_tokens());
        status_data.window_tokens = static_cast<long long>(tracker.window_tokens());
        // 缓存注记(缓存诊断单):与回合内局部更新同一只 helper、同一只
        // tracker,空闲重建的第一帧不会先新后旧。
        status_data.cache_note = lubancode::cli::BuildCacheNote(tracker, !tracker.usage_stale());
        // 旧值标记同样出自 tracker:回合内 on_usage 局部发布的快照与这里整份
        // 重建读同一只 ContextTracker,数字与 ~ 标记完全一致。
        status_data.context_stale = tracker.usage_stale();
    }
    // REC 标记:录制中恒挂状态行第一段(见 StatusPanelData::rec)。轨迹档
    // 活动选段的文案(rec_override)压过老录制器标记;空串回落。
    if (!inputs.rec_override.empty()) {
        status_data.rec = inputs.rec_override;
    } else {
        static const std::optional<lubancode::skills::WorkflowRecorder> kNoRecorder;
        status_data.rec = lubancode::cli::RecorderStatusMarker(
            inputs.recorder != nullptr ? *inputs.recorder : kNoRecorder);
    }
    // 工具调用后端档(PTC 单):json 默认时留空(状态行零变化),programmatic/
    // auto 时恒亮一段,回落原因写全(规格 UI 节)。
    if (tool_calling != lubancode::config::ToolCallingMode::Json &&
        inputs.ptc_resolution != nullptr) {
        status_data.tools = *inputs.ptc_resolution;
    }
    // Plan 模式标记(只读研究硬闸单):与 confirm/auto/yolo 并列(规格
    // "plan · confirm"),不重置确认档。
    if (inputs.session_runtime != nullptr &&
        inputs.session_runtime->collaboration_mode() == lubancode::runtime::CollaborationMode::Plan) {
        status_data.plan_mode = lubancode::cli::tr("plan.mode_label");
    }
    // goal/loop 会话状态段(goal 单合流):有常驻自动工作在跑才挂。
    status_data.goal_loop = lubancode::app::BuildGoalLoopStatusSegment(inputs.goal, inputs.loop_scheduler);
    // 后台命令任务段(background 管理面单):台账里有任务才挂"后台 N 运行
    // / M 完成"。这里给的是圈边界那份基线;空闲 100ms 拍与流式 footer
    // 每帧另经 SetBackgroundStatusProvider 现折,后台起/收当场就变。
    status_data.background = lubancode::app::BuildBackgroundStatusSegment(
        lubancode::tools::BackgroundTaskRegistry::Instance().List());
    return status_data;
}

}  // namespace lubancode::app
