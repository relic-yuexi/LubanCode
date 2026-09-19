// 显式全局通知区的实现(按代理状态投影单 P3)。

#include "cli/global_notice.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "platform/console.hpp"

namespace lubancode::cli {

GlobalNoticeBoard& SessionGlobalNotices() {
    static GlobalNoticeBoard board;
    return board;
}

void GlobalNoticeBoard::Push(std::string text, std::chrono::milliseconds ttl) {
    // 首尾空白裁掉;全空白的行没有信息量,不占板。
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    if (text.empty()) {
        return;
    }
    const auto until = std::chrono::steady_clock::now() + ttl;
    std::lock_guard<std::mutex> lock(mutex_);
    PruneLocked();
    // 同文连续重复只续命,不叠条——心跳异常那类高频源刷不满板。
    for (Notice& notice : notices_) {
        if (notice.text == text) {
            notice.until = until;
            return;
        }
    }
    notices_.push_back(Notice{std::move(text), until});
    if (notices_.size() > kMaxNotices) {
        notices_.erase(notices_.begin(),
                       notices_.end() - static_cast<std::ptrdiff_t>(kMaxNotices));
    }
}

std::vector<std::string> GlobalNoticeBoard::ActiveRows() {
    std::lock_guard<std::mutex> lock(mutex_);
    PruneLocked();
    std::vector<std::string> rows;
    rows.reserve(notices_.size());
    for (const Notice& notice : notices_) {
        rows.push_back(notice.text);
    }
    return rows;
}

bool GlobalNoticeBoard::HasActive() {
    std::lock_guard<std::mutex> lock(mutex_);
    PruneLocked();
    return !notices_.empty();
}

void GlobalNoticeBoard::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    notices_.clear();
}

void GlobalNoticeBoard::PruneLocked() {
    const auto now = std::chrono::steady_clock::now();
    notices_.erase(std::remove_if(notices_.begin(), notices_.end(),
                                  [now](const Notice& notice) { return notice.until <= now; }),
                   notices_.end());
}

bool ReportDiagnosticLine(std::string line) {
    // 交互判定与工具确认/ask_user 同一颗尺:stdin 可交互 + stdout 是 console。
    // 判定有成本(进程级缓存),只在真有诊断要报时才问。
    if (!platform::StdinIsInteractive() || !platform::ProbeStdoutConsole().is_console) {
        return false;
    }
    SessionGlobalNotices().Push(std::move(line));
    return true;
}

}  // namespace lubancode::cli
