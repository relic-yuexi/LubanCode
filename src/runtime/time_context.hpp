#pragma once

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

}  // namespace lubancode::runtime
