// 远端技能库:远端技能的持久化、来源账本和网络抽象。

#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::config {

constexpr std::size_t kMaxRemoteSkillBytes = 1024 * 1024;

enum class RemoteSkillSourceKind { MarkdownFile, GithubRepository };

struct RemoteSkillSource {
    RemoteSkillSourceKind kind = RemoteSkillSourceKind::MarkdownFile;
    std::string url;
    std::string owner;
    std::string repository;
    std::string ref;
};

struct SkillHttpResponse {
    int status_code = 0;
    std::string body;
    std::string error;
};
using SkillHttpGet = std::function<SkillHttpResponse(const std::string& url)>;

struct RemoteSkillRecord {
    std::string name;
    std::string source_url;
    std::string installed_at;
};

struct StoredSkill {
    std::string name;
    std::string dir_path;
    std::optional<std::string> source_url;
    std::optional<std::string> installed_at;
};

struct SkillInstallOptions {
    bool overwrite = false;
    std::vector<std::string> only_names;
    std::string installed_at;
};

struct SkillInstallResult {
    std::vector<std::string> installed_names;
};

std::expected<RemoteSkillSource, std::string> ParseRemoteSkillSource(const std::string& url);
std::expected<std::string, std::string> SanitizeSkillDirectoryName(const std::string& name);
SkillHttpResponse FetchRemoteSkillUrl(const std::string& url);

std::expected<std::vector<RemoteSkillRecord>, std::string> LoadRemoteSkillRecords(
    const std::filesystem::path& skills_root);
std::expected<void, std::string> SaveRemoteSkillRecords(const std::filesystem::path& skills_root,
                                                         const std::vector<RemoteSkillRecord>& records);
std::expected<std::vector<StoredSkill>, std::string> ListStoredSkills(const std::filesystem::path& skills_root);

std::expected<SkillInstallResult, std::string> InstallRemoteSkills(
    const std::filesystem::path& skills_root, const std::string& source_url, const SkillHttpGet& http_get,
    const SkillInstallOptions& options = {});
std::expected<void, std::string> RemoveStoredSkill(const std::filesystem::path& skills_root,
                                                    const std::string& name);

}  // namespace lubancode::config
