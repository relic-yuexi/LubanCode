// 状态面板(常驻状态行)的数据拼装(骨架拆解反弹·问题 2 自
// TerminalSessionController::Run 的循环顶拆出):每圈把 model/cwd/分支/
// worktree/provider/effort/context/cache 注记/REC/工具后端/plan/goal-loop/
// 后台段折成一份 cli::StatusPanelData。控制器只剩"每圈刷新两枚活字段 +
// 调函数 + SetStatusLineData"三步,拼装规则聚在这——单测填一份 inputs 就
// 能钉字段映射,不必起整个会话。
#pragma once

#include <optional>
#include <string>

#include "cli/format_utils.hpp"  // StatusPanelData

namespace lubancode::cli {
class ContextTracker;
class WorktreeSession;
}  // namespace lubancode::cli

namespace lubancode::config {
struct ConfigResult;
enum class ToolCallingMode;
}  // namespace lubancode::config

namespace lubancode::runtime {
class SessionRuntime;
namespace goal {
class GoalCoordinator;
}
namespace loop {
class LoopScheduler;
}
}  // namespace lubancode::runtime

namespace lubancode::skills {
class WorkflowRecorder;
}

namespace lubancode::app {

// 拼装材料:指针一律指调用方保活的真值。goal/loop 两枚每圈现取(接线器
// 的活口),controller 在调用前刷新;其余构造时绑一次。可空指针按"该段
// 收起"处理(与原先各分支的空语义对齐)。
struct StatusPanelInputs {
    const std::string* current_model = nullptr;        // 状态行 model 段
    const std::string* active_provider = nullptr;      // provider 段(env 未覆盖时)
    const std::string* current_think = nullptr;        // effort 段
    const std::string* ptc_resolution = nullptr;       // 工具后端档文案(条件见 cpp)
    const lubancode::cli::ContextTracker* context_tracker = nullptr;
    const lubancode::cli::WorktreeSession* worktree_session = nullptr;
    const lubancode::config::ConfigResult* config_result = nullptr;  // env override 判定
    const lubancode::runtime::SessionRuntime* session_runtime = nullptr;  // plan 模式档
    // REC 标记:录制器可空(没在录),空指针按"无录制器"处理。
    const std::optional<lubancode::skills::WorkflowRecorder>* recorder = nullptr;
    // REC 覆盖(P0-2 轨迹选段器):轨迹档活动选段的 REC 文案由调用方折好
    // 填这——非空即压过 recorder 标记;空串回落老录制器。
    std::string rec_override;
    // 每圈刷新的两枚活字段:
    lubancode::runtime::goal::GoalCoordinator* goal = nullptr;
    lubancode::runtime::loop::LoopScheduler* loop_scheduler = nullptr;
};

// 折一份状态面板数据。cwd/git 分支现场探(与原先一致,每圈重取,不挂旧帧);
// 后台任务段直接从 BackgroundTaskRegistry 全局台账现折(空台账给空串,
// 段收起)。plan 段与工具后端段各按档位条件挂——不满足条件的字段保持
// StatusPanelData 默认值(空串/空),与原先"不设即不画"同义。
lubancode::cli::StatusPanelData BuildStatusPanelData(const StatusPanelInputs& inputs,
                                                     lubancode::config::ToolCallingMode tool_calling);

}  // namespace lubancode::app
