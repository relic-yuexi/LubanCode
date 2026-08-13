#include "cli/agent_status.hpp"

#include <cstdio>
#include <mutex>
#include <utility>

#include "cli/i18n.hpp"
#include "cli/transcript.hpp"  // TruncateUtf8Codepoints —— 跟 BuildToolTitle 共用同一条截断规矩

namespace lubancode::cli {

namespace {

constexpr const char* kDot = "\xE2\x97\x8F";    // ● U+25CF,跟 transcript.cpp 同一个字符,视觉统一
constexpr const char* kElbow = "\xE2\x8E\xBF";  // ⎿ U+23BF,跟 transcript.cpp 同一个字符

// token 数超过 1000 折成 "X.Xk",不到 1000 就是原数——跟 Claude Code 的
// "27.9k tokens" 一个路数,数太大不刷屏。
std::string FormatTokenCount(std::int64_t tokens) {
    if (tokens < 0) {
        tokens = 0;
    }
    char buf[32];
    if (tokens >= 1000) {
        std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(tokens) / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(tokens));
    }
    return std::string(buf);
}

std::string StatusColor(AgentStatusState state, const Theme& theme) {
    switch (state) {
        case AgentStatusState::Running:
            return theme.tool_line;
        case AgentStatusState::Ok:
            return theme.prompt;
        case AgentStatusState::Error:
            return theme.error;
    }
    return std::string();
}

std::string StatusWord(AgentStatusState state) {
    switch (state) {
        case AgentStatusState::Running:
            return "[RUNNING]";
        case AgentStatusState::Ok:
            return "[OK]";
        case AgentStatusState::Error:
            return "[ERROR]";
    }
    return "[?]";
}

// 换行/回车/制表符压成空格——任务摘要来自子代理的 prompt 入参,可能是
// 多行文本,原样塞进单行状态条会毁掉一行一条目的记账(跟 ticker 的相对
// 光标重画配合不了)。
std::string SanitizeLabel(std::string label) {
    for (char& c : label) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
    }
    return label;
}

}  // namespace

int AgentStatusBoard::Start(std::string label) {
    std::lock_guard<std::mutex> lock(mutex_);
    AgentStatusEntry entry;
    entry.id = next_id_++;
    entry.label = TruncateUtf8Codepoints(SanitizeLabel(std::move(label)), 40);
    entry.state = AgentStatusState::Running;
    entry.start_time = std::chrono::steady_clock::now();
    const int id = entry.id;
    entries_.push_back(std::move(entry));
    return id;
}

void AgentStatusBoard::RecordToolCall(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        if (entry.id == id) {
            if (entry.state == AgentStatusState::Running) {
                ++entry.tool_calls;
            }
            return;
        }
    }
}

void AgentStatusBoard::RecordUsage(int id, std::int64_t input_tokens, std::int64_t output_tokens) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        if (entry.id == id) {
            if (entry.state == AgentStatusState::Running) {
                entry.input_tokens += input_tokens;
                entry.output_tokens += output_tokens;
            }
            return;
        }
    }
}

void AgentStatusBoard::Finish(int id, bool success) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        if (entry.id == id) {
            if (entry.state == AgentStatusState::Running) {
                entry.state = success ? AgentStatusState::Ok : AgentStatusState::Error;
                entry.end_time = std::chrono::steady_clock::now();
            }
            return;
        }
    }
}

void AgentStatusBoard::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    next_id_ = 1;
}

std::vector<AgentStatusEntry> AgentStatusBoard::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

bool AgentStatusBoard::HasRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        if (entry.state == AgentStatusState::Running) {
            return true;
        }
    }
    return false;
}

bool AgentStatusBoard::Empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.empty();
}

std::vector<std::string> FormatAgentStatusLines(const std::vector<AgentStatusEntry>& entries,
                                                  std::chrono::steady_clock::time_point now,
                                                  const Theme& theme) {
    const bool plain = theme.reset.empty();
    std::vector<std::string> lines;
    lines.reserve(entries.size() * 3);
    for (const auto& entry : entries) {
        const auto end = entry.state == AgentStatusState::Running ? now : entry.end_time;
        const double seconds = std::chrono::duration<double>(end - entry.start_time).count();
        const std::string tokens = FormatTokenCount(entry.input_tokens + entry.output_tokens);

        // 首行:状态灯(或 plain 方括号词)+ 任务摘要——跟旧版单行开头
        // 逐字一致,只是尾巴的耗时/工具次数挪到第二行去了。
        if (plain) {
            lines.push_back(StatusWord(entry.state) + " " + entry.label);
        } else {
            lines.push_back(StatusColor(entry.state, theme) + kDot + theme.reset + " " + entry.label);
        }

        // 第二行:"⎿  <状态词>(N 次工具调用 · Xk tokens · Ys)",三个数字
        // 一次凑齐,骨架照 Claude Code 的 "Done (N tool uses · Xk tokens ·
        // Ys)" 靠。
        std::string state_key;
        switch (entry.state) {
            case AgentStatusState::Running:
                state_key = "agent_status.state_running";
                break;
            case AgentStatusState::Ok:
                state_key = "agent_status.state_done";
                break;
            case AgentStatusState::Error:
                state_key = "agent_status.state_failed";
                break;
        }
        const std::string summary =
            trf("agent_status.summary", tr(state_key), entry.tool_calls, tokens, FormatSeconds(seconds));
        if (plain) {
            lines.push_back(std::string("  ") + kElbow + "  " + summary);
        } else {
            lines.push_back(std::string("  ") + theme.stats + kElbow + "  " + summary + theme.reset);
        }

        // 第三行:折叠提示,提醒用户 Ctrl+O 能展开明细(#三:紧凑模式默认
        // 只留这三行摘要,子代理内层工具调用不逐条铺屏)。
        if (plain) {
            lines.push_back(std::string("  ") + tr("agent_status.expand_hint"));
        } else {
            lines.push_back(std::string("  ") + theme.stats + tr("agent_status.expand_hint") + theme.reset);
        }
    }
    return lines;
}

}  // namespace lubancode::cli
