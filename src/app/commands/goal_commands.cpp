// /goal 命令处理器实现(纯排版与 gate;状态机调用在 interactive_session)。

#include "app/commands/goal_commands.hpp"

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

}  // namespace lubancode::app
