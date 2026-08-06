#pragma once

#include <expected>
#include <string>

namespace lubancode::config {

inline constexpr const char* kLatestReleaseApiUrl =
    "https://api.github.com/repos/relic-yuexi/LubanCode/releases/latest";

struct UpdateInfo {
    std::string current_version;
    std::string latest_version;
    std::string release_url;
    bool update_available = false;
};

// 比较两个 SemVer 版本。left 新于 right 返回 1，相同返回 0，旧于返回 -1。
// 认开头的 v；构建元数据不参与比较。
std::expected<int, std::string> CompareSemanticVersions(const std::string& left,
                                                         const std::string& right);

// 解析 GitHub releases/latest 的响应。拆出来单测，网络层只管 HTTP。
std::expected<UpdateInfo, std::string> ParseLatestReleaseJson(const std::string& text,
                                                              const std::string& current_version);

std::expected<UpdateInfo, std::string> CheckForUpdate(
    const std::string& current_version, int connect_timeout_ms, int request_timeout_secs);

}  // namespace lubancode::config
