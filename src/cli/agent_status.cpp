#include "cli/agent_status.hpp"

#include <cstdio>
#include <mutex>
#include <utility>

#include "cli/i18n.hpp"
#include "cli/transcript.hpp"  // TruncateUtf8Codepoints —— 跟 BuildToolTitle 共用同一条截断规矩

namespace lubancode::cli {

namespace {

constexpr const char* kDot = "\xE2\x97\x8F";  // ● U+25CF,跟 transcript.cpp 同一个字符,视觉统一

std::string FormatSeconds(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fs", seconds);
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
    lines.reserve(entries.size());
    for (const auto& entry : entries) {
        const auto end = entry.state == AgentStatusState::Running ? now : entry.end_time;
        const double seconds = std::chrono::duration<double>(end - entry.start_time).count();

        std::string prefix;
        if (plain) {
            prefix = StatusWord(entry.state) + std::string(" ");
        } else {
            prefix = StatusColor(entry.state, theme) + kDot + theme.reset + " ";
        }

        std::string key;
        switch (entry.state) {
            case AgentStatusState::Running:
                key = "agent_status.running";
                break;
            case AgentStatusState::Ok:
                key = "agent_status.done_ok";
                break;
            case AgentStatusState::Error:
                key = "agent_status.done_error";
                break;
        }
        lines.push_back(prefix + trf(key, entry.label, FormatSeconds(seconds), entry.tool_calls));
    }
    return lines;
}

}  // namespace lubancode::cli
