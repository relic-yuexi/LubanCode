#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace lubancode::config {

inline constexpr const char* kLatestReleaseApiUrl =
    "https://api.github.com/repos/relic-yuexi/LubanCode/releases/latest";

inline constexpr const char* kReleasesListApiUrl =
    "https://api.github.com/repos/relic-yuexi/LubanCode/releases?per_page=20";

struct UpdateInfo {
    std::string current_version;
    std::string latest_version;
    std::string release_url;
    bool update_available = false;
};

// GitHubRelease自动更新单 P1:Release 一侧的资产账(update 子命令用)。
// digest 是 GitHub 资产自带的 "sha256:<hex>";缺失时按 §四,没有可信摘要
// 不做自动安装——由调用方拒绝,不静默降级为不校验。
struct ReleaseAssetInfo {
    std::string name;             // 如 lubancode-v0.26.300-windows-x64.zip
    std::uint64_t id = 0;         // asset id:下载钉死它,发布更新拼不成套
    std::uint64_t size = 0;       // 字节;下载上限与磁盘预检的输入
    std::string digest;           // "sha256:<hex>";可空 = 资产没带
    std::string download_url;     // browser_download_url(展示用;下载走 asset id)
};

struct ReleaseInfo {
    std::string tag_name;         // v0.26.300
    std::string version;          // 剥 v 前缀后的版本号
    std::string html_url;
    std::uint64_t id = 0;         // release id
    bool prerelease = false;
    std::vector<ReleaseAssetInfo> assets;
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

// ---- P1:update 子命令的 Release 解析与抓取(版本比较复用上面那枚) ----

// 解析一枚 Release JSON(含 assets)。坏 JSON/缺 tag 明错;资产缺 digest 不算
// 错——那是调用方按 §四 拒绝自动安装的事,解析层如实交账。
std::expected<ReleaseInfo, std::string> ParseReleaseInfoJson(const std::string& text);

// GET releases/latest(稳定通道;GitHub 语义:latest 不含 prerelease)。
std::expected<ReleaseInfo, std::string> FetchLatestRelease(int connect_timeout_ms,
                                                            int request_timeout_secs);
// GET releases 列表,挑最新的 prerelease(预发布通道:显式选择才走)。
std::expected<ReleaseInfo, std::string> FetchNewestPrerelease(int connect_timeout_ms,
                                                              int request_timeout_secs);

// 在 Release 里按平台精确挑资产(windows-x64 / linux-x64 / macos-arm64)。
// 名字对不上明确报错,不猜。
std::expected<ReleaseAssetInfo, std::string> PickAssetForPlatform(const ReleaseInfo& release,
                                                                  const std::string& platform);

// tag/版本串 -> EXE 探针期望的版本号:剥 v 前缀再剥 '-尾巴'(release.yml
// 的口径:预发布 tag 的 EXE 印正式版本号)。已核过合法性的版本串专用。
std::string ExeVersionFromTag(std::string_view tag);

}  // namespace lubancode::config
