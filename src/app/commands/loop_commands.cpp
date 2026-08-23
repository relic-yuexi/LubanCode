// /loop 命令处理器(loop 单第 2 期)实现。

#include "app/commands/loop_commands.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "platform/paths.hpp"

namespace lubancode::app {

using lubancode::cli::LoopCommandAction;
using lubancode::cli::ParsedLoopCommand;
using lubancode::runtime::loop::LoopDefaults;
using lubancode::runtime::loop::LoopPromptSource;
using lubancode::runtime::loop::LoopScheduler;
using lubancode::runtime::loop::LoopTaskState;
using lubancode::runtime::loop::ToString;

bool LoopDisabledByEnv() {
    const auto v = lubancode::platform::GetEnvVar("LUBANCODE_DISABLE_LOOP");
    return v.has_value() && (*v == "1" || *v == "true" || *v == "on");
}

std::string BuiltinLoopMaintenancePrompt() {
    // 单子"内置 maintenance prompt"节:只准在已授权的工作范围内做三件
    // 事,不得凭空开新功能、不得自行 push/发布/删文件;它自己不是授权。
    return "这是会话维护循环的定时一拍。在当前已授权的工作范围内:\n"
           "1. 若本会话还有尚未收口的工作,继续推进它;\n"
           "2. 否则检查当前分支已有的 PR/CI/review 问题,有则汇报;\n"
           "3. 都没有就做一次只读健康检查,报告\"无事\"。\n"
           "不要开新功能,不要 push、发布或删除文件。不可逆动作仍走正常权限流程。";
}

std::string FormatLoopInterval(std::chrono::seconds interval) {
    const long long secs = interval.count();
    if (secs % 86400 == 0) {
        const long long d = secs / 86400;
        return std::to_string(d) + (d == 1 ? " 天" : " 天");
    }
    if (secs % 3600 == 0) {
        const long long h = secs / 3600;
        return std::to_string(h) + (h == 1 ? " 小时" : " 小时");
    }
    const long long m = secs / 60;
    return std::to_string(m) + (m == 1 ? " 分钟" : " 分钟");
}

std::string FormatLoopDelta(std::int64_t now_ms, std::int64_t at_ms) {
    const std::int64_t delta = at_ms - now_ms;
    if (delta <= 0) {
        return "已到点";
    }
    const long long secs = delta / 1000;
    if (secs < 60) {
        return std::to_string(secs) + " 秒后";
    }
    if (secs < 3600) {
        return std::to_string(secs / 60) + " 分钟后";
    }
    if (secs < 86400) {
        return std::to_string(secs / 3600) + " 小时后";
    }
    return std::to_string(secs / 86400) + " 天后";
}

namespace {

std::string StateLabel(LoopTaskState state) {
    switch (state) {
        case LoopTaskState::Active: return "运行中";
        case LoopTaskState::Paused: return "已暂停";
        case LoopTaskState::Due: return "到点待取";
        case LoopTaskState::Running: return "拍执行中";
        case LoopTaskState::WaitingPermission: return "等审批";
        case LoopTaskState::BackingOff: return "退避中";
        case LoopTaskState::Completed: return "已完成";
        case LoopTaskState::Cancelled: return "已停止";
        case LoopTaskState::Expired: return "已过期";
        case LoopTaskState::Broken: return "已损坏";
    }
    return ToString(state);
}

// prompt 预览:首行截 30 字(list 默认只给 preview,status 给全稿)。
std::string PromptPreview(const std::string& prompt) {
    std::string first = prompt;
    const std::size_t cut = first.find('\n');
    if (cut != std::string::npos) {
        first.resize(cut);
    }
    if (first.size() > 60) {
        first.resize(57);
        first += "...";
    }
    return first.empty() ? "(来自 loop.md/内置维护提示)" : first;
}

}  // namespace

LoopCommandOutcome HandleLoopManageCommand(LoopScheduler& scheduler, const ParsedLoopCommand& command,
                                           std::int64_t now_ms) {
    LoopCommandOutcome out;
    if (command.action == LoopCommandAction::List) {
        const auto views = scheduler.Snapshot(now_ms);
        if (views.empty()) {
            out.ok = true;
            out.lines.push_back("没有 loop 任务。/loop [间隔] [正文] 建一只。");
            return out;
        }
        out.ok = true;
        out.lines.push_back("loop 任务(" + std::to_string(views.size()) + " 只):");
        for (const auto& v : views) {
            std::string line = "  " + v.task.task_id + "  [" + StateLabel(v.task.state) + "] " +
                               FormatLoopInterval(v.task.interval) + " · 下一拍 " +
                               FormatLoopDelta(now_ms, v.task.next_due_at_ms);
            if (v.delayed) {
                line += " · 已延迟";
            }
            line += " · " + PromptPreview(v.task.prompt);
            out.lines.push_back(line);
        }
        return out;
    }
    if (command.action == LoopCommandAction::Status) {
        if (command.task_ref == "all") {
            return HandleLoopManageCommand(
                scheduler, [] {
                    ParsedLoopCommand list;
                    list.action = LoopCommandAction::List;
                    return list;
                }(),
                now_ms);
        }
        const std::string id = scheduler.ResolveTaskId(command.task_ref);
        const auto view = scheduler.Find(id, now_ms);
        if (!view.has_value()) {
            out.lines.push_back("任务不存在: " + command.task_ref);
            return out;
        }
        out.ok = true;
        const auto& t = view->task;
        out.lines.push_back(id + "  [" + StateLabel(t.state) + "]");
        out.lines.push_back("  间隔: " + FormatLoopInterval(t.interval) +
                            " · 已跑 " + std::to_string(t.run_count) + " 拍 · 合并掉 " +
                            std::to_string(t.skipped_count) + " 拍");
        out.lines.push_back("  下一拍: " + FormatLoopDelta(now_ms, t.next_due_at_ms) +
                            " · 过期: " + FormatLoopDelta(now_ms, t.expires_at_ms));
        if (t.consecutive_failures > 0) {
            out.lines.push_back("  连败 " + std::to_string(t.consecutive_failures) + " 拍(到 " +
                                std::to_string(LoopDefaults::kProviderFailPauseThreshold) + " 拍自动暂停)");
        }
        out.lines.push_back("  prompt(" + ToString(t.prompt_source) + "): " +
                            (t.prompt.empty() ? "(来自 loop.md/内置维护提示)" : t.prompt));
        return out;
    }
    if (command.action == LoopCommandAction::Pause) {
        const auto r = scheduler.Pause(command.task_ref == "all" ? "all"
                                           : scheduler.ResolveTaskId(command.task_ref),
                                       now_ms, "user");
        out.ok = r.ok;
        if (!r.ok) {
            out.lines.push_back("暂停失败: " + r.error_message);
            return out;
        }
        if (command.task_ref == "all") {
            out.lines.push_back("已暂停 " + std::to_string(r.payload.value("paused", 0)) + " 只任务。");
        } else {
            out.lines.push_back("已暂停 " + r.payload.value("task_id", std::string()) +
                                ";定义保留,resume 从现在起再排。");
        }
        return out;
    }
    if (command.action == LoopCommandAction::Resume) {
        const auto r = scheduler.Resume(command.task_ref == "all" ? "all"
                                            : scheduler.ResolveTaskId(command.task_ref),
                                        now_ms);
        out.ok = r.ok;
        if (!r.ok) {
            out.lines.push_back("续跑失败: " + r.error_message);
            return out;
        }
        if (command.task_ref == "all") {
            out.lines.push_back("已续跑 " + std::to_string(r.payload.value("resumed", 0)) + " 只任务。");
        } else {
            out.lines.push_back("已续跑 " + r.payload.value("task_id", std::string()) + ",下一拍 " +
                                FormatLoopDelta(now_ms, static_cast<std::int64_t>(
                                                            r.payload.value("next_due_at_ms", 0))) +
                                "。");
        }
        return out;
    }
    if (command.action == LoopCommandAction::Stop) {
        const auto r = scheduler.Stop(command.task_ref == "all" ? "all"
                                          : scheduler.ResolveTaskId(command.task_ref),
                                      now_ms, "user");
        out.ok = r.ok;
        if (!r.ok) {
            out.lines.push_back("停止失败: " + r.error_message);
            return out;
        }
        if (command.task_ref == "all") {
            out.lines.push_back("已停止 " + std::to_string(r.payload.value("stopped", 0)) + " 只任务。");
        } else {
            out.lines.push_back("已停止 " + r.payload.value("task_id", std::string()) + ";账保留在会话存档。");
        }
        return out;
    }
    if (command.action == LoopCommandAction::Run) {
        const auto r = scheduler.RunNow(scheduler.ResolveTaskId(command.task_ref), now_ms);
        out.ok = r.ok;
        if (!r.ok) {
            out.lines.push_back("补拍失败: " + r.error_message);
            return out;
        }
        out.lines.push_back("已为 " + r.payload.value("task_id", std::string()) +
                            " 排一次立即补拍(不改原间隔)。");
        return out;
    }
    out.lines.push_back("认不得的 /loop 动作。");
    return out;
}

LoopCommandOutcome HandleLoopCreateCommand(LoopScheduler& scheduler, const std::string& prompt,
                                           std::chrono::seconds interval, const std::string& cwd_identity,
                                           const std::string& session_id, std::int64_t now_ms,
                                           LoopPromptSource source, const std::string& prompt_file) {
    LoopCommandOutcome out;
    const auto r = scheduler.Create(prompt, interval, now_ms, cwd_identity, session_id, source,
                                    prompt_file);
    out.ok = r.ok;
    if (!r.ok) {
        out.lines.push_back("建任务失败: " + r.error_message);
        return out;
    }
    out.lines.push_back("loop 任务已建: " + r.payload.value("task_id", std::string()) + "(" +
                        FormatLoopInterval(interval) + ",下一拍 " +
                        FormatLoopDelta(now_ms, static_cast<std::int64_t>(
                                                    r.payload.value("next_due_at_ms", 0))) +
                        ")。");
    out.lines.push_back("查看 /loop list;暂停 /loop pause <id>;停止 /loop stop <id>。");
    return out;
}

}  // namespace lubancode::app
