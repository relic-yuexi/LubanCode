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

// ---------------------------------------------------------------------------
// P1:update 子命令的 Release 解析与抓取
// ---------------------------------------------------------------------------

std::expected<ReleaseInfo, std::string> ParseReleaseInfoJson(const std::string& text) {
    const nlohmann::json release = nlohmann::json::parse(text, nullptr, false);
    if (release.is_discarded() || !release.is_object()) {
        return std::unexpected("GitHub 返回的 Release 不是合法 JSON object");
    }
    if (!release.contains("tag_name") || !release["tag_name"].is_string() ||
        release["tag_name"].get_ref<const std::string&>().empty()) {
        return std::unexpected("GitHub Release 缺少 tag_name");
    }
    const auto parsed = ParseSemanticVersion(release["tag_name"].get<std::string>());
    if (!parsed.has_value()) return std::unexpected("Release " + parsed.error());

    ReleaseInfo info;
    info.tag_name = release["tag_name"].get<std::string>();
    info.version = parsed->normalized;
    if (release.contains("html_url") && release["html_url"].is_string()) {
        info.html_url = release["html_url"].get<std::string>();
    }
    if (release.contains("id") && release["id"].is_number_unsigned()) {
        info.id = release["id"].get<std::uint64_t>();
    }
    if (release.contains("prerelease") && release["prerelease"].is_boolean()) {
        info.prerelease = release["prerelease"].get<bool>();
    }
    if (release.contains("assets") && release["assets"].is_array()) {
        for (const auto& asset : release["assets"]) {
            if (!asset.is_object() || !asset.contains("name") || !asset["name"].is_string()) {
                continue;
            }
            ReleaseAssetInfo entry;
            entry.name = asset["name"].get<std::string>();
            if (asset.contains("id") && asset["id"].is_number_unsigned()) {
                entry.id = asset["id"].get<std::uint64_t>();
            }
            if (asset.contains("size") && asset["size"].is_number_unsigned()) {
                entry.size = asset["size"].get<std::uint64_t>();
            }
            if (asset.contains("digest") && asset["digest"].is_string()) {
                entry.digest = asset["digest"].get<std::string>();
            }
            if (asset.contains("browser_download_url") &&
                asset["browser_download_url"].is_string()) {
                entry.download_url = asset["browser_download_url"].get<std::string>();
            }
            info.assets.push_back(std::move(entry));
        }
    }
    return info;
}

namespace {

std::expected<std::string, std::string> GetReleaseJson(const char* url,
                                                       int connect_timeout_ms,
                                                       int request_timeout_secs) {
    const cpr::Header headers{{"User-Agent", "lubancode-update-check"},
                              {"Accept", "application/vnd.github+json"},
                              {"X-GitHub-Api-Version", "2022-11-28"}};
    const cpr::Response response = cpr::Get(
        cpr::Url{url}, headers,
        cpr::ConnectTimeout{std::chrono::milliseconds(connect_timeout_ms)},
        cpr::Timeout{std::chrono::seconds(request_timeout_secs)});
    if (response.error) return std::unexpected("查询 GitHub Release 失败: " + response.error.message);
    if (response.status_code < 200 || response.status_code >= 300) {
        return std::unexpected("查询 GitHub Release 失败: HTTP " +
                               std::to_string(response.status_code));
    }
    if (response.text.size() > 4 * 1024 * 1024) {
        return std::unexpected("GitHub Release 响应超过 4 MiB，已拒绝解析");
    }
    return response.text;
}

}  // namespace

std::expected<ReleaseInfo, std::string> FetchLatestRelease(int connect_timeout_ms,
                                                            int request_timeout_secs) {
    const auto text = GetReleaseJson(kLatestReleaseApiUrl, connect_timeout_ms, request_timeout_secs);
    if (!text.has_value()) return std::unexpected(text.error());
    auto info = ParseReleaseInfoJson(*text);
    if (info.has_value()) info->prerelease = false;  // latest 语义:不含预发布
    return info;
}

std::expected<ReleaseInfo, std::string> FetchNewestPrerelease(int connect_timeout_ms,
                                                              int request_timeout_secs) {
    const auto text = GetReleaseJson(kReleasesListApiUrl, connect_timeout_ms, request_timeout_secs);
    if (!text.has_value()) return std::unexpected(text.error());
    const nlohmann::json list = nlohmann::json::parse(*text, nullptr, false);
    if (list.is_discarded() || !list.is_array()) {
        return std::unexpected("GitHub releases 列表不是 JSON 数组");
    }
    for (const auto& item : list) {
        if (!item.is_object()) continue;
        if (!item.contains("prerelease") || !item["prerelease"].is_boolean() ||
            !item["prerelease"].get<bool>()) {
            continue;
        }
        // 列表元素与单枚 Release 同构,直接序列化给共享解析器
        const auto info = ParseReleaseInfoJson(item.dump());
        if (info.has_value()) return info;
    }
    return std::unexpected("没有找到预发布 Release");
}

std::expected<ReleaseAssetInfo, std::string> PickAssetForPlatform(const ReleaseInfo& release,
                                                                  const std::string& platform) {
    const std::string marker = "-" + platform + ".";
    for (const auto& asset : release.assets) {
        const std::size_t at = asset.name.find(marker);
        if (at == std::string::npos) continue;
        if (at + marker.size() <= asset.name.size()) return asset;  // 后缀是 .zip/.tar.gz
    }
    return std::unexpected("Release " + release.tag_name + " 里找不到平台 " + platform +
                           " 的资产");
}

std::string ExeVersionFromTag(std::string_view tag) {
    if (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V')) tag.remove_prefix(1);
    const std::size_t dash = tag.find('-');
    return std::string(tag.substr(0, dash == std::string_view::npos ? tag.size() : dash));
}

}  // namespace lubancode::config
