// 见 hpp 合同注释(QQ 接入单 Q7)。
#include "channel/channel_commands.hpp"

#include <cstring>
#include <sstream>
#include <utility>

#include "channel/qq/qq_proto.hpp"  // ParseLooseInt64(数值字段两态容忍)

namespace lubancode::channel {

namespace {

// 去首尾空白(空格/制表/CR/LF;不做 Unicode 级 trim——菜单填入的文本由
// 平台客户端产生,边界空白是 ASCII 的)。
std::string TrimAscii(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end) {
        const char c = text[begin];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++begin;
        } else {
            break;
        }
    }
    while (end > begin) {
        const char c = text[end - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            --end;
        } else {
            break;
        }
    }
    return text.substr(begin, end - begin);
}

// Howard Hinnant civil_from_days:天数 → 年月日(UTC,无时区)。
void CivilFromDays(std::int64_t days, int* y, unsigned* m, unsigned* d) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y_ = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d_ = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m_ = mp < 10 ? mp + 3 : mp - 9;
    *y = static_cast<int>(m_ <= 2 ? y_ + 1 : y_);
    *m = static_cast<unsigned>(m_);
    *d = static_cast<unsigned>(d_);
}

std::string TwoDigits(std::int64_t value) {
    std::string text = std::to_string(value);
    if (text.size() < 2) {
        text.insert(text.begin(), '0');
    }
    return text;
}

std::string JobStateText(const std::string& state) {
    if (state == "active") return "进行中";
    if (state == "paused") return "已暂停";
    if (state == "cancelled") return "已取消";
    if (state == "completed") return "已完成";
    return state;
}

std::string ScheduleKindText(const std::string& kind) {
    if (kind == "once") return "单次";
    if (kind == "cron") return "周期";
    if (kind == "interval") return "间隔";
    return kind;
}

}  // namespace

std::optional<ChannelCommandBindingUserConfig> MatchChannelCommand(
    const std::vector<ChannelCommandBindingUserConfig>& commands, const std::string& text) {
    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    // 宿主命令保留，不能被 prompt 别名盖掉。参数只交给宿主解析。
    const auto split = trimmed.find_first_of(" \t\r\n");
    const auto name = trimmed.substr(0, split);
    const auto args = split == std::string::npos ? std::string() : TrimAscii(trimmed.substr(split));
    ChannelCommandBindingUserConfig builtin;
    builtin.match = name;
    builtin.prompt = args;
    if (name == "/help" || name == "/帮助") builtin.action = "help";
    else if (name == "/session" || name == "/sessions" || name == "/会话") builtin.action = "session";
    else if (name == "/new" || name == "/clear" || name == "/新会话") {
        builtin.action = "session";
        builtin.prompt = "new";
    } else if (name == "/files" || name == "/文件说明") builtin.action = "file_help";
    else if (name == "/reminders" || name == "/提醒") builtin.action = "list_reminders";
    else if (name == "/status" || name == "/tools" || name == "/skills") builtin.action = "capabilities";
    else if (name == "/menu" || name == "/菜单") builtin.action = "menu_help";
    if (!builtin.action.empty()) return builtin;
    for (const auto& binding : commands) {
        if (binding.match == trimmed) {
            return binding;
        }
    }
    // 未知 slash 不消耗模型调用，也不让模型猜宿主功能。
    if (trimmed.front() == '/') {
        builtin.action = "unknown";
        return builtin;
    }
    return std::nullopt;
}

std::string MakeChannelHelpText(const std::vector<ChannelCommandBindingUserConfig>& commands) {
    std::ostringstream out;
    out << "QQ 助手命令:\n"
           "/help — 帮助\n/session — 会话列表\n/session current — 当前会话\n"
           "/new 或 /clear — 开新上下文，保留历史与提醒\n"
           "/session switch <编号> — 切换到列表中的会话\n"
           "/status — 当前工具与配置状态\n/skills — 技能状态\n"
           "/files — 文件与图片说明\n/reminders — 我的提醒\n";
    out << "/menu — 菜单启用说明\n";
    for (const auto& binding : commands) {
        if (binding.action == "prompt") {
            // 预设输入:列 match 与 prompt 的引导句(prompt 是配置里明写的
            // 用户可见预设,不算私有信息)。
            std::string summary = binding.prompt;
            // 摘要只取第一行,压进 40 平台字符。
            const std::size_t newline = summary.find('\n');
            if (newline != std::string::npos) {
                summary.resize(newline);
            }
            if (CountPlatformChars(summary) > 40) {
                // 平台字符截断(不拆 UTF-8 多字节尾)。
                std::size_t platform = 0;
                std::size_t cut = 0;
                for (std::size_t i = 0; i < summary.size();) {
                    const auto raw = static_cast<unsigned char>(summary[i]);
                    const std::size_t width = (raw & 0x80) == 0 ? 1 : 2;
                    if (platform + width > 40) {
                        break;
                    }
                    platform += width;
                    i += (raw & 0x80) == 0 ? 1 : (raw & 0xE0) == 0xC0 ? 2 : (raw & 0xF0) == 0xE0 ? 3 : 4;
                    cut = i;
                }
                summary.resize(cut);
                summary += "…";
            }
            out << "- " << binding.match << ":" << summary << "\n";
        } else if (binding.action == "help") {
            out << "- " << binding.match << ":这份帮助\n";
        } else if (binding.action == "file_help") {
            out << "- " << binding.match << ":怎么发文件给我\n";
        } else if (binding.action == "list_reminders") {
            out << "- " << binding.match << ":查看我的定时任务\n";
        }
    }
    out << "直接发消息也行,不一定走菜单。";
    return out.str();
}

std::string MakeChannelFileHelpText() {
    return "在 QQ 聊天的 + 菜单中发文件或图片。每条最多 4 件，每件最多 20 MiB。"
           "文本附上预览；PNG/JPEG/GIF/WebP 送入模型视觉输入，仍需模型支持图片。"
           "PDF、Office 等原件会存档，须有解析工具或技能才能读正文，不会假称已解析。"
           "要取回生成文件，请说清文件名；助手可用 send_file 投递当前工作区内文件，"
           "该工具须在渠道权限中放行或批准。";
}

std::string FormatReminderListText(const nlohmann::json& payload, std::int64_t now_ms) {
    (void)now_ms;
    std::vector<std::pair<std::string, const nlohmann::json*>> jobs;
    if (payload.is_object() && payload.contains("jobs") && payload.at("jobs").is_array()) {
        for (const auto& job : payload.at("jobs")) {
            if (job.is_object() && job.contains("jobId") && job.at("jobId").is_string()) {
                jobs.emplace_back(job.at("jobId").get<std::string>(), &job);
            }
        }
    }
    if (jobs.empty()) {
        return "你还没有定时任务。想要的话直接说,比如\"每晚九点提醒我喝水\"。";
    }
    std::ostringstream out;
    out << "你的定时任务(" << jobs.size() << " 笔):\n";
    for (const auto& [job_id, job] : jobs) {
        // nlohmann 对象访问都先 contains(缺键 UB 的教训)。
        std::string state = "active";
        if (job->contains("state") && job->at("state").is_string()) {
            state = job->at("state").get<std::string>();
        }
        std::string kind = "once";
        if (job->contains("kind") && job->at("kind").is_string()) {
            kind = job->at("kind").get<std::string>();
        }
        std::string prompt;
        if (job->contains("prompt") && job->at("prompt").is_string()) {
            prompt = job->at("prompt").get<std::string>();
        }
        // 摘要:job.prompt 是 Q5 建单时冻结的自包含包装正文(头衔行 + 任务
        // 描述 + 执行指示),用户清单只该看描述——优先取"任务描述:"段
        //(ComposeJobPrompt 的冻结形,标记丢了退首行)。
        std::string summary;
        const std::size_t marker = prompt.find("任务描述:");
        if (marker != std::string::npos) {
            summary = prompt.substr(marker + std::strlen("任务描述:"));
        } else {
            summary = prompt;
        }
        const std::size_t newline = summary.find('\n');
        if (newline != std::string::npos) {
            summary.resize(newline);
        }
        out << "- [" << job_id << "] " << JobStateText(state) << " · " << ScheduleKindText(kind);
        if (!summary.empty()) {
            out << " · " << summary;
        }
        if (job->contains("nextFireMs") &&
            qq::ParseLooseInt64(job->at("nextFireMs")).has_value()) {
            out << " · 下次 "
                << FormatUtcTimestamp(*qq::ParseLooseInt64(job->at("nextFireMs")));
        }
        out << "\n";
    }
    out << "要取消哪一笔,把 jobId 告诉我。";
    return out.str();
}

std::string MakeMenuCommandDeniedText(const std::vector<std::string>& missing_tools) {
    std::string text = "这个入口当前不在你的授权范围内,没有执行。";
    if (!missing_tools.empty()) {
        text += "(需要:";
        for (std::size_t i = 0; i < missing_tools.size(); ++i) {
            if (i > 0) {
                text += ",";
            }
            text += missing_tools[i];
        }
        text += ")";
    }
    text += "需要开通的话,在本机配置里给这只账号放行对应工具。";
    return text;
}

std::string MakeMenuCommandUnavailableText() {
    return "任务查询暂时不可用(任务服务没装配)。稍后再试,或直接发消息给我。";
}

std::string FormatUtcTimestamp(std::int64_t epoch_ms) {
    const std::int64_t days = epoch_ms / 86'400'000;
    const std::int64_t ms_of_day = epoch_ms % 86'400'000;
    int year = 1970;
    unsigned month = 1;
    unsigned day = 1;
    CivilFromDays(days, &year, &month, &day);
    const std::int64_t hour = ms_of_day / 3'600'000;
    const std::int64_t minute = (ms_of_day % 3'600'000) / 60'000;
    std::ostringstream out;
    out << year << "-" << TwoDigits(month) << "-" << TwoDigits(day) << " " << TwoDigits(hour)
        << ":" << TwoDigits(minute) << " UTC";
    return out.str();
}

}  // namespace lubancode::channel
