#include "config/skill_store.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <string_view>
#include <utility>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>


namespace lubancode::config {

namespace {

namespace fs = std::filesystem;

fs::path Utf8ToPath(const std::string& utf8) {
    const std::u8string_view view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return fs::path(view);
}

std::string PathToUtf8(const fs::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

constexpr const char* kSourcesFileName = "remote-sources.json";

struct RemoteFile {
    std::string relative_path;
    std::string content;
};

struct RemotePackage {
    std::string name;
    std::vector<RemoteFile> files;
};

std::string Trim(const std::string& text) {
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) {
        --last;
    }
    return text.substr(first, last - first);
}

std::string UnquotePath(const std::string& text) {
    const std::string trimmed = Trim(text);
    if (trimmed.size() >= 2 && ((trimmed.front() == '"' && trimmed.back() == '"') ||
                                (trimmed.front() == '\'' && trimmed.back() == '\''))) {
        return trimmed.substr(1, trimmed.size() - 2);
    }
    return trimmed;
}

std::string ToLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool IsHttpUrl(const std::string& url) {
    const std::string lower = ToLower(url);
    return StartsWith(lower, "http://") || StartsWith(lower, "https://");
}

std::string StripQueryAndFragment(const std::string& url) {
    const std::size_t end = url.find_first_of("?#");
    return end == std::string::npos ? url : url.substr(0, end);
}

std::vector<std::string> SplitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string part = path.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!part.empty()) {
            parts.push_back(part);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return parts;
}

bool EndsWithMd(const std::string& text) {
    const std::string lower = ToLower(text);
    return lower.size() > 3 && lower.compare(lower.size() - 3, 3, ".md") == 0;
}

std::string UrlEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out += static_cast<char>(ch);
        } else {
            out += '%';
            out += kHex[(ch >> 4) & 0x0F];
            out += kHex[ch & 0x0F];
        }
    }
    return out;
}

std::string UrlEncodePath(const std::string& path) {
    std::string out;
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t end = path.find('/', begin);
        if (!out.empty()) {
            out += '/';
        }
        out += UrlEncode(path.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return out;
}

std::expected<void, std::string> CheckResponse(const SkillHttpResponse& response, const std::string& url) {
    if (!response.error.empty()) {
        return std::unexpected("网络请求失败: " + response.error);
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        return std::unexpected("服务端返回 HTTP " + std::to_string(response.status_code) + ": " + url);
    }
    return {};
}

std::expected<std::string, std::string> CheckSkillContent(const std::string& content, const std::string& label) {
    if (content.empty()) {
        return std::unexpected(label + " 内容为空");
    }
    if (content.size() > kMaxRemoteSkillBytes) {
        return std::unexpected(label + " 超过 " + std::to_string(kMaxRemoteSkillBytes) + " 字节上限");
    }
    if (content.find('\0') != std::string::npos) {
        return std::unexpected(label + " 含二进制 NUL 字节,不是文本技能");
    }
    return content;
}

bool IsSafeRelativePath(const std::string& path) {
    if (path.empty() || path[0] == '/' || path[0] == '\\' || path.find('\\') != std::string::npos ||
        path.find('\0') != std::string::npos) {
        return false;
    }
    for (const std::string& part : SplitPath(path)) {
        if (part.empty() || part == "." || part == ".." || part.find("..") != std::string::npos) {
            return false;
        }
        for (const unsigned char ch : part) {
            if (ch < 0x20 || ch == 0x7F) {
                return false;
            }
        }
    }
    return true;
}

std::string CurrentUtcTimestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

fs::path SourcesPath(const fs::path& skills_root) {
    return skills_root / kSourcesFileName;
}

std::expected<std::vector<RemoteSkillRecord>, std::string> ParseRecords(const std::string& text,
                                                                          const fs::path& source_path) {
    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        if (!root.is_object()) {
            return std::unexpected("来源账本不是 JSON object: " + PathToUtf8(source_path));
        }
        const auto skills_it = root.find("skills");
        if (skills_it == root.end()) {
            return std::vector<RemoteSkillRecord>{};
        }
        if (!skills_it->is_object()) {
            return std::unexpected("来源账本的 skills 字段不是 object: " + PathToUtf8(source_path));
        }

        std::vector<RemoteSkillRecord> records;
        for (auto it = skills_it->begin(); it != skills_it->end(); ++it) {
            const auto clean_name = SanitizeSkillDirectoryName(it.key());
            if (!clean_name.has_value() || !it.value().is_object()) {
                return std::unexpected("来源账本含损坏条目: " + PathToUtf8(source_path));
            }
            const auto url_it = it.value().find("url");
            const auto time_it = it.value().find("installed_at");
            if (url_it == it.value().end() || !url_it->is_string() || time_it == it.value().end() ||
                !time_it->is_string() || url_it->get<std::string>().empty()) {
                return std::unexpected("来源账本含不完整条目: " + PathToUtf8(source_path));
            }
            records.push_back({*clean_name, url_it->get<std::string>(), time_it->get<std::string>()});
        }
        std::sort(records.begin(), records.end(),
                  [](const RemoteSkillRecord& left, const RemoteSkillRecord& right) { return left.name < right.name; });
        return records;
    } catch (const nlohmann::json::exception& error) {
        return std::unexpected("来源账本 JSON 损坏(" + PathToUtf8(source_path) + "): " + error.what());
    }
}

std::expected<void, std::string> WriteWholeFile(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return std::unexpected("打不开文件: " + PathToUtf8(path));
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file.good()) {
        return std::unexpected("写文件失败: " + PathToUtf8(path));
    }
    return {};
}

std::expected<void, std::string> EnsureDir(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        return std::unexpected("建目录失败: " + PathToUtf8(path) + ": " + ec.message());
    }
    return {};
}

std::expected<std::vector<RemotePackage>, std::string> DownloadMarkdownFile(const RemoteSkillSource& source,
                                                                              const SkillHttpGet& http_get) {
    const std::string url_path = StripQueryAndFragment(source.url);
    const std::size_t slash = url_path.find_last_of('/');
    const std::string file_name = slash == std::string::npos ? url_path : url_path.substr(slash + 1);
    const std::string stem = file_name.substr(0, file_name.size() - 3);
    const auto name = SanitizeSkillDirectoryName(stem);
    if (!name.has_value()) {
        return std::unexpected(name.error());
    }

    const SkillHttpResponse response = http_get(source.url);
    if (const auto checked = CheckResponse(response, source.url); !checked.has_value()) {
        return std::unexpected(checked.error());
    }
    const auto content = CheckSkillContent(response.body, "技能文件 " + source.url);
    if (!content.has_value()) {
        return std::unexpected(content.error());
    }
    return std::vector<RemotePackage>{{*name, {{"SKILL.md", *content}}}};
}

std::expected<std::vector<RemotePackage>, std::string> DownloadGithubRepository(const RemoteSkillSource& source,
                                                                                   const SkillHttpGet& http_get) {
    std::string ref = source.ref;
    if (ref.empty()) {
        const std::string repo_url = "https://api.github.com/repos/" + UrlEncode(source.owner) + "/" +
                                     UrlEncode(source.repository);
        const SkillHttpResponse response = http_get(repo_url);
        if (const auto checked = CheckResponse(response, repo_url); !checked.has_value()) {
            return std::unexpected(checked.error());
        }
        try {
            const nlohmann::json repo = nlohmann::json::parse(response.body);
            if (!repo.contains("default_branch") || !repo.at("default_branch").is_string() ||
                repo.at("default_branch").get<std::string>().empty()) {
                return std::unexpected("GitHub 仓库没有 default_branch: " + source.url);
            }
            ref = repo.at("default_branch").get<std::string>();
        } catch (const nlohmann::json::exception& error) {
            return std::unexpected("GitHub 仓库信息不是可读 JSON: " + std::string(error.what()));
        }
    }

    const std::string tree_url = "https://api.github.com/repos/" + UrlEncode(source.owner) + "/" +
                                 UrlEncode(source.repository) + "/git/trees/" + UrlEncode(ref) + "?recursive=1";
    const SkillHttpResponse tree_response = http_get(tree_url);
    if (const auto checked = CheckResponse(tree_response, tree_url); !checked.has_value()) {
        return std::unexpected(checked.error());
    }

    struct TreeFile {
        std::string path;
        std::string package_name;
        std::string relative_path;
    };
    std::vector<TreeFile> tree_files;
    std::set<std::string> packages_with_skill;
    try {
        const nlohmann::json tree = nlohmann::json::parse(tree_response.body);
        if (tree.value("truncated", false)) {
            return std::unexpected("GitHub 仓库树太大，API 截断了结果: " + source.url);
        }
        if (!tree.contains("tree") || !tree.at("tree").is_array()) {
            return std::unexpected("GitHub 仓库树格式不对: " + source.url);
        }
        for (const auto& entry : tree.at("tree")) {
            if (!entry.is_object() || entry.value("type", std::string()) != "blob" || !entry.contains("path") ||
                !entry.at("path").is_string()) {
                continue;
            }
            const std::string path = entry.at("path").get<std::string>();
            if (!IsSafeRelativePath(path)) {
                return std::unexpected("GitHub 仓库含不安全路径: " + path);
            }
            const std::vector<std::string> parts = SplitPath(path);
            if (parts.size() == 3 && parts[0] == "skills" && parts[2] == "SKILL.md") {
                const auto name = SanitizeSkillDirectoryName(parts[1]);
                if (!name.has_value()) {
                    return std::unexpected(name.error());
                }
                packages_with_skill.insert(*name);
            }
        }

        std::set<std::pair<std::string, std::string>> seen_outputs;
        for (const auto& entry : tree.at("tree")) {
            if (!entry.is_object() || entry.value("type", std::string()) != "blob" || !entry.contains("path") ||
                !entry.at("path").is_string()) {
                continue;
            }
            const std::string path = entry.at("path").get<std::string>();
            const std::vector<std::string> parts = SplitPath(path);
            if (parts.size() == 1 && EndsWithMd(parts[0])) {
                const auto name = SanitizeSkillDirectoryName(parts[0].substr(0, parts[0].size() - 3));
                if (!name.has_value()) {
                    return std::unexpected(name.error());
                }
                if (!seen_outputs.emplace(*name, "SKILL.md").second) {
                    return std::unexpected("仓库里的技能目录名冲突: " + *name);
                }
                tree_files.push_back({path, *name, "SKILL.md"});
                continue;
            }
            if (parts.size() >= 3 && parts[0] == "skills") {
                const auto name = SanitizeSkillDirectoryName(parts[1]);
                if (!name.has_value()) {
                    return std::unexpected(name.error());
                }
                if (packages_with_skill.count(*name) != 0) {
                    const std::string relative = path.substr(std::string("skills/").size() + parts[1].size() + 1);
                    if (!IsSafeRelativePath(relative)) {
                        return std::unexpected("GitHub 技能含不安全相对路径: " + path);
                    }
                    if (!seen_outputs.emplace(*name, relative).second) {
                        return std::unexpected("GitHub 技能里有重复文件: " + path);
                    }
                    tree_files.push_back({path, *name, relative});
                }
            }
        }
    } catch (const nlohmann::json::exception& error) {
        return std::unexpected("GitHub 仓库树不是可读 JSON: " + std::string(error.what()));
    }

    if (tree_files.empty()) {
        return std::unexpected("仓库里没有 skills/<名字>/SKILL.md 或根目录 .md: " + source.url);
    }

    std::map<std::string, RemotePackage> packages;
    std::size_t total_bytes = 0;
    for (const TreeFile& file : tree_files) {
        const std::string raw_url = "https://raw.githubusercontent.com/" + UrlEncode(source.owner) + "/" +
                                    UrlEncode(source.repository) + "/" + UrlEncode(ref) + "/" +
                                    UrlEncodePath(file.path);
        const SkillHttpResponse response = http_get(raw_url);
        if (const auto checked = CheckResponse(response, raw_url); !checked.has_value()) {
            return std::unexpected(checked.error());
        }
        const auto content = CheckSkillContent(response.body, "技能文件 " + file.path);
        if (!content.has_value()) {
            return std::unexpected(content.error());
        }
        if (response.body.size() > kMaxRemoteSkillBytes - total_bytes) {
            return std::unexpected("整个 GitHub 技能包超过 " + std::to_string(kMaxRemoteSkillBytes) + " 字节上限");
        }
        total_bytes += response.body.size();
        RemotePackage& package = packages[file.package_name];
        package.name = file.package_name;
        package.files.push_back({file.relative_path, *content});
    }

    std::vector<RemotePackage> out;
    out.reserve(packages.size());
    for (auto& [name, package] : packages) {
        (void)name;
        std::sort(package.files.begin(), package.files.end(),
                  [](const RemoteFile& left, const RemoteFile& right) { return left.relative_path < right.relative_path; });
        out.push_back(std::move(package));
    }
    return out;
}

std::expected<std::vector<RemotePackage>, std::string> DownloadPackages(const RemoteSkillSource& source,
                                                                          const SkillHttpGet& http_get) {
    if (!http_get) {
        return std::unexpected("HTTP 获取函数为空");
    }
    if (source.kind == RemoteSkillSourceKind::MarkdownFile) {
        return DownloadMarkdownFile(source, http_get);
    }
    return DownloadGithubRepository(source, http_get);
}

std::string NewStagingName() {
    static std::atomic<unsigned long long> sequence{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return ".remote-skill-staging-" + std::to_string(ticks) + "-" + std::to_string(++sequence);
}

}  // namespace

std::expected<RemoteSkillSource, std::string> ParseRemoteSkillSource(const std::string& raw_url) {
    const std::string url = Trim(raw_url);
    if (!IsHttpUrl(url)) {
        return std::unexpected("URL 必须以 http:// 或 https:// 开头");
    }
    const std::string without_query = StripQueryAndFragment(url);
    const std::size_t scheme_end = without_query.find("://");
    const std::size_t host_end = without_query.find('/', scheme_end + 3);
    const std::string host = ToLower(without_query.substr(scheme_end + 3, host_end - (scheme_end + 3)));
    const std::string path = host_end == std::string::npos ? std::string() : without_query.substr(host_end + 1);
    if (host.empty() || path.empty()) {
        return std::unexpected("URL 缺少可下载的路径");
    }
    if (path.find("../") != std::string::npos || path.find('\\') != std::string::npos) {
        return std::unexpected("URL 路径不收 ../ 或反斜杠");
    }
    if (EndsWithMd(path)) {
        return RemoteSkillSource{RemoteSkillSourceKind::MarkdownFile, url, {}, {}, {}};
    }

    if (host != "github.com" && host != "www.github.com") {
        return std::unexpected("Git 仓库目前只认 github.com URL；裸 .md 直链不受此限");
    }
    const std::vector<std::string> parts = SplitPath(path);
    if (parts.size() < 2) {
        return std::unexpected("GitHub 仓库 URL 要写成 https://github.com/<owner>/<repo>");
    }
    std::string repo = parts[1];
    if (repo.size() > 4 && repo.compare(repo.size() - 4, 4, ".git") == 0) {
        repo.resize(repo.size() - 4);
    }
    const auto owner_name = SanitizeSkillDirectoryName(parts[0]);
    const auto repo_name = SanitizeSkillDirectoryName(repo);
    if (!owner_name.has_value() || !repo_name.has_value()) {
        return std::unexpected("GitHub owner 或仓库名不合法");
    }

    std::string ref;
    if (parts.size() > 2) {
        if (parts.size() < 4 || parts[2] != "tree") {
            return std::unexpected("GitHub 仓库 URL 只认根地址或 /tree/<分支>");
        }
        for (std::size_t i = 3; i < parts.size(); ++i) {
            if (!ref.empty()) {
                ref += '/';
            }
            ref += parts[i];
        }
        if (ref.empty() || ref.find("..") != std::string::npos || ref.find('\\') != std::string::npos) {
            return std::unexpected("GitHub 分支或 tag 不合法");
        }
    }
    return RemoteSkillSource{RemoteSkillSourceKind::GithubRepository, url, *owner_name, *repo_name, ref};
}

std::expected<std::string, std::string> SanitizeSkillDirectoryName(const std::string& raw_name) {
    const std::string name = Trim(raw_name);
    if (name.empty() || name == "." || name == ".." || name.find("..") != std::string::npos ||
        name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name.find('\0') != std::string::npos) {
        return std::unexpected("技能名不合法（不收空名、../、斜杠）: " + raw_name);
    }
    for (const unsigned char ch : name) {
        if (std::isalnum(ch) == 0 && ch != '-' && ch != '_' && ch != '.') {
            return std::unexpected("技能名只可用字母、数字、点、横线、下划线: " + raw_name);
        }
    }
    return name;
}

SkillHttpResponse FetchRemoteSkillUrl(const std::string& url) {
    const cpr::Response response = cpr::Get(cpr::Url{url}, cpr::Header{{"User-Agent", "lubancode/skill"}},
                                            cpr::Timeout{30000}, cpr::Redirect{});
    if (response.error) {
        return {0, {}, response.error.message};
    }
    return {static_cast<int>(response.status_code), response.text, {}};
}

std::expected<std::vector<RemoteSkillRecord>, std::string> LoadRemoteSkillRecords(const fs::path& skills_root) {
    const fs::path source_path = SourcesPath(skills_root);
    std::error_code ec;
    if (!fs::exists(source_path, ec)) {
        if (ec) {
            return std::unexpected("检查来源账本失败: " + ec.message());
        }
        return std::vector<RemoteSkillRecord>{};
    }
    std::ifstream file(source_path, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("打不开来源账本: " + PathToUtf8(source_path));
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return ParseRecords(buffer.str(), source_path);
}

std::expected<void, std::string> SaveRemoteSkillRecords(const fs::path& skills_root,
                                                         const std::vector<RemoteSkillRecord>& records) {
    if (const auto dir = EnsureDir(skills_root); !dir.has_value()) {
        return std::unexpected(dir.error());
    }
    nlohmann::json root;
    root["version"] = 1;
    root["skills"] = nlohmann::json::object();
    for (const RemoteSkillRecord& record : records) {
        const auto name = SanitizeSkillDirectoryName(record.name);
        if (!name.has_value() || record.source_url.empty() || record.installed_at.empty()) {
            return std::unexpected("不能写入不完整的来源账本条目");
        }
        root["skills"][*name] = {{"url", record.source_url}, {"installed_at", record.installed_at}};
    }

    const fs::path target = SourcesPath(skills_root);
    fs::path temporary = target;
    temporary += ".tmp";
    if (const auto wrote = WriteWholeFile(temporary, root.dump(2) + "\n"); !wrote.has_value()) {
        return std::unexpected(wrote.error());
    }
    std::error_code ec;
    fs::remove(target, ec);  // Windows 不肯 rename 覆盖已有文件,先挪开旧账
    if (ec) {
        fs::remove(temporary, ec);
        return std::unexpected("替换来源账本失败: " + ec.message());
    }
    fs::rename(temporary, target, ec);
    if (ec) {
        fs::remove(temporary, ec);
        return std::unexpected("写入来源账本失败: " + ec.message());
    }
    return {};
}

std::expected<std::vector<StoredSkill>, std::string> ListStoredSkills(const fs::path& skills_root) {
    const auto records = LoadRemoteSkillRecords(skills_root);
    if (!records.has_value()) {
        return std::unexpected(records.error());
    }
    std::map<std::string, RemoteSkillRecord> remote;
    for (const RemoteSkillRecord& record : *records) {
        remote.emplace(record.name, record);
    }

    std::vector<StoredSkill> out;
    std::error_code ec;
    if (!fs::exists(skills_root, ec)) {
        if (ec) {
            return std::unexpected("检查技能目录失败: " + ec.message());
        }
        return out;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(skills_root, ec)) {
        if (ec) {
            return std::unexpected("扫描技能目录失败: " + ec.message());
        }
        if (!entry.is_directory(ec) || ec) {
            ec.clear();
            continue;
        }
        const fs::path skill_md = entry.path() / "SKILL.md";
        if (!fs::is_regular_file(skill_md, ec) || ec) {
            ec.clear();
            continue;
        }
        const std::string name = PathToUtf8(entry.path().filename());
        const auto clean_name = SanitizeSkillDirectoryName(name);
        if (!clean_name.has_value()) {
            continue;
        }
        StoredSkill stored{*clean_name, PathToUtf8(entry.path()), std::nullopt, std::nullopt};
        if (const auto it = remote.find(stored.name); it != remote.end()) {
            stored.source_url = it->second.source_url;
            stored.installed_at = it->second.installed_at;
        }
        out.push_back(std::move(stored));
    }
    std::sort(out.begin(), out.end(),
              [](const StoredSkill& left, const StoredSkill& right) { return left.name < right.name; });
    return out;
}

std::expected<SkillInstallResult, std::string> InstallRemoteSkills(const fs::path& skills_root,
                                                                    const std::string& source_url,
                                                                    const SkillHttpGet& http_get,
                                                                    const SkillInstallOptions& options) {
    const auto source = ParseRemoteSkillSource(source_url);
    if (!source.has_value()) {
        return std::unexpected(source.error());
    }
    const auto downloaded = DownloadPackages(*source, http_get);
    if (!downloaded.has_value()) {
        return std::unexpected(downloaded.error());
    }

    std::set<std::string> requested;
    for (const std::string& raw_name : options.only_names) {
        const auto name = SanitizeSkillDirectoryName(raw_name);
        if (!name.has_value()) {
            return std::unexpected(name.error());
        }
        requested.insert(*name);
    }
    std::vector<RemotePackage> packages;
    for (const RemotePackage& package : *downloaded) {
        if (requested.empty() || requested.count(package.name) != 0) {
            packages.push_back(package);
        }
    }
    if (packages.empty()) {
        return std::unexpected("来源里没有要更新的技能");
    }
    if (!requested.empty() && packages.size() != requested.size()) {
        return std::unexpected("来源里找不到指定技能，未改动本地文件");
    }

    if (const auto dir = EnsureDir(skills_root); !dir.has_value()) {
        return std::unexpected(dir.error());
    }
    const auto loaded_records = LoadRemoteSkillRecords(skills_root);
    if (!loaded_records.has_value()) {
        return std::unexpected(loaded_records.error());
    }
    for (const RemotePackage& package : packages) {
        const fs::path target = skills_root / package.name;
        std::error_code ec;
        if (fs::exists(target, ec) && !options.overwrite) {
            return std::unexpected("技能已存在，先 /skill remove " + package.name + " 再装，或用 /skill update");
        }
        if (ec) {
            return std::unexpected("检查目标目录失败: " + ec.message());
        }
    }

    const fs::path staging = skills_root / NewStagingName();
    if (const auto dir = EnsureDir(staging); !dir.has_value()) {
        return std::unexpected(dir.error());
    }
    const auto clean_staging = [&]() {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
    };
    for (const RemotePackage& package : packages) {
        for (const RemoteFile& file : package.files) {
            const fs::path output = staging / package.name / Utf8ToPath(file.relative_path);
            if (const auto dir = EnsureDir(output.parent_path()); !dir.has_value()) {
                clean_staging();
                return std::unexpected(dir.error());
            }
            if (const auto wrote = WriteWholeFile(output, file.content); !wrote.has_value()) {
                clean_staging();
                return std::unexpected(wrote.error());
            }
        }
    }

    for (const RemotePackage& package : packages) {
        const fs::path target = skills_root / package.name;
        std::error_code ec;
        if (options.overwrite) {
            fs::remove_all(target, ec);
            if (ec) {
                clean_staging();
                return std::unexpected("替换旧技能失败: " + PathToUtf8(target) + ": " + ec.message());
            }
        }
        fs::rename(staging / package.name, target, ec);
        if (ec) {
            clean_staging();
            return std::unexpected("安装技能失败: " + PathToUtf8(target) + ": " + ec.message());
        }
    }
    clean_staging();

    std::map<std::string, RemoteSkillRecord> records;
    for (const RemoteSkillRecord& record : *loaded_records) {
        records[record.name] = record;
    }

    const std::string installed_at = options.installed_at.empty() ? CurrentUtcTimestamp() : options.installed_at;
    SkillInstallResult result;
    for (const RemotePackage& package : packages) {
        records[package.name] = RemoteSkillRecord{package.name, source_url, installed_at};
        result.installed_names.push_back(package.name);
    }
    std::sort(result.installed_names.begin(), result.installed_names.end());

    std::vector<RemoteSkillRecord> record_list;
    record_list.reserve(records.size());
    for (const auto& [name, record] : records) {
        (void)name;
        record_list.push_back(record);
    }
    if (const auto saved = SaveRemoteSkillRecords(skills_root, record_list); !saved.has_value()) {
        return std::unexpected(saved.error());
    }

    return result;
}

std::expected<SkillInstallResult, std::string> InstallLocalSkill(const fs::path& skills_root,
                                                                  const std::string& raw_source_path) {
    const std::string source_text = UnquotePath(raw_source_path);
    if (source_text.empty()) {
        return std::unexpected("本地技能路径不能为空");
    }

    std::error_code ec;
    fs::path source = fs::absolute(Utf8ToPath(source_text), ec);
    if (ec) {
        return std::unexpected("解析本地技能路径失败: " + ec.message());
    }
    source = source.lexically_normal();
    if (!fs::exists(source, ec) || ec) {
        return std::unexpected("本地技能不存在: " + PathToUtf8(source));
    }

    fs::path package_root;
    fs::path standalone_markdown;
    if (fs::is_directory(source, ec) && !ec) {
        package_root = source;
    } else if (fs::is_regular_file(source, ec) && !ec) {
        if (source.filename() == "SKILL.md") {
            package_root = source.parent_path();
        } else if (ToLower(PathToUtf8(source.extension())) == ".md") {
            standalone_markdown = source;
        } else {
            return std::unexpected("本地技能须是目录、SKILL.md 或独立 .md 文件: " + PathToUtf8(source));
        }
    } else {
        return std::unexpected("本地技能路径不是普通文件或目录: " + PathToUtf8(source));
    }

    const fs::path name_path = standalone_markdown.empty() ? package_root.filename() : standalone_markdown.stem();
    const auto name = SanitizeSkillDirectoryName(PathToUtf8(name_path));
    if (!name.has_value()) {
        return std::unexpected(name.error());
    }
    if (standalone_markdown.empty()) {
        const fs::path manifest = package_root / "SKILL.md";
        const fs::file_status manifest_status = fs::symlink_status(manifest, ec);
        if (ec || !fs::is_regular_file(manifest_status) || fs::is_symlink(manifest_status)) {
            return std::unexpected("技能目录根部没有普通文件 SKILL.md: " + PathToUtf8(package_root));
        }
    }

    if (const auto dir = EnsureDir(skills_root); !dir.has_value()) {
        return std::unexpected(dir.error());
    }
    const fs::path target = skills_root / *name;
    if (fs::exists(target, ec)) {
        if (ec) {
            return std::unexpected("检查目标目录失败: " + ec.message());
        }
        return std::unexpected("技能已存在，先 /skill remove " + *name + " 再装");
    }
    if (ec) {
        return std::unexpected("检查目标目录失败: " + ec.message());
    }

    const fs::path staging = skills_root / NewStagingName();
    const fs::path staged_package = staging / *name;
    if (const auto dir = EnsureDir(staged_package); !dir.has_value()) {
        return std::unexpected(dir.error());
    }
    const auto clean_staging = [&]() {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
    };
    const auto copy_regular_file = [&](const fs::path& from, const fs::path& to) -> std::expected<void, std::string> {
        const fs::file_status status = fs::symlink_status(from, ec);
        if (ec || fs::is_symlink(status) || !fs::is_regular_file(status)) {
            return std::unexpected("本地技能含符号链接或特殊文件，不予复制: " + PathToUtf8(from));
        }
        if (const auto dir = EnsureDir(to.parent_path()); !dir.has_value()) {
            return std::unexpected(dir.error());
        }
        fs::copy_file(from, to, fs::copy_options::none, ec);
        if (ec) {
            return std::unexpected("复制技能文件失败: " + PathToUtf8(from) + ": " + ec.message());
        }
        return {};
    };

    if (!standalone_markdown.empty()) {
        if (const auto copied = copy_regular_file(standalone_markdown, staged_package / "SKILL.md");
            !copied.has_value()) {
            clean_staging();
            return std::unexpected(copied.error());
        }
    } else {
        fs::recursive_directory_iterator it(package_root, fs::directory_options::none, ec);
        const fs::recursive_directory_iterator end;
        if (ec) {
            clean_staging();
            return std::unexpected("扫描本地技能失败: " + ec.message());
        }
        for (; it != end; it.increment(ec)) {
            if (ec) {
                clean_staging();
                return std::unexpected("扫描本地技能失败: " + ec.message());
            }
            const fs::file_status status = it->symlink_status(ec);
            if (ec || fs::is_symlink(status)) {
                clean_staging();
                return std::unexpected("本地技能含符号链接，不予复制: " + PathToUtf8(it->path()));
            }
            const fs::path relative = it->path().lexically_relative(package_root);
            const fs::path output = staged_package / relative;
            if (fs::is_directory(status)) {
                if (const auto dir = EnsureDir(output); !dir.has_value()) {
                    clean_staging();
                    return std::unexpected(dir.error());
                }
            } else if (fs::is_regular_file(status)) {
                if (const auto copied = copy_regular_file(it->path(), output); !copied.has_value()) {
                    clean_staging();
                    return std::unexpected(copied.error());
                }
            } else {
                clean_staging();
                return std::unexpected("本地技能含特殊文件，不予复制: " + PathToUtf8(it->path()));
            }
        }
    }

    fs::rename(staged_package, target, ec);
    if (ec) {
        clean_staging();
        return std::unexpected("安装本地技能失败: " + PathToUtf8(target) + ": " + ec.message());
    }
    clean_staging();
    return SkillInstallResult{{*name}};
}

std::expected<SkillInstallResult, std::string> InstallSkillSource(const fs::path& skills_root,
                                                                  const std::string& source,
                                                                  const SkillHttpGet& http_get) {
    const std::string clean_source = UnquotePath(source);
    if (IsHttpUrl(clean_source)) {
        return InstallRemoteSkills(skills_root, clean_source, http_get);
    }
    return InstallLocalSkill(skills_root, clean_source);
}

std::expected<void, std::string> RemoveStoredSkill(const fs::path& skills_root, const std::string& name) {
    const auto clean_name = SanitizeSkillDirectoryName(name);
    if (!clean_name.has_value()) {
        return std::unexpected(clean_name.error());
    }
    const fs::path target = skills_root / *clean_name;
    std::error_code ec;
    const bool exists = fs::exists(target, ec);
    if (ec) {
        return std::unexpected("检查技能目录失败: " + ec.message());
    }
    if (!exists) {
        return std::unexpected("技能不存在: " + *clean_name);
    }
    fs::remove_all(target, ec);
    if (ec) {
        return std::unexpected("删除技能失败: " + PathToUtf8(target) + ": " + ec.message());
    }

    const auto loaded_records = LoadRemoteSkillRecords(skills_root);
    if (!loaded_records.has_value()) {
        return std::unexpected(loaded_records.error());
    }
    std::vector<RemoteSkillRecord> remaining;
    remaining.reserve(loaded_records->size());
    for (const RemoteSkillRecord& record : *loaded_records) {
        if (record.name != *clean_name) {
            remaining.push_back(record);
        }
    }
    if (remaining.size() != loaded_records->size()) {
        if (const auto saved = SaveRemoteSkillRecords(skills_root, remaining); !saved.has_value()) {
            return std::unexpected(saved.error());
        }
    }
    return {};
}

}  // namespace lubancode::config
