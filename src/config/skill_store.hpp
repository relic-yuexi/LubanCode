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
// 安装本机已有技能。source_path 可指向技能目录、目录里的 SKILL.md，
// 或一份独立 Markdown；整包落进 skills_root/<技能名>，不记远端来源。
std::expected<SkillInstallResult, std::string> InstallLocalSkill(
    const std::filesystem::path& skills_root, const std::string& source_path);
// /skill install 的统一入口：http(s) 走远端，其余一概按本地路径办。
std::expected<SkillInstallResult, std::string> InstallSkillSource(
    const std::filesystem::path& skills_root, const std::string& source, const SkillHttpGet& http_get);
// 录制草稿的原子安装(0.25.x"录一遍生成技能"):draft_dir 须有普通文件
// SKILL.md;validate 先行把关——收 SKILL.md 全文,返回干净技能名或错误
// (校验规矩在 agent 层的起草器那头,config 层不认得,经回调注入,依赖
// 仍旧单向)。过了再走与 InstallLocalSkill 同一套 staging+rename 整包落
// 进 skills_root/<名字>,目标已存在/校验不过一律报错不动本地文件。
std::expected<SkillInstallResult, std::string> InstallDraftSkill(
    const std::filesystem::path& skills_root, const std::filesystem::path& draft_dir,
    const std::function<std::expected<std::string, std::string>(const std::string& skill_md_content)>& validate);
std::expected<void, std::string> RemoveStoredSkill(const std::filesystem::path& skills_root,
                                                    const std::string& name);

}  // namespace lubancode::config
