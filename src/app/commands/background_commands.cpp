// background_commands.hpp 的实现:/background 的四个子命令。读写全落
// BackgroundTaskRegistry(进程级单例,模型工具 background_output/
// stop_background 同一本账),不碰网络、不发模型请求。文案走字面量,
// 给开发者的运维视图,与清单页同一副笔法。
#include "app/commands/background_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

using lubancode::cli::TermOut;
using lubancode::cli::TermErr;

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cli/console_input.hpp"  // ReadLine:stop all 的本地确认
#include "cli/format_utils.hpp"   // FormatTurnDuration:已跑时长的人话
#include "cli/theme.hpp"
#include "platform/text_encoding.hpp"  // SanitizeExternalText:清单尾巴的外部字节
#include "tools/background_tasks.hpp"

namespace lubancode::app {

namespace {

// ---------------- 小工具 ----------------

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::vector<std::string> SplitWords(const std::string& args) {
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < args.size()) {
        while (i < args.size() &&
               std::isspace(static_cast<unsigned char>(args[i])) != 0) {
            ++i;
        }
        const std::size_t begin = i;
        while (i < args.size() && std::isspace(static_cast<unsigned char>(args[i])) == 0) {
            ++i;
        }
        if (i > begin) {
            words.push_back(args.substr(begin, i - begin));
        }
    }
    return words;
}

bool ParseInteger(const std::string& text, long long* out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = value;
    return true;
}

// 启动时间的本地时区人话(YYYY-MM-DD HH:MM:SS)。拿不到如实说,不编。
std::string FormatStartTime(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm parts{};
#ifdef _WIN32
    if (localtime_s(&parts, &tt) != 0) {
        return "(时间不明)";
    }
#else
    if (localtime_r(&tt, &parts) == nullptr) {
        return "(时间不明)";
    }
#endif
    char buf[32];
    if (std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", parts.tm_year + 1900,
                      parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min,
                      parts.tm_sec) < 0) {
        return "(时间不明)";
    }
    return buf;
}

// 已跑时长:运行中 = now - start;终态 = finish - start(拿不到按 0)。
// 输出走 FormatTurnDuration 同一把尺(Working 条与 turn footer 共用)。
std::string FormatElapsed(const lubancode::tools::BackgroundTaskInfo& t) {
    const auto span_end = t.status == lubancode::tools::BackgroundTaskStatus::Running ||
                                  t.status == lubancode::tools::BackgroundTaskStatus::Stopping
                              ? std::chrono::system_clock::now()
                              : t.finish_time;
    const long long ms =
        span_end > t.start_time
            ? std::chrono::duration_cast<std::chrono::milliseconds>(span_end - t.start_time).count()
            : 0;
    return lubancode::cli::FormatTurnDuration(ms);
}

// 状态标签走台账那份 BackgroundTaskStatusLabel(List/detail/通知共用一份,
// 单子叮嘱别再两处各写一套;模型工具 background_output 的返回文本同源,
// 两条路字面一致)。
const char* StatusLabel(lubancode::tools::BackgroundTaskStatus s) {
    return lubancode::tools::BackgroundTaskStatusLabel(s);
}

bool IsTerminal(lubancode::tools::BackgroundTaskStatus s) {
    return s != lubancode::tools::BackgroundTaskStatus::Running &&
           s != lubancode::tools::BackgroundTaskStatus::Stopping &&
           s != lubancode::tools::BackgroundTaskStatus::StopFailed;
}

std::string ExitText(const lubancode::tools::BackgroundExit& exit) {
    if (!exit.exit_code.has_value()) {
        return "unknown";
    }
    std::string text = std::to_string(*exit.exit_code);
    if (exit.signal.has_value()) {
        text += ", signal " + std::to_string(*exit.signal);
    }
    return text;
}

// 用户照着清单抄编号,常带一枚 #(清单行是 [#2]):show/logs/stop 收目标
// 前剥掉,别叫一枚井号把人挡在门外。
std::string NormalizeTaskId(const std::string& target) {
    if (!target.empty() && target[0] == '#') {
        return target.substr(1);
    }
    return target;
}

void PrintBackgroundUsage() {
    TermOut() << "用法: /background                       列后台任务清单\n"
                 "      /background show <id>             任务详情(状态/PID/命令/cwd/启动时间/已跑/退出码/日志路径)\n"
                 "      /background logs <id> [--tail N]  查看日志尾部(默认 100 行;N<=0 查全文,上限 64KB)\n"
                 "      /background stop <id>             停一只(整棵进程树一起收)\n"
                 "      /background stop all              停全部在跑的(先列清单再确认;已终态不重复杀)\n"
                 "全部本地执行,不经模型;与模型工具 background_output/stop_background 同一本账。\n"
                 "logs 是查看日志,不是进入终端(可附着 PTY 是另一码事,本期不做)。\n";
    TermOut().flush();
}

}  // namespace

// ---------------- 纯解析(单测钉) ----------------

ParsedBackgroundCommand ParseBackgroundCommand(const std::string& args) {
    ParsedBackgroundCommand parsed;
    const std::vector<std::string> words = SplitWords(args);
    if (words.empty()) {
        parsed.action = BackgroundCommandAction::List;  // 裸 /background = 清单
        return parsed;
    }
    const std::string verb = ToLowerAscii(words[0]);

    if (verb == "list") {
        if (words.size() > 1) {
            parsed.action = BackgroundCommandAction::Invalid;
            parsed.bad_word = words[1];
            return parsed;
        }
        parsed.action = BackgroundCommandAction::List;
        return parsed;
    }

    if (verb == "show" || verb == "logs" || verb == "stop") {
        // 目标缺了 / 目标长得像旗子:Invalid,提示补 id。
        if (words.size() < 2 || (!words[1].empty() && words[1][0] == '-')) {
            parsed.action = BackgroundCommandAction::Invalid;
            parsed.bad_word = verb;
            return parsed;
        }
        if (verb == "stop") {
            // stop all:大小写不敏感;后头不许再跟东西。
            if (ToLowerAscii(words[1]) == "all") {
                if (words.size() > 2) {
                    parsed.action = BackgroundCommandAction::Invalid;
                    parsed.bad_word = words[2];
                    return parsed;
                }
                parsed.action = BackgroundCommandAction::Stop;
                parsed.stop_all = true;
                return parsed;
            }
            if (words.size() > 2) {
                parsed.action = BackgroundCommandAction::Invalid;
                parsed.bad_word = words[2];
                return parsed;
            }
            parsed.action = BackgroundCommandAction::Stop;
            parsed.target = words[1];
            return parsed;
        }
        // show / logs:目标收定,logs 另认 --tail N / --tail=N。
        parsed.target = words[1];
        if (verb == "show") {
            if (words.size() > 2) {
                parsed.action = BackgroundCommandAction::Invalid;
                parsed.bad_word = words[2];
                return parsed;
            }
            parsed.action = BackgroundCommandAction::Show;
            return parsed;
        }
        parsed.action = BackgroundCommandAction::Logs;
        std::size_t i = 2;
        while (i < words.size()) {
            if (words[i] == "--tail") {
                if (i + 1 >= words.size()) {
                    parsed.action = BackgroundCommandAction::Invalid;
                    parsed.bad_word = "--tail";
                    return parsed;
                }
                long long value = 0;
                if (!ParseInteger(words[i + 1], &value)) {
                    parsed.action = BackgroundCommandAction::Invalid;
                    parsed.bad_word = "--tail " + words[i + 1];
                    return parsed;
                }
                // 与 background_output 同语义:N<=0 查全文(上限 64KB);
                // 超大正值夹紧(64KB 里塞不下百万行,防窄化走样)。
                parsed.tail_lines = value < 0          ? 0
                                    : value > 1000000 ? 1000000
                                                      : static_cast<int>(value);
                i += 2;
                continue;
            }
            if (words[i].rfind("--tail=", 0) == 0) {
                long long value = 0;
                if (!ParseInteger(words[i].substr(7), &value)) {
                    parsed.action = BackgroundCommandAction::Invalid;
                    parsed.bad_word = words[i];
                    return parsed;
                }
                parsed.tail_lines = value < 0          ? 0
                                    : value > 1000000 ? 1000000
                                                      : static_cast<int>(value);
                i += 1;
                continue;
            }
            parsed.action = BackgroundCommandAction::Invalid;
            parsed.bad_word = words[i];
            return parsed;
        }
        return parsed;
    }

    parsed.action = BackgroundCommandAction::Invalid;
    parsed.bad_word = words[0];
    return parsed;
}

// ---------------- 状态行段(纯函数,单测钉) ----------------

std::string BuildBackgroundStatusSegment(const std::vector<lubancode::tools::BackgroundTaskInfo>& tasks) {
    if (tasks.empty()) {
        return std::string();  // 没任务:段收起
    }
    int running = 0;
    int finished = 0;
    for (const auto& t : tasks) {
        if (t.status == lubancode::tools::BackgroundTaskStatus::Running ||
            t.status == lubancode::tools::BackgroundTaskStatus::Stopping) {
            ++running;  // 停止中也还没停完,算"在跑"这边
        } else {
            ++finished;  // 完成/失败/已停止/停止失败,都算"已收场"
        }
    }
    std::string text = "后台 ";
    if (running > 0) {
        text += std::to_string(running) + " 运行";
        if (finished > 0) {
            text += " / " + std::to_string(finished) + " 完成";
        }
        return text;
    }
    return text + std::to_string(finished) + " 完成";
}

// ---------------- 执行 ----------------

namespace {

// 清单页(0.30.x 起的老账面原样搬来:每项带三行尾巴、启动与时长;日志被
// 删/非法 UTF-8/进程已死都只是那一项少几行,菜单不带崩)。
void RunBackgroundList(const lubancode::cli::Theme& theme) {
    auto& registry = lubancode::tools::BackgroundTaskRegistry::Instance();
    const auto tasks = registry.List();
    if (tasks.empty()) {
        TermOut() << "当前没有后台任务。" << "\n";
        TermOut().flush();
        return;
    }
    TermOut() << "后台任务共 " << tasks.size() << " 个:" << "\n" << "\n";
    for (const auto& t : tasks) {
        TermOut() << theme.tool_line << "[#" << t.task_id << "] " << StatusLabel(t.status);
        if (IsTerminal(t.status)) {
            TermOut() << " (exit " << ExitText(t.exit) << ")";
        }
        TermOut() << theme.reset << "  PID=" << t.pid << "  已跑 " << FormatElapsed(t) << "\n"
                  << theme.stats << "  命令: " << t.command << theme.reset << "\n"
                  << theme.stats << "  日志: " << t.log_path << theme.reset << "\n";
        if (const std::string tail = registry.ReadOutput(t.task_id, 24); !tail.empty()) {
            std::string safe = lubancode::platform::SanitizeExternalText(tail);
            std::vector<std::string> non_empty;
            std::size_t line_start = 0;
            while (line_start <= safe.size()) {
                const std::size_t line_end = safe.find('\n', line_start);
                std::string line = safe.substr(
                    line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                    line.pop_back();
                }
                if (!line.empty()) {
                    non_empty.push_back(std::move(line));
                }
                if (line_end == std::string::npos) {
                    break;
                }
                line_start = line_end + 1;
            }
            if (non_empty.size() > 3) {
                non_empty.erase(non_empty.begin(), non_empty.end() - 3);  // 只要最近三行非空
            }
            for (const std::string& line : non_empty) {
                TermOut() << theme.stats << "  ⎿ " << line << theme.reset << "\n";
            }
        }
        TermOut() << "\n";
    }
    TermOut().flush();
}

// 详情页:单子点名的字段一个不少;停止失败的缘故也照摆。
void RunBackgroundShow(const lubancode::cli::Theme& theme, const std::string& target) {
    const auto info = lubancode::tools::BackgroundTaskRegistry::Instance().Get(target);
    if (!info.has_value()) {
        TermOut() << "找不到 #" << target << " 的后台任务。先 /background 看清单,拿编号来查。\n";
        TermOut().flush();
        return;
    }
    TermOut() << "后台任务 #" << info->task_id << "\n";
    TermOut() << "  状态:  " << StatusLabel(info->status) << "\n";
    TermOut() << "  PID:   " << info->pid << "\n";
    TermOut() << "  命令:  " << info->command << "\n";
    TermOut() << "  shell: " << info->shell << "\n";
    TermOut() << "  cwd:   " << (info->cwd.empty() ? "(未记录)" : info->cwd) << "\n";
    TermOut() << "  启动:  " << FormatStartTime(info->start_time) << "\n";
    TermOut() << "  已跑:  " << FormatElapsed(*info) << "\n";
    if (IsTerminal(info->status)) {
        TermOut() << "  退出码: " << ExitText(info->exit) << "\n";
    } else {
        TermOut() << "  退出码: (还没退出)\n";
    }
    if (info->status == lubancode::tools::BackgroundTaskStatus::StopFailed && !info->stop_error.empty()) {
        TermOut() << theme.error << "  停止失败原因: " << info->stop_error << theme.reset << "\n";
    }
    TermOut() << "  日志:  " << info->log_path << "\n";
    TermOut() << "  查看:  /background logs " << info->task_id << "(查看日志,只读)\n";
    TermOut().flush();
}

// 日志页:空/删/坏/超长/已退出各有各的如实话。本期只做查看,不写"进入终端"。
void RunBackgroundLogs(const lubancode::cli::Theme& theme, const std::string& target, int tail_lines) {
    auto& registry = lubancode::tools::BackgroundTaskRegistry::Instance();
    const auto info = registry.Get(target);
    if (!info.has_value()) {
        TermOut() << "找不到 #" << target << " 的后台任务。先 /background 看清单,拿编号来查。\n";
        TermOut().flush();
        return;
    }
    const auto log = registry.ReadLogDetail(target, tail_lines);
    TermOut() << "后台任务 #" << info->task_id << " 日志(查看日志:只读,不动进程)\n";
    TermOut() << theme.stats << "  状态: " << StatusLabel(info->status) << "  日志: " << info->log_path
              << theme.reset << "\n";
    switch (log.kind) {
        case lubancode::tools::BackgroundLogRead::Kind::TaskNotFound:
            // Get 已认得,这里不该到;到就说实话。
            TermOut() << "  日志读不了:任务账在,台账里没有这份日志的路径。\n";
            break;
        case lubancode::tools::BackgroundLogRead::Kind::FileMissing:
            TermOut() << "  日志文件已不存在(可能已被清理):" << info->log_path << "\n";
            break;
        case lubancode::tools::BackgroundLogRead::Kind::Empty:
            TermOut() << "  日志暂时没有内容(进程可能还没开始写):" << info->log_path << "\n";
            break;
        case lubancode::tools::BackgroundLogRead::Kind::ReadFailed:
            TermOut() << theme.error << "  日志文件打不开(权限/占用):" << info->log_path << theme.reset
                      << "\n";
            break;
        case lubancode::tools::BackgroundLogRead::Kind::Ok: {
            if (log.head_omitted) {
                TermOut() << theme.stats << "  [日志超过单次读档上限(64KB),前部已省略,这里是最末一段]"
                          << theme.reset << "\n";
            }
            if (log.sanitized) {
                TermOut() << theme.stats << "  [日志里混有非法 UTF-8 字节,已按替换符清洗显示]" << theme.reset
                          << "\n";
            }
            if (IsTerminal(info->status)) {
                TermOut() << theme.stats << "  [任务已退出(" << StatusLabel(info->status)
                          << "),以下为最终输出]" << theme.reset << "\n";
            }
            const std::string head_line = tail_lines > 0
                                              ? "── 末尾 " + std::to_string(tail_lines) + " 行 ──"
                                              : "── 全文(上限 64KB)──";
            TermOut() << theme.stats << "  " << head_line << theme.reset << "\n";
            TermOut() << log.text;
            if (!log.text.empty() && log.text.back() != '\n') {
                TermOut() << "\n";
            }
            TermOut() << theme.stats << "  ── 完(共读 " << log.text.size() << " 字节,文件 "
                      << log.file_size << " 字节)──" << theme.reset << "\n";
            break;
        }
    }
    TermOut().flush();
}

// 停一只:先报"停止信号已发"(此刻台账已进 Stopping),Stop 返回时树已死透
// 或收不动——再按终态如实回话。Stop() 是同步的,内部走
// Running→Stopping→Stopped/StopFailed 三段;这里前后各取一次台账,
// 用户在 /background 里看到的就是这三段。
void ReportStopOutcome(const lubancode::cli::Theme& theme, const lubancode::tools::BackgroundTaskInfo& after) {
    switch (after.status) {
        case lubancode::tools::BackgroundTaskStatus::Stopped:
            TermOut() << theme.tool_line << "后台任务 #" << after.task_id
                      << " 已停止:整棵进程树已收净(exit " << ExitText(after.exit) << ")" << theme.reset
                      << "\n";
            break;
        case lubancode::tools::BackgroundTaskStatus::StopFailed:
            TermOut() << theme.error << "后台任务 #" << after.task_id << " 停止失败:进程可能还活着。"
                      << (after.stop_error.empty() ? std::string("原因未知") : after.stop_error)
                      << theme.reset << "\n"
                      << "  可重试 /background stop " << after.task_id << ",或照详情里的 PID 手动收。\n";
            break;
        case lubancode::tools::BackgroundTaskStatus::Completed:
        case lubancode::tools::BackgroundTaskStatus::Failed:
            TermOut() << theme.stats << "后台任务 #" << after.task_id << " 在停止前已自己退出("
                      << StatusLabel(after.status) << ",exit " << ExitText(after.exit) << ")" << theme.reset
                      << "\n";
            break;
        case lubancode::tools::BackgroundTaskStatus::Running:
        case lubancode::tools::BackgroundTaskStatus::Stopping:
            // Stop 返回了还活着:账没落终态,照实说,不装成功。
            TermOut() << theme.error << "后台任务 #" << after.task_id << " 还在"
                      << (after.status == lubancode::tools::BackgroundTaskStatus::Running ? "运行" : "停止中")
                      << ",停止没有收口。稍后再查 /background show " << after.task_id << theme.reset << "\n";
            break;
    }
}

void RunBackgroundStop(const lubancode::cli::Theme& theme, const std::string& target) {
    auto& registry = lubancode::tools::BackgroundTaskRegistry::Instance();
    const auto before = registry.Get(target);
    if (!before.has_value()) {
        TermOut() << "找不到 #" << target << " 的后台任务。先 /background 看清单,拿编号来停。\n";
        TermOut().flush();
        return;
    }
    if (IsTerminal(before->status)) {
        // 已终态:不重复杀(老账里 StopFailed 也算没停成,可再试)。
        TermOut() << theme.stats << "后台任务 #" << before->task_id << " 已是终态(" << StatusLabel(before->status)
                  << "),不重复杀。" << theme.reset << "\n";
        TermOut().flush();
        return;
    }
    // StopFailed 不算终态,允许再停——上面 IsTerminal 已放行。
    TermOut() << "正在停止 #" << before->task_id << "(停止信号已发,状态进「停止中」,等整棵进程树退净,最多 2 秒宽限)...\n";
    TermOut().flush();
    registry.Stop(target);  // 同步:返回时树已死透或已进 StopFailed
    const auto after = registry.Get(target);
    if (after.has_value()) {
        ReportStopOutcome(theme, *after);
    }
    TermOut().flush();
}

void RunBackgroundStopAll(const lubancode::cli::Theme& theme) {
    auto& registry = lubancode::tools::BackgroundTaskRegistry::Instance();
    const auto tasks = registry.List();
    std::vector<lubancode::tools::BackgroundTaskInfo> live;
    std::size_t terminal = 0;
    for (const auto& t : tasks) {
        if (IsTerminal(t.status)) {
            ++terminal;
        } else {
            live.push_back(t);
        }
    }
    if (live.empty()) {
        TermOut() << "没有在跑的后台任务";
        if (terminal > 0) {
            TermOut() << "(" << terminal << " 个已终态,不重复杀)";
        }
        TermOut() << "。\n";
        TermOut().flush();
        return;
    }
    TermOut() << "将停止以下 " << live.size() << " 个后台任务:\n";
    for (const auto& t : live) {
        TermOut() << theme.stats << "  [#" << t.task_id << "] " << StatusLabel(t.status) << "  "
                  << t.command << theme.reset << "\n";
    }
    if (terminal > 0) {
        TermOut() << theme.stats << "另有 " << terminal << " 个已终态任务,不重复杀。" << theme.reset << "\n";
    }
    const auto answer = lubancode::cli::ReadLine(
        theme.confirm + "确认停止以上 " + std::to_string(live.size()) + " 个任务? [y/N] " + theme.reset,
        theme, /*esc_rejects=*/true);
    if (!answer.has_value() || !(*answer == "y" || *answer == "Y")) {
        TermOut() << "已取消,一只都没停。\n";
        TermOut().flush();
        return;
    }
    for (const auto& t : live) {
        TermOut() << "正在停止 #" << t.task_id << "...\n";
        TermOut().flush();
        registry.Stop(t.task_id);
        const auto after = registry.Get(t.task_id);
        if (after.has_value()) {
            ReportStopOutcome(theme, *after);
        }
    }
    TermOut().flush();
}

}  // namespace

CommandFlow HandleSlashBackground(SlashDispatchContext& ctx,
                                  const lubancode::cli::ParsedSlashCommand& parsed) {
    const lubancode::cli::Theme& theme = *ctx.theme;
    const ParsedBackgroundCommand command = ParseBackgroundCommand(parsed.args);
    const std::string target = NormalizeTaskId(command.target);
    switch (command.action) {
        case BackgroundCommandAction::List:
            RunBackgroundList(theme);
            return CommandFlow::Continue;
        case BackgroundCommandAction::Show:
            RunBackgroundShow(theme, target);
            return CommandFlow::Continue;
        case BackgroundCommandAction::Logs:
            RunBackgroundLogs(theme, target, command.tail_lines);
            return CommandFlow::Continue;
        case BackgroundCommandAction::Stop:
            if (command.stop_all) {
                RunBackgroundStopAll(theme);
            } else {
                RunBackgroundStop(theme, target);
            }
            return CommandFlow::Continue;
        case BackgroundCommandAction::Invalid:
            TermOut() << "认不得 \"" << command.bad_word << "\"。\n";
            PrintBackgroundUsage();
            return CommandFlow::Continue;
    }
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
