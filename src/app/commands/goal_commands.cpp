// /goal 命令处理器实现(纯排版与 gate;状态机调用在 interactive_session)。

#include "app/commands/goal_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)

#include <chrono>
#include <utility>

#include <nlohmann/json.hpp>

#include "app/hook_runtime.hpp"
#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/slash_commands.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "hooks/dispatcher.hpp"
#include "hooks/hash.hpp"
#include "platform/paths.hpp"
#include "sessions/session_store.hpp"
#include "tools/agent_tool.hpp"

#include <cstdlib>
#include <vector>

#include "platform/paths.hpp"

namespace lubancode::app {

bool GoalsDisabledByEnv() {
    const auto value = lubancode::platform::GetEnvVar("LUBANCODE_DISABLE_GOALS");
    if (!value.has_value() || value->empty()) return false;
    return *value == "1" || *value == "true" || *value == "TRUE" || *value == "yes";
}

lubancode::runtime::goal::GoalCoordinator::Options GoalOptionsFromConfig(
    bool features_goals, const lubancode::config::GoalsConfig& goals) {
    lubancode::runtime::goal::GoalCoordinator::Options options;
    // env 总闸只关功能不改存档;features.goals 是正门。
    options.goals_enabled = features_goals && !GoalsDisabledByEnv();
    options.default_max_elapsed_ms = goals.max_elapsed_ms;
    options.default_max_iterations = goals.max_iterations;
    options.default_max_no_progress_iterations = goals.max_no_progress_iterations;
    options.default_max_same_blocker_iterations = goals.max_same_blocker_iterations;
    options.default_max_consecutive_provider_failures = goals.max_consecutive_provider_failures;
    return options;
}

// 错误码 → 一句人话(稳定码原样带出,人话只做引导)。
std::string DescribeGoalErrorCode(const std::string& code, const std::string& message) {
    if (code == lubancode::runtime::goal::kErrGoalAlreadyActive) {
        return "已有一只活动目标。要改用 /goal edit,不用了 /goal clear;两只并行请另开会话。";
    }
    if (code == lubancode::runtime::goal::kErrGoalTerminal) {
        return "目标已收账(" + message + ");Achieved 后要再跑,新建一只目标。";
    }
    if (code == lubancode::runtime::goal::kErrGoalRevisionConflict) {
        return "目标已被改过(revision 冲突):重看 /goal status 再操作。";
    }
    if (code == lubancode::runtime::goal::kErrGoalBusy) {
        return "目标正在跑;这个命令等安全边界再生效(先 /goal pause)。";
    }
    if (code == lubancode::runtime::goal::kErrGoalBudgetExhausted) {
        return "预算已尽(" + message + ")";
    }
    if (code == lubancode::runtime::goal::kErrGoalObjectiveEmpty) {
        return "objective 不能为空。写清做什么、拿什么验、何时停。";
    }
    if (code == lubancode::runtime::goal::kErrGoalObjectiveTooLong) {
        return "objective 超过 4000 字符上限。长说明写进文件(如 docs/goal.md),目标里引用它。";
    }
    return message.empty() ? code : message;
}

namespace {

// 状态中文名(排版用;机器判断仍认稳定字符串)。
std::string GoalStateLabel(const std::string& state) {
    if (state == "preparing") return "拟合同";
    if (state == "active") return "待续轮";
    if (state == "running") return "执行中";
    if (state == "evaluating") return "验收中";
    if (state == "pausing") return "暂停中(等安全边界)";
    if (state == "paused") return "已暂停";
    if (state == "awaiting_approval") return "等审批";
    if (state == "awaiting_user") return "等用户";
    if (state == "blocked") return "受阻";
    if (state == "achieved") return "已达标";
    if (state == "budget_exhausted") return "预算尽";
    if (state == "suspended_by_policy") return "策略挂起";
    if (state == "failed") return "失败";
    if (state == "cleared") return "已清除";
    return state;
}

}  // namespace

GoalCommandOutcome FormatGoalStatus(const lubancode::runtime::goal::GoalCoordinator& coordinator,
                                    std::int64_t now_ms) {
    GoalCommandOutcome out;
    const nlohmann::json status = coordinator.Status(now_ms);
    out.payload = status;
    if (!status.value("has_goal", false)) {
        out.ok = true;
        out.lines.push_back(status.value("goals_enabled", false)
                                ? "当前会话没有目标。用 /goal <objective> 立一只(写明终点与验证法)。"
                                : "当前会话没有目标;goals 功能未开启(features.goals = true 才可用)。");
        return out;
    }
    const nlohmann::json& goal = status.at("goal");
    const std::string id = goal.value("id", std::string());
    const std::string state = status.value("state_label", std::string());
    const int revision = goal.value("revision", 1);
    const int iteration = goal.at("counters").value("iterations_started", 0);

    out.ok = true;
    std::string head = id + " · " + GoalStateLabel(state) + " · r" + std::to_string(revision);
    if (iteration > 0) head += " · iter " + std::to_string(iteration);
    out.lines.push_back(head);

    const std::string objective = goal.value("objective", std::string());
    if (objective.size() > 80) {
        out.lines.push_back("目标: " + objective.substr(0, 80) + "…");
    } else {
        out.lines.push_back("目标: " + objective);
    }
    const nlohmann::json& checkpoint = goal.at("checkpoint");
    if (checkpoint.value("summary", std::string()).empty()) {
        out.lines.push_back("当前: (还没有检查点)");
    } else {
        out.lines.push_back("当前: " + checkpoint.value("summary", std::string()));
        const std::string next = checkpoint.value("next_action", std::string());
        if (!next.empty()) out.lines.push_back("下一步: " + next);
    }
    if (status.contains("last_decision")) {
        out.lines.push_back("判定: " + status.value("last_decision", std::string()));
    }
    const int no_progress = status.value("no_progress_streak", 0);
    const int same_blocker = status.value("same_blocker_streak", 0);
    out.lines.push_back("防空转: 无进展连击 " + std::to_string(no_progress) +
                        " · 同 blocker 连击 " + std::to_string(same_blocker));
    if (status.contains("spent")) {
        const nlohmann::json& spent = status.at("spent");
        std::string line = "预算: iter " + std::to_string(spent.value("iterations", 0));
        if (spent.contains("iterations_limit")) {
            line += "/" + std::to_string(spent.value("iterations_limit", 0));
        }
        if (spent.contains("elapsed_ms")) {
            const std::int64_t secs = spent.value("elapsed_ms", std::int64_t{0}) / 1000;
            line += " · 用时 " + std::to_string(secs / 60) + "m" + std::to_string(secs % 60) + "s";
        }
        if (spent.contains("usage")) {
            const nlohmann::json& usage = spent.at("usage");
            if (usage.value("usage_reported", false)) {
                const std::int64_t total = usage.value("input_tokens", std::int64_t{0}) +
                                           usage.value("output_tokens", std::int64_t{0});
                line += " · token " + std::to_string(total) + " reported";
            } else {
                line += " · token 未报告";
            }
        }
        out.lines.push_back(line);
    }
    if (status.value("pause_requested", false)) {
        out.lines.push_back("(pause 已请求,正在跑的轮在下一安全边界收口)");
    }
    return out;
}

std::vector<std::string> BuildGoalClearConfirmLines(const lubancode::runtime::goal::GoalTask& task) {
    std::vector<std::string> lines;
    lines.push_back("将要清除目标 " + task.id + "(r" + std::to_string(task.revision) + ")");
    std::string preview = task.objective;
    if (preview.size() > 60) preview = preview.substr(0, 60) + "…";
    lines.push_back("目标: " + preview);
    lines.push_back("已跑 " + std::to_string(task.counters.iterations_started) + " 轮;审计账保留在会话存档。");
    lines.push_back("clear 不撤销已改过的文件;要回滚请用 git/undo 工具。");
    lines.push_back("确认清除? (y/N)");
    return lines;
}

// ---- /goal 会话接线(终端接线收尾单自大类搬出;原文随行,输出走 TerminalPort) ----

lubancode::app::CommandFlow HandleGoalCommand(const lubancode::cli::ParsedGoalCommand& goal,
                                               const GoalWiring& wiring) {
    auto& out = lubancode::cli::TermOut();
    const lubancode::cli::Theme& theme = *wiring.theme;
    lubancode::runtime::goal::GoalCoordinator& coordinator = *wiring.coordinator;
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    const std::string workspace_root = lubancode::platform::CurrentDirUtf8();

    using Action = lubancode::cli::GoalCommandAction;
    if (goal.action == Action::View || goal.action == Action::Status) {
        // 查账纯本地输出,不发模型(单子"状态查询不发模型")。
        const auto outcome = lubancode::app::FormatGoalStatus(coordinator, now_ms);
        for (const std::string& line : outcome.lines) {
            out << theme.stats << line << theme.reset << "\n";
        }
        return lubancode::app::CommandFlow::Continue;
    }

    if (goal.action == Action::Create) {
        const auto result = coordinator.Create(goal.objective, workspace_root, workspace_root, now_ms);
        if (!result.ok) {
            out << theme.error
                << lubancode::app::DescribeGoalErrorCode(result.error_code, result.error_message)
                << theme.reset << "\n";
            if (result.error_code == lubancode::runtime::goal::kErrGoalStoreUnavailable) {
                out << theme.stats
                    << "开启:配置文件里 [features] goals = true(环境变量 LUBANCODE_DISABLE_GOALS=1 是总闸)"
                    << theme.reset << "\n";
            }
            return lubancode::app::CommandFlow::Continue;
        }
        out << theme.stats << "目标已立: " << result.payload.value("goal_id", std::string())
            << "(状态 " << result.payload.value("state", std::string())
            << ")。首轮将先拟合同(做什么/不动什么/拿什么验/何时停),合同冻结后才开始排轮。"
            << theme.reset << "\n";
        EmitGoalHook(wiring, lubancode::hooks::HookEvent::GoalCreated,
                     nlohmann::json{{"goal_id", result.payload.value("goal_id", std::string())},
                                    {"objective_preview", goal.objective.substr(0, 120)}},
                     /*match_value=*/result.payload.value("state", std::string()));
        return lubancode::app::CommandFlow::Continue;
    }

    if (goal.action == Action::Edit) {
        const auto* task = coordinator.task();
        const int expected = task != nullptr ? task->revision : 0;
        const auto result = coordinator.Edit(goal.objective, expected, now_ms);
        if (!result.ok) {
            out << theme.error
                << lubancode::app::DescribeGoalErrorCode(result.error_code, result.error_message)
                << theme.reset << "\n";
            return lubancode::app::CommandFlow::Continue;
        }
        out << theme.stats << "目标已改(revision " << result.payload.value("revision", 0)
            << ");合同重拟,防空转连击清零,用量账保留。" << theme.reset << "\n";
        return lubancode::app::CommandFlow::Continue;
    }

    if (goal.action == Action::Pause) {
        const auto result = coordinator.Pause(now_ms);
        if (!result.ok) {
            out << theme.error
                << lubancode::app::DescribeGoalErrorCode(result.error_code, result.error_message)
                << theme.reset << "\n";
            return lubancode::app::CommandFlow::Continue;
        }
        out << theme.stats;
        if (result.payload.value("immediate", true)) {
            out << "目标已暂停;checkpoint/预算/防空转账都留着。";
        } else {
            out << "pause 已请求;正在跑的轮在下一安全边界收口。";
        }
        out << theme.reset << "\n";
        EmitGoalHook(wiring, lubancode::hooks::HookEvent::GoalPaused,
                     nlohmann::json{{"goal_id", coordinator.task() != nullptr
                                                     ? coordinator.task()->id
                                                     : std::string()},
                                    {"immediate", result.payload.value("immediate", true)}},
                     /*match_value=*/"user");
        return lubancode::app::CommandFlow::Continue;
    }

    if (goal.action == Action::Resume) {
        const auto* task = coordinator.task();
        const int expected = task != nullptr ? task->revision : 0;
        const auto result = coordinator.Resume(expected, now_ms);
        if (!result.ok) {
            out << theme.error
                << lubancode::app::DescribeGoalErrorCode(result.error_code, result.error_message)
                << theme.reset << "\n";
            return lubancode::app::CommandFlow::Continue;
        }
        out << theme.stats << "目标已续(从最后 checkpoint 起,不重放旧 iteration)。"
            << theme.reset << "\n";
        return lubancode::app::CommandFlow::Continue;
    }

    if (goal.action == Action::Clear) {
        const auto* task = coordinator.task();
        if (task == nullptr) {
            out << theme.stats << "当前会话没有目标。" << theme.reset << "\n";
            return lubancode::app::CommandFlow::Continue;
        }
        for (const std::string& line : lubancode::app::BuildGoalClearConfirmLines(*task)) {
            out << theme.stats << line << theme.reset << "\n";
        }
        const std::optional<std::string> answer = lubancode::cli::ReadLine("y/N", theme, true);
        if (!answer.has_value() || !(*answer == "y" || *answer == "Y" || *answer == "yes")) {
            out << theme.stats << "未清除,目标照旧。" << theme.reset << "\n";
            return lubancode::app::CommandFlow::Continue;
        }
        const auto result = coordinator.Clear(now_ms);
        if (!result.ok) {
            out << theme.error
                << lubancode::app::DescribeGoalErrorCode(result.error_code, result.error_message)
                << theme.reset << "\n";
            return lubancode::app::CommandFlow::Continue;
        }
        out << theme.stats << "目标已清除;审计账保留在会话存档,已改文件不撤销。" << theme.reset
            << "\n";
        return lubancode::app::CommandFlow::Continue;
    }
    return lubancode::app::CommandFlow::Continue;
}

void EmitGoalHook(const GoalWiring& wiring, lubancode::hooks::HookEvent event, nlohmann::json fields,
                  const std::string& match_value) {
    // additionalContext(OutputCapabilities 已在 events.hpp 定死:没有
    // permission_decision、can_block 恒 false——Hook 不可直接写 Achieved,
    // GoalCompleted 失败不把 Achieved 改回 Active)。goal_id/revision 由
    // 调用方塞进 fields,这里只管空定义表早退。
    lubancode::hooks::HookDispatcher* dispatcher = lubancode::app::HookRuntime();
    if (dispatcher == nullptr || dispatcher->Empty() || !dispatcher->HasHandlersFor(event)) {
        return;
    }
    if (wiring.session_store != nullptr && wiring.session_store->active()) {
        fields["session_id"] = wiring.session_store->session_id();
    }
    lubancode::hooks::HookPayload payload;
    payload.event = event;
    payload.fields = std::move(fields);
    payload.match_value = match_value;
    dispatcher->Emit(event, payload);
}

std::string BuildGoalLoopStatusSegment(lubancode::runtime::goal::GoalCoordinator* goal,
                                       lubancode::runtime::loop::LoopScheduler* loop) {
    // 只是 GoalState 的显示投影):
    //   run=Running/Preparing/Active·Pausing  eval=Evaluating
    //   pause=Paused/AwaitingApproval/AwaitingUser/SuspendedByPolicy
    //   blocked=Blocked  done=Achieved  budget=BudgetExhausted  x=Failed/Cleared
    // loop 只数非终态非 Paused 的活任务;next 给最近一拍还差多少(已到点/
    // 没有排程时省略)。
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    std::string goal_part;
    if (goal != nullptr && goal->task() != nullptr) {
        using GS = lubancode::runtime::goal::GoalState;
        const auto* task = goal->task();
        const char* code = "x";
        switch (task->state) {
            case GS::Preparing:
            case GS::Active:
            case GS::Running:
            case GS::Pausing:
                code = "run";
                break;
            case GS::Evaluating:
                code = "eval";
                break;
            case GS::Paused:
            case GS::AwaitingApproval:
            case GS::AwaitingUser:
            case GS::SuspendedByPolicy:
                code = "pause";
                break;
            case GS::Blocked:
                code = "blocked";
                break;
            case GS::Achieved:
                code = "done";
                break;
            case GS::BudgetExhausted:
                code = "budget";
                break;
            case GS::Failed:
            case GS::Cleared:
                code = "x";
                break;
        }
        goal_part = std::string("goal ") + code;
        if (task->counters.iterations_started > 0) {
            goal_part += "·iter" + std::to_string(task->counters.iterations_started);
        }
        if (task->revision > 1) {
            goal_part += "·r" + std::to_string(task->revision);
        }
    }
    std::string loop_part;
    if (loop != nullptr) {
        int active = 0;
        std::int64_t next_due = 0;
        bool has_next = false;
        for (const auto& view : loop->Snapshot(now_ms)) {
            if (lubancode::runtime::loop::IsLoopTerminal(view.task.state) ||
                view.task.state == lubancode::runtime::loop::LoopTaskState::Paused) {
                continue;
            }
            ++active;
            if (!has_next || view.task.next_due_at_ms < next_due) {
                next_due = view.task.next_due_at_ms;
                has_next = true;
            }
        }
        if (active > 0) {
            loop_part = "loop×" + std::to_string(active);
            if (has_next && next_due > now_ms) {
                const std::int64_t secs = (next_due - now_ms) / 1000;
                if (secs < 60) {
                    loop_part += " next " + std::to_string(secs) + "s";
                } else if (secs < 3600) {
                    loop_part += " next " + std::to_string(secs / 60) + "m";
                } else {
                    loop_part += " next " + std::to_string(secs / 3600) + "h";
                }
            }
        }
    }
    if (goal_part.empty() && loop_part.empty()) {
        return std::string();
    }
    if (goal_part.empty()) {
        return loop_part;
    }
    if (loop_part.empty()) {
        return goal_part;
    }
    return goal_part + " · " + loop_part;
}

void NoteSubagentCompletionForGoal(const GoalWiring& wiring) {
    // 子代理完成事件喂 goal 的证据/进度账(goal 单合流项)。没有活跃 goal
    // 或没有待回流完成,零影响。
    if (wiring.coordinator == nullptr || wiring.agent_tool == nullptr) {
        return;
    }
    lubancode::runtime::goal::GoalCoordinator& coordinator = *wiring.coordinator;
    const auto* task = coordinator.task();
    if (task == nullptr || lubancode::runtime::goal::IsGoalTerminal(task->state)) {
        return;  // terminal 后迟到的子代理结果:只留审计,不喂账(单子)
    }
    const std::vector<int> ids = wiring.agent_tool->UndeliveredCompletionTaskIds();
    if (ids.empty()) {
        return;
    }
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    const std::string iteration_id =
        task->id + "/iter-" + std::to_string(task->counters.iterations_started);
    int evidence_seq = static_cast<int>(coordinator.evidence_count());
    for (const int subagent_id : ids) {
        const auto detail = wiring.agent_tool->TaskDetail(subagent_id);
        if (!detail.has_value()) {
            continue;  // 台账里没有了:跳过不误伤
        }
        // usage 归 goal(单子:"子代理由该 iteration 派生,其 usage 归 goal")。
        lubancode::runtime::goal::GoalUsage sub_usage;
        sub_usage.input_tokens = detail->input_tokens;
        sub_usage.output_tokens = detail->output_tokens;
        sub_usage.cache_read_tokens = detail->cache_read_tokens;
        sub_usage.cache_creation_tokens = detail->cache_creation_tokens;
        sub_usage.request_count = detail->steps_used;
        sub_usage.usage_reported = detail->usage_reported;
        coordinator.AddUsage(sub_usage);
        // 结果折二级证据:producer 标 subagent,facts 带子任务 id 与 agent
        // 类型;content 锚用结果正文的 hash(有结果才立;空结果不立证据,
        // 免得查表里堆空壳)。单子:仅"子代理说通过了"仍是二级证据——
        // HardGateAchieved 只认 fresh 的一级证据,criterion 拿它顶数会在
        // 查表时露馅(goal_id 对得上,fresh 对得上,但 checkpoint 引用与
        // evaluator 判词另有一致性检查)。
        if (detail->result.empty()) {
            continue;
        }
        lubancode::runtime::goal::GoalEvidence evidence;
        evidence.id = "ev-" + std::to_string(++evidence_seq);
        evidence.kind = lubancode::runtime::goal::EvidenceKind::ToolResult;
        evidence.goal_id = task->id;
        evidence.iteration_id = iteration_id;
        evidence.tool_use_id = "subagent-" + std::to_string(subagent_id);
        evidence.producer = "subagent:" + detail->agent_type;
        evidence.facts["subagent_task_id"] = subagent_id;
        evidence.facts["agent_type"] = detail->agent_type;
        evidence.facts["title"] = detail->title;
        evidence.facts["steps_used"] = detail->steps_used;
        evidence.facts["tool_call_count"] = detail->tool_calls.size();
        // 终态枚举翻稳定串(翻不出的给数字兜底,不冒充)。
        switch (detail->state) {
            case lubancode::tools::AgentTaskState::Running:
                evidence.facts["state"] = "running";
                break;
            case lubancode::tools::AgentTaskState::Done:
                evidence.facts["state"] = "done";
                break;
            case lubancode::tools::AgentTaskState::Failed:
                evidence.facts["state"] = "failed";
                break;
            case lubancode::tools::AgentTaskState::Cancelled:
                evidence.facts["state"] = "cancelled";
                break;
            case lubancode::tools::AgentTaskState::BudgetExhausted:
                evidence.facts["state"] = "budget_exhausted";
                break;
        }
        evidence.content_sha256 = lubancode::hooks::Sha256Hex(detail->result);
        evidence.observed_at_ms = now_ms;
        evidence.fresh = true;
        // 事件行(goal_evidence_v1)先落再进账,与工具采证同序。
        lubancode::sessions::GoalSessionEvent line;
        line.type = "goal_evidence_v1";
        line.event = "observed";
        line.goal_id = evidence.goal_id;
        line.iteration_id = evidence.iteration_id;
        line.revision = task->revision;
        nlohmann::json payload;
        payload["evidence"] = evidence.to_json();
        line.payload = std::move(payload);
        line.timestamp_ms = now_ms;
        if (wiring.session_store != nullptr && wiring.session_store->active()) {
            (void)wiring.session_store->AppendGoalEvent(line);
        }
        coordinator.RecordEvidence(evidence);
        // 白名单顺手补(checkpoint 工具可引用它)。
        if (wiring.checkpoint_state != nullptr &&
            wiring.checkpoint_state->iteration_id == iteration_id) {
            wiring.checkpoint_state->valid_evidence_ids.push_back(evidence.id);
        }
    }
}

// 命令分派注册制(会话终章):/goal 的分派位。二级纯解析在 cli 层,业务
// 在这(状态机唯一写口是 GoalCoordinator;装配 ensure 与材料包走
// SlashDispatchContext 的回调)。
CommandFlow HandleSlashGoal(SlashDispatchContext& dispatch, const lubancode::cli::ParsedSlashCommand& parsed) {
    const lubancode::cli::ParsedGoalCommand goal = lubancode::cli::ParseGoalCommand(parsed.args);
    if (goal.action == lubancode::cli::GoalCommandAction::Invalid) {
        auto& out = lubancode::cli::TermOut();
        out << dispatch.theme->error;
        if (goal.bad_word.empty()) {
            out << "用法: /goal <objective> | status | edit <objective> | pause | resume | clear";
        } else {
            out << "子命令或参数不对: " << goal.bad_word
                << "。正文以子命令词开头时用 /goal -- <正文>";
        }
        out << dispatch.theme->reset << "\n";
        return CommandFlow::Continue;
    }
    dispatch.ensure_goal_coordinator();
    return lubancode::app::HandleGoalCommand(goal, dispatch.make_goal_wiring());
}


}  // namespace lubancode::app
