#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>

namespace lubancode::runtime {

inline std::string UtcTimeText(std::int64_t ms) {
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &seconds) != 0) return {};
#else
    if (gmtime_r(&seconds, &utc) == nullptr) return {};
#endif
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

inline std::string CurrentTimeContext() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "\n本轮开始时间(宿主时钟): " + UtcTimeText(ms) +
           "\n相对提醒用 create_reminder.delay_seconds，由宿主计算；不要猜测或试探时间戳。\n";
}

}  // namespace lubancode::runtime
