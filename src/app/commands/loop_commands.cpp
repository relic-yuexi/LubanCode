// /loop 命令处理器(loop 单第 2 期)实现。

#include "app/commands/loop_commands.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "app/commands/command_flow.hpp"
#include "cli/line_editor.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "platform/paths.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/session_runtime.hpp"
#include "tools/path_utils.hpp"

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

// prompt 预览:首行按显示宽度截 30 列(list 默认只给 preview,status 给
// 全稿)。用 TruncateUtf8ToDisplayWidth——CJK 一字两列、绝不切半个字;
// 旧实现按字节截,汉字 prompt 会被拦腰截出非法 UTF-8。
std::string PromptPreview(const std::string& prompt) {
    std::string first = prompt;
    const std::size_t cut = first.find('\n');
    if (cut != std::string::npos) {
        first.resize(cut);
    }
    if (first.empty()) {
        return "(来自 loop.md/内置维护提示)";
    }
    std::string clipped = lubancode::cli::TruncateUtf8ToDisplayWidth(first, 30);
    if (clipped != first) {
        clipped += "…";
    }
    return clipped;
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

// ---- /loop 会话接线(终端接线收尾单自大类搬出;原文随行,输出走 TerminalPort) ----

LoopPromptResolution ResolveLoopPrompt(const LoopWiring& wiring, const std::string& inline_prompt) {
    LoopPromptResolution out;
    // inline 永远压 loop.md(单子"loop.md"节)。
    if (!inline_prompt.empty()) {
        out.text = inline_prompt;
        out.source = LoopPromptSource::Inline;
        return out;
    }
    // 项目 loop.md:<project-root>/.lubancode/loop.md,须过项目 trust。
    // 未信任便跳过并提示,不执行里面的话。
    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::filesystem::path project = cwd / ".lubancode" / "loop.md";
    if (std::filesystem::exists(project)) {
        // trust 判定与项目指令同源:project_instructions 装配时已过 trust,
        // 这里用同一根线(没过 trust 的项目不给读)。
        // 读文件、限长 25k。
        std::error_code ec;
        const auto size = std::filesystem::file_size(project, ec);
        if (ec) {
            out.source = LoopPromptSource::ProjectFile;
            out.file = lubancode::tools::PathToUtf8(project);
            out.error = "loop.md 读不了";
            return out;
        }
        if (size > LoopDefaults::kPromptFileMaxBytes) {
            out.source = LoopPromptSource::ProjectFile;
            out.file = lubancode::tools::PathToUtf8(project);
            out.error = "loop.md 超过 25,000 bytes 上限,拒绝执行(不截断)";
            return out;
        }
        std::ifstream in(project, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in && content.empty()) {
            out.source = LoopPromptSource::ProjectFile;
            out.file = lubancode::tools::PathToUtf8(project);
            out.error = "loop.md 读失败";
            return out;
        }
        out.text = std::move(content);
        out.source = LoopPromptSource::ProjectFile;
        out.file = lubancode::tools::PathToUtf8(project);
        return out;
    }
    // 用户级 ~/.lubancode/loop.md。
    if (wiring.home_lubancode != nullptr && wiring.home_lubancode->has_value()) {
        const std::optional<std::string>& home = *wiring.home_lubancode;
        const std::filesystem::path user = lubancode::tools::Utf8ToPath(*home) / "loop.md";
        if (std::filesystem::exists(user)) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(user, ec);
            if (!ec && size <= LoopDefaults::kPromptFileMaxBytes) {
                std::ifstream in(user, std::ios::binary);
                std::string content((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
                if (in || !content.empty()) {
                    out.text = std::move(content);
                    out.source = LoopPromptSource::UserFile;
                    out.file = lubancode::tools::PathToUtf8(user);
                    return out;
                }
            }
            out.source = LoopPromptSource::UserFile;
            out.file = lubancode::tools::PathToUtf8(user);
            out.error = "用户级 loop.md 读失败或超限";
            return out;
        }
    }
    // 内置 maintenance prompt。
    out.text = lubancode::app::BuiltinLoopMaintenancePrompt();
    out.source = LoopPromptSource::Builtin;
    return out;
}

int HandleLoopCommand(const lubancode::cli::ParsedLoopCommand& command, const LoopWiring& wiring) {
    auto& out = lubancode::cli::TermOut();
    const lubancode::cli::Theme& theme = *wiring.theme;
    LoopScheduler& scheduler = *wiring.scheduler;
    const int flow_continue = static_cast<int>(lubancode::app::CommandFlow::Continue);
    // 无交互入口明拒(pipe/one-shot 没人回来答审批,loop 会挂死)。
    if (!wiring.interactive) {
        out << theme.error
            << "当前不是交互终端,不能建常驻 loop(无人可答审批会挂死)。"
            << theme.reset << "\n";
        return flow_continue;
    }
    if (!wiring.feature_enabled) {
        out << theme.error
            << "loop 功能未开启:配置文件里 [features] loop = true(环境变量 "
               "LUBANCODE_DISABLE_LOOP=1 是总闸)。"
            << theme.reset << "\n";
        return flow_continue;
    }
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();

    if (command.action == lubancode::cli::LoopCommandAction::Invalid) {
        out << theme.error;
        if (!command.error_hint.empty()) {
            out << command.error_hint;
        } else {
            out << "用法: /loop [间隔] [正文] | list | status <id|all> | pause <id|all> | "
                   "resume <id|all> | stop <id|all> | run <id>";
        }
        out << theme.reset << "\n";
        return flow_continue;
    }

    if (command.action == lubancode::cli::LoopCommandAction::Create) {
        // inline prompt 以 '/' 开头:拒绝(首版不许调度 slash 命令;单子
        // "Slash prompt 的边界"——/exit /clear 这类定时执行会出事)。
        if (!command.prompt.empty() && command.prompt.front() == '/') {
            out << theme.error
                << "loop 正文不能以 / 开头(定时执行 slash 命令首版不支持);请改写成自然语言。"
                << theme.reset << "\n";
            return flow_continue;
        }
        // interval:显式 token 解析;空则默认 10m。
        std::chrono::seconds interval = LoopDefaults::kDefaultInterval;
        if (!command.interval_text.empty()) {
            const auto parsed_interval = lubancode::runtime::loop::ParseLoopInterval(command.interval_text);
            if (!parsed_interval.has_value()) {
                out << theme.error << "间隔写法不对: " << command.interval_text
                    << "(只认 <正整数>m|h|d,最小 1m,最大 7d)。" << theme.reset << "\n";
                return flow_continue;
            }
            interval = *parsed_interval;
        }
        // prompt 源:inline 压 loop.md 压内置(每拍现读;这里先解一次定源,
        // 文件源每拍重读)。
        const auto resolved = ResolveLoopPrompt(wiring, command.prompt);
        if (!resolved.error.empty()) {
            out << theme.error << resolved.error << theme.reset << "\n";
            return flow_continue;
        }
        const auto outcome = lubancode::app::HandleLoopCreateCommand(
            scheduler, resolved.text, interval, lubancode::platform::CurrentDirUtf8(),
            wiring.session_store != nullptr && wiring.session_store->active()
                ? wiring.session_store->session_id()
                : std::string(),
            now_ms, resolved.source, resolved.file);
        for (const std::string& line : outcome.lines) {
            out << theme.stats << line << theme.reset << "\n";
        }
        if (wiring.flush_events) {
            wiring.flush_events();
        }
        return flow_continue;
    }

    const auto outcome = lubancode::app::HandleLoopManageCommand(scheduler, command, now_ms);
    for (const std::string& line : outcome.lines) {
        out << theme.stats << line << theme.reset << "\n";
    }
    if (wiring.flush_events) {
        wiring.flush_events();
    }
    return flow_continue;
}

void RestoreLoopFromArchive(const LoopWiring& wiring) {
    auto& out = lubancode::cli::TermOut();
    const lubancode::cli::Theme& theme = *wiring.theme;
    LoopScheduler& scheduler = *wiring.scheduler;
    if (wiring.session_store == nullptr || !wiring.session_store->active()) {
        return;
    }
    const auto bytes = lubancode::sessions::ReadSessionFileBytes(wiring.session_store->file_path());
    if (!bytes.has_value()) {
        return;
    }
    int replayed = 0;
    std::istringstream stream(*bytes);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("\"loop_task_v1\"") == std::string::npos &&
            line.find("\"loop_tick_v1\"") == std::string::npos) {
            continue;
        }
        try {
            const nlohmann::json j = nlohmann::json::parse(line);
            lubancode::runtime::loop::LoopSchedulerEvent event;
            event.family = j.value("type", std::string());
            event.event = j.value("event", std::string());
            event.task_id = j.value("task_id", std::string());
            event.tick_id = j.value("tick_id", std::string());
            event.payload = j.value("payload", nlohmann::json::object());
            event.timestamp_ms = j.value("timestamp_ms", static_cast<std::int64_t>(0));
            if (scheduler.ReplayEvent(event)) {
                ++replayed;
            }
        } catch (const std::exception&) {
            // 坏行跳过,不废整场。
        }
    }
    if (replayed == 0) {
        return;
    }
    // 恢复的 active task 默认暂停(resume 时不问一句就自动烧 token,风险
    // 大过便利;单子"恢复"节:Active 且未过期可恢复——这里保守起步,用户
    // /loop resume 显式续)。Running 中断的标 Interrupted 语义:转 Paused。
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    int resumed_active = 0;
    for (const auto& view : scheduler.Snapshot(now_ms)) {
        if (view.task.state == LoopTaskState::Active || view.task.state == LoopTaskState::Running ||
            view.task.state == LoopTaskState::Due) {
            scheduler.Pause(view.task.task_id, now_ms, "resumed_paused");
            ++resumed_active;
        }
    }
    out << theme.stats << "loop 任务已随会话恢复(" << replayed << " 条事件;"
        << resumed_active << " 只默认暂停,续跑 /loop resume <id>)。" << theme.reset << "\n";
    if (wiring.flush_events) {
        wiring.flush_events();
    }
}

void AttachLoopSnapshotToCompact(const LoopWiring& wiring, nlohmann::json& metrics_out) {
    if (wiring.scheduler == nullptr) {
        return;
    }
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    const auto views = wiring.scheduler->Snapshot(now_ms);
    nlohmann::json tasks = nlohmann::json::array();
    for (const auto& v : views) {
        if (lubancode::runtime::loop::IsLoopTerminal(v.task.state)) {
            continue;  // 只守活任务:终态的账在事件行里
        }
        nlohmann::json t;
        t["task_id"] = v.task.task_id;
        t["prompt_sha256"] = v.task.prompt_sha256;
        t["interval_ms"] = static_cast<std::int64_t>(v.task.interval.count()) * 1000;
        t["state"] = lubancode::runtime::loop::ToString(v.task.state);
        t["next_due_at_ms"] = v.task.next_due_at_ms;
        t["run_count"] = v.task.run_count;
        t["prompt_source"] = lubancode::runtime::loop::ToString(v.task.prompt_source);
        tasks.push_back(std::move(t));
    }
    if (tasks.empty()) {
        return;  // 没活 loop:不带,普通会话照旧
    }
    metrics_out["loop"] = nlohmann::json{{"active_tasks", std::move(tasks)}};
}

void FlushLoopEvents(const LoopWiring& wiring) {
    auto& out = lubancode::cli::TermOut();
    if (wiring.scheduler == nullptr) {
        return;
    }
    const auto events = wiring.scheduler->TakeEvents();
    if (events.empty()) {
        return;
    }
    // EventSink 投影(loop 单遗留:ServerEvent 面已立未灌):loop 的状态
    // 变更折 thread 层 ServerEvent 给挂了的 sink——前端凭 payload 画
    // 状态栏与任务行,不解析 slash 字符串(单子"前端凭 payload 画")。
    // 投影不拦落盘:UI 失败不拦工具的规矩在这里同款。
    EmitLoopServerEvents(wiring, events);
    if (wiring.session_store == nullptr || !wiring.session_store->active()) {
        return;  // 没建档的会话照常跑,事件只进内存
    }
    for (const auto& e : events) {
        nlohmann::json line;
        line["type"] = e.family;
        line["event"] = e.event;
        line["task_id"] = e.task_id;
        if (!e.tick_id.empty()) {
            line["tick_id"] = e.tick_id;
        }
        line["payload"] = e.payload;
        line["timestamp_ms"] = e.timestamp_ms;
        if (!wiring.session_store->AppendRawLine(line.dump())) {
            // 写盘失败熔断:失去恢复账后继续跑,风险大过便利。
            wiring.scheduler->FailStore("session append failed");
            out << wiring.theme->error
                << "loop 事件写盘失败,定时任务已熔断(已跑的拍照常收口;新拍不再排)。"
                << wiring.theme->reset << "\n";
            return;
        }
    }
}

void EmitLoopServerEvents(const LoopWiring& wiring,
                          const std::vector<lubancode::runtime::loop::LoopSchedulerEvent>& events) {
    if (wiring.session_runtime == nullptr) {
        return;
    }
    lubancode::runtime::EventSink* sink = wiring.session_runtime->sink();
    if (sink == nullptr) {
        return;  // 终端老路不接事件流,零影响
    }
    for (const auto& e : events) {
        lubancode::runtime::ServerEvent event;
        event.envelope.thread_id = wiring.session_runtime->thread_id();
        event.envelope.seq = wiring.session_runtime->ids().NextSeq();
        event.envelope.timestamp_ms = e.timestamp_ms;
        // 事件分族:task 级状态变更走 LoopTaskStateChanged,tick 级按动词
        // 分(due/started/finished)。family 里 task/tick 的分法与 scheduler
        // 的 EmitLocked 同源,这里只做协议投影。
        event.kind = lubancode::runtime::ServerEventKind::LoopTaskStateChanged;
        if (e.family == "loop_tick_v1") {
            if (e.event == "due") {
                event.kind = lubancode::runtime::ServerEventKind::LoopTickDue;
            } else if (e.event == "started") {
                event.kind = lubancode::runtime::ServerEventKind::LoopTickStarted;
            } else if (e.event == "finished") {
                event.kind = lubancode::runtime::ServerEventKind::LoopTickCompleted;
            }
        } else if (e.event == "expired") {
            event.kind = lubancode::runtime::ServerEventKind::LoopTaskExpired;
        } else if (e.event == "created") {
            event.kind = lubancode::runtime::ServerEventKind::LoopTaskCreated;
        }
        event.payload["task_id"] = e.task_id;
        if (!e.tick_id.empty()) {
            event.payload["tick_id"] = e.tick_id;
        }
        event.payload["event"] = e.event;
        event.payload["data"] = e.payload;
        sink->Emit(event);
    }
}

}  // namespace lubancode::app
