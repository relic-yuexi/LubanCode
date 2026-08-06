#include "config/update_checker.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <vector>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

namespace lubancode::config {

namespace {

struct SemanticVersion {
    std::array<std::uint64_t, 3> core{};
    std::vector<std::string> prerelease;
    std::string normalized;
};

bool IsNumeric(std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](char ch) {
               return std::isdigit(static_cast<unsigned char>(ch)) != 0;
           });
}

std::vector<std::string> Split(std::string_view value, char delimiter) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = value.find(delimiter, begin);
        parts.emplace_back(value.substr(begin, end == std::string_view::npos
                                                   ? value.size() - begin
                                                   : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return parts;
}

std::expected<SemanticVersion, std::string> ParseSemanticVersion(std::string_view raw) {
    if (!raw.empty() && (raw.front() == 'v' || raw.front() == 'V')) raw.remove_prefix(1);
    if (raw.empty()) return std::unexpected("版本号为空");

    const std::size_t plus = raw.find('+');
    const std::string_view without_build = raw.substr(0, plus);
    const std::size_t dash = without_build.find('-');
    const std::string_view core_text = without_build.substr(0, dash);
    const std::string_view prerelease_text =
        dash == std::string_view::npos ? std::string_view{} : without_build.substr(dash + 1);

    const auto core_parts = Split(core_text, '.');
    if (core_parts.size() != 3) {
        return std::unexpected("版本号须是 major.minor.patch: " + std::string(raw));
    }

    SemanticVersion parsed;
    for (std::size_t i = 0; i < core_parts.size(); ++i) {
        const std::string& part = core_parts[i];
        if (!IsNumeric(part) || (part.size() > 1 && part.front() == '0')) {
            return std::unexpected("版本号数字段不合法: " + std::string(raw));
        }
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), parsed.core[i]);
        if (error != std::errc() || end != part.data() + part.size()) {
            return std::unexpected("版本号数字过大: " + std::string(raw));
        }
    }

    if (dash != std::string_view::npos) {
        if (prerelease_text.empty()) return std::unexpected("预发布版本标识为空: " + std::string(raw));
        parsed.prerelease = Split(prerelease_text, '.');
        for (const std::string& part : parsed.prerelease) {
            if (part.empty() || !std::all_of(part.begin(), part.end(), [](char ch) {
                    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '-';
                }) || (IsNumeric(part) && part.size() > 1 && part.front() == '0')) {
                return std::unexpected("预发布版本标识不合法: " + std::string(raw));
            }
        }
    }

    parsed.normalized = std::string(without_build);
    return parsed;
}

int ComparePrerelease(const std::vector<std::string>& left, const std::vector<std::string>& right) {
    if (left.empty() || right.empty()) {
        if (left.empty() && right.empty()) return 0;
        return left.empty() ? 1 : -1;  // 正式版高于同号预发布版
    }
    const std::size_t common = (std::min)(left.size(), right.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (left[i] == right[i]) continue;
        const bool left_numeric = IsNumeric(left[i]);
        const bool right_numeric = IsNumeric(right[i]);
        if (left_numeric && right_numeric) {
            if (left[i].size() != right[i].size()) {
                return left[i].size() < right[i].size() ? -1 : 1;
            }
            return left[i] < right[i] ? -1 : 1;
        }
        if (left_numeric != right_numeric) return left_numeric ? -1 : 1;
        return left[i] < right[i] ? -1 : 1;
    }
    if (left.size() == right.size()) return 0;
    return left.size() < right.size() ? -1 : 1;
}

}  // namespace

std::expected<int, std::string> CompareSemanticVersions(const std::string& left,
                                                         const std::string& right) {
    const auto parsed_left = ParseSemanticVersion(left);
    if (!parsed_left.has_value()) return std::unexpected(parsed_left.error());
    const auto parsed_right = ParseSemanticVersion(right);
    if (!parsed_right.has_value()) return std::unexpected(parsed_right.error());
    if (parsed_left->core < parsed_right->core) return -1;
    if (parsed_left->core > parsed_right->core) return 1;
    return ComparePrerelease(parsed_left->prerelease, parsed_right->prerelease);
}

std::expected<UpdateInfo, std::string> ParseLatestReleaseJson(const std::string& text,
                                                              const std::string& current_version) {
    const nlohmann::json release = nlohmann::json::parse(text, nullptr, false);
    if (release.is_discarded() || !release.is_object()) {
        return std::unexpected("GitHub 返回的 Release 不是合法 JSON object");
    }
    if (!release.contains("tag_name") || !release["tag_name"].is_string() ||
        release["tag_name"].get_ref<const std::string&>().empty()) {
        return std::unexpected("GitHub Release 缺少 tag_name");
    }
    if (!release.contains("html_url") || !release["html_url"].is_string() ||
        release["html_url"].get_ref<const std::string&>().empty()) {
        return std::unexpected("GitHub Release 缺少 html_url");
    }

    const auto current = ParseSemanticVersion(current_version);
    if (!current.has_value()) return std::unexpected("当前" + current.error());
    const auto latest = ParseSemanticVersion(release["tag_name"].get<std::string>());
    if (!latest.has_value()) return std::unexpected("远端" + latest.error());
    const auto comparison = CompareSemanticVersions(latest->normalized, current->normalized);
    if (!comparison.has_value()) return std::unexpected(comparison.error());

    UpdateInfo info;
    info.current_version = current->normalized;
    info.latest_version = latest->normalized;
    info.release_url = release["html_url"].get<std::string>();
    info.update_available = *comparison > 0;
    return info;
}

std::expected<UpdateInfo, std::string> CheckForUpdate(const std::string& current_version,
                                                      int connect_timeout_ms,
                                                      int request_timeout_secs) {
    const cpr::Header headers{{"User-Agent", "lubancode-update-check/" + current_version},
                              {"Accept", "application/vnd.github+json"},
                              {"X-GitHub-Api-Version", "2022-11-28"}};
    const cpr::Response response = cpr::Get(
        cpr::Url{kLatestReleaseApiUrl}, headers,
        cpr::ConnectTimeout{std::chrono::milliseconds(connect_timeout_ms)},
        cpr::Timeout{std::chrono::seconds(request_timeout_secs)});
    if (response.error) return std::unexpected("查询 GitHub Release 失败: " + response.error.message);
    if (response.status_code < 200 || response.status_code >= 300) {
        return std::unexpected("查询 GitHub Release 失败: HTTP " + std::to_string(response.status_code));
    }
    if (response.text.size() > 1024 * 1024) {
        return std::unexpected("GitHub Release 响应超过 1 MiB，已拒绝解析");
    }
    return ParseLatestReleaseJson(response.text, current_version);
}

}  // namespace lubancode::config
