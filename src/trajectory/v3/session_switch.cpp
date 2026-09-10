// 新会话 v3 开关实现 + 接线点 2 的目录发现助手。接线点清单见
// session_switch.hpp 头注。
#include "trajectory/v3/session_switch.hpp"

#include <ctime>
#include <fstream>

#include "platform/paths.hpp"
#include "trajectory/v3/envelope.hpp"

namespace lubancode::trajectory::v3 {

bool NewSessionV3WriteEnabled() {
    auto value = platform::GetEnvVar("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS");
    return value.has_value() && *value == "1";
}

std::optional<nlohmann::json> ReadV3FirstLine(const std::filesystem::path& stream) {
    std::error_code ec;
    if (!std::filesystem::exists(stream, ec)) {
        return std::nullopt;
    }
    std::ifstream file(stream, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::string line;
    if (!std::getline(file, line)) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const nlohmann::json json = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded() || !json.is_object()) {
        return std::nullopt;
    }
    return json;
}

std::optional<std::filesystem::path> FindV3SessionStream(const std::filesystem::path& session_dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(session_dir, ec)) {
        return std::nullopt;
    }
    // main.jsonl 在 = v2 布局:两回路互斥,不在此处分派(接线点 1 开关
    // 管写侧二选一,读侧只认既有格式)。
    if (std::filesystem::exists(session_dir / "main.jsonl", ec)) {
        return std::nullopt;
    }
    const std::string id = platform::PathToUtf8(session_dir.filename());
    if (id.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path stream = session_dir / platform::Utf8ToPath(id + ".jsonl");
    auto first = ReadV3FirstLine(stream);
    if (!first.has_value()) {
        return std::nullopt;
    }
    const auto version = first->find("schemaVersion");
    if (version == first->end() || !version->is_number_integer() ||
        version->get<int>() != kSchemaVersion) {
        return std::nullopt;
    }
    return stream;
}

std::optional<std::int64_t> ParseV3TimestampMs(const std::string& iso) {
    // "yyyy-MM-ddTHH:mm:ss[.fff]Z":前 19 位固定,毫秒段至多按三位折算
    //(不足补零,超出截断——排序粒度毫秒已够)。
    if (iso.size() < 19 || iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' ||
        iso[16] != ':') {
        return std::nullopt;
    }
    for (const std::size_t pos : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18}) {
        if (iso[pos] < '0' || iso[pos] > '9') {
            return std::nullopt;
        }
    }
    std::tm parts{};
    parts.tm_year = (iso[0] - '0') * 1000 + (iso[1] - '0') * 100 + (iso[2] - '0') * 10 +
                    (iso[3] - '0') - 1900;
    parts.tm_mon = (iso[5] - '0') * 10 + (iso[6] - '0') - 1;
    parts.tm_mday = (iso[8] - '0') * 10 + (iso[9] - '0');
    parts.tm_hour = (iso[11] - '0') * 10 + (iso[12] - '0');
    parts.tm_min = (iso[14] - '0') * 10 + (iso[15] - '0');
    parts.tm_sec = (iso[17] - '0') * 10 + (iso[18] - '0');
    parts.tm_isdst = 0;
#ifdef _WIN32
    const std::time_t seconds = _mkgmtime(&parts);
#else
    const std::time_t seconds = timegm(&parts);
#endif
    if (seconds == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    std::int64_t ms = static_cast<std::int64_t>(seconds) * 1000;
    if (iso.size() > 20 && iso[19] == '.') {
        std::int64_t fraction = 0;
        int digits = 0;
        for (std::size_t i = 20; i < iso.size() && digits < 3; ++i) {
            if (iso[i] < '0' || iso[i] > '9') {
                break;
            }
            fraction = fraction * 10 + (iso[i] - '0');
            ++digits;
        }
        while (digits > 0 && digits < 3) {
            fraction *= 10;
            ++digits;
        }
        ms += fraction;
    }
    return ms;
}

}  // namespace lubancode::trajectory::v3
