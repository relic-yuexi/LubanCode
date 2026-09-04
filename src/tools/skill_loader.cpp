#include "tools/skill_loader.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>

#include <yaml-cpp/yaml.h>

#include "tools/path_utils.hpp"
#include "platform/log_sink.hpp"

namespace lubancode::tools {

namespace {

constexpr std::string_view kManagedOfficialMarker =
    "<!-- lubancode 系统维护,随版本自动更新;自定义请另建技能 -->";

// 0.20.x 只发过这一份无管理标记的内置配置技能。按完整字节指纹认旧件，
// 不拿一句 description 猜，免得把用户碰巧同名的技能当成旧副本。
constexpr std::size_t kLegacyConfigSkillBytes = 10482;
constexpr std::uint64_t kLegacyConfigSkillFnv1a = 0xAD1BC701FAB01B9FULL;

std::uint64_t Fnv1a(const std::string& content) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : content) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool IsManagedOfficialCopy(const std::string& content, const ParsedSkillFile& parsed) {
    if (content.find(kManagedOfficialMarker) != std::string::npos) {
        return true;
    }
    return parsed.name == "lubancode-config" && content.size() == kLegacyConfigSkillBytes &&
           Fnv1a(content) == kLegacyConfigSkillFnv1a;
}

std::string Trim(const std::string& s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string StripQuotes(const std::string& s) {
    if (s.size() >= 2) {
        const char front = s.front();
        const char back = s.back();
        if ((front == '"' && back == '"') || (front == '\'' && back == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

std::string TrimTrailingCr(const std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        return line.substr(0, line.size() - 1);
    }
    return line;
}

std::optional<ParsedSkillFile> ParseLooseFrontmatter(const std::string& frontmatter, std::string body) {
    ParsedSkillFile result;
    result.body = std::move(body);
    std::istringstream iss(frontmatter);
    std::string line;
    while (std::getline(iss, line)) {
        line = TrimTrailingCr(line);
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key = Trim(line.substr(0, colon));
        const std::string value = StripQuotes(Trim(line.substr(colon + 1)));
        if (key == "name") {
            result.name = value;
        } else if (key == "description") {
            result.description = value;
        }
    }
    if (!result.name.has_value() && !result.description.has_value()) {
        return std::nullopt;
    }
    return result;
}

void WarnCollision(const SkillMeta& previous, const SkillMeta& replacement) {
    platform::LogSink::Instance().Warn(
        "skills", "同名技能 " + replacement.name + " 冲突，采用 " + replacement.dir_path + "，遮住 " +
                      previous.dir_path);
}

}  // namespace

bool IsValidAgentSkillName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name.front() == '-' || name.back() == '-' ||
        name.find("--") != std::string::npos) {
        return false;
    }
    for (const unsigned char ch : name) {
        if ((ch < 'a' || ch > 'z') && (ch < '0' || ch > '9') && ch != '-') {
            return false;
        }
    }
    return true;
}

namespace {

std::size_t Utf8CharacterCount(const std::string& text) {
    std::size_t count = 0;
    for (const unsigned char ch : text) {
        if ((ch & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

}  // namespace

std::optional<ParsedSkillFile> ParseSkillMarkdown(const std::string& content) {
    ParsedSkillFile result;

    // 内容压根不是以 "---" 起头的一行,视为"没有 frontmatter",body 就是
    // 整篇原文,不算错。
    if (content.rfind("---", 0) != 0) {
        result.body = content;
        return result;
    }

    const std::size_t first_line_end = content.find('\n');
    const std::string first_line =
        TrimTrailingCr((first_line_end == std::string::npos) ? content : content.substr(0, first_line_end));
    if (first_line != "---") {
        // 第一行长得像 "---xxx" 但不是单独一个 "---",不算 frontmatter 定界符。
        result.body = content;
        return result;
    }

    std::size_t pos = (first_line_end == std::string::npos) ? content.size() : first_line_end + 1;
    const std::size_t frontmatter_begin = pos;
    std::size_t close_begin = std::string::npos;
    std::size_t close_line_end = std::string::npos;

    while (pos <= content.size()) {
        const std::size_t line_end = content.find('\n', pos);
        const std::string raw_line = (line_end == std::string::npos) ? content.substr(pos) : content.substr(pos, line_end - pos);
        if (TrimTrailingCr(raw_line) == "---") {
            close_begin = pos;
            close_line_end = line_end;
            break;
        }
        if (line_end == std::string::npos) {
            break;
        }
        pos = line_end + 1;
    }

    if (close_begin == std::string::npos) {
        // 起了 --- 头,却找不到闭合的 ---:frontmatter 损坏,调用方该跳过
        // 整个技能。
        return std::nullopt;
    }

    const std::string frontmatter = content.substr(frontmatter_begin, close_begin - frontmatter_begin);
    result.body = (close_line_end == std::string::npos) ? std::string() : content.substr(close_line_end + 1);

    try {
        const YAML::Node root = YAML::Load(frontmatter);
        if (!root.IsMap()) {
            return std::nullopt;
        }
        const auto read_string = [&](const char* key) -> std::optional<std::string> {
            const YAML::Node value = root[key];
            if (!value || !value.IsScalar()) {
                return std::nullopt;
            }
            return value.as<std::string>();
        };
        result.name = read_string("name");
        result.description = read_string("description");
        return result;
    } catch (const YAML::Exception&) {
        // 接入指南特意建议宽容这类旧件。只回退顶层 name/description，
        // 不拿这条小路冒充完整 YAML 解析器。
        return ParseLooseFrontmatter(frontmatter, std::move(result.body));
    }
}

// 裸扫描的实现体:package_id 非空 = 包内挂载扫描(阶段 3)——名字折成
// canonical("<包id>:<名>")、记 package_id、跳过裸名规范与名不符警告
//(local id 的命名规矩归 package 层的盘点与 doctor,loader 不重复报)。
std::vector<SkillMeta> ScanSkillsDirImpl(const std::filesystem::path& skills_root, const std::string& source_level,
                                         const std::string* package_id) {
    std::vector<SkillMeta> metas;

    std::error_code ec;
    if (!std::filesystem::exists(skills_root, ec) || ec || !std::filesystem::is_directory(skills_root, ec)) {
        return metas;
    }

    std::error_code iter_ec;
    for (const auto& entry : std::filesystem::directory_iterator(skills_root, iter_ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::filesystem::path skill_md = entry.path() / "SKILL.md";
        std::error_code exists_ec;
        if (!std::filesystem::exists(skill_md, exists_ec) || exists_ec) {
            continue;  // 目录下没有 SKILL.md,不算一份技能,悄悄跳过
        }

        std::ifstream file(skill_md, std::ios::binary);
        if (!file.is_open()) {
            platform::LogSink::Instance().Warn("skills", "打不开 " + PathToUtf8(skill_md) + ",跳过");
            continue;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const std::string content = buffer.str();

        const auto parsed = ParseSkillMarkdown(content);
        if (!parsed.has_value()) {
            platform::LogSink::Instance().Warn(
                "skills", PathToUtf8(skill_md) + " 的 frontmatter 损坏(没有闭合的 ---),跳过");
            continue;
        }

        const std::string dir_name = PathToUtf8(entry.path().filename());
        if (!parsed->name.has_value() || parsed->name->empty()) {
            platform::LogSink::Instance().Warn("skills", PathToUtf8(skill_md) + " 缺必填 name，跳过");
            continue;
        }
        if (!parsed->description.has_value() || Trim(*parsed->description).empty()) {
            platform::LogSink::Instance().Warn("skills", PathToUtf8(skill_md) + " 缺必填 description，跳过");
            continue;
        }
        if (package_id == nullptr) {
            // 裸目录的老规矩:名字规范与目录一致性都只警告、照加载。包内扫描
            // 不走这两条——canonical 名带点带冒号,本就不合裸名规范;目录名
            // 规矩归 package 层盘点。
            if (!IsValidAgentSkillName(*parsed->name)) {
                platform::LogSink::Instance().Warn(
                    "skills", PathToUtf8(skill_md) + " 的 name 不合 Agent Skills 命名规范，仍按兼容模式加载");
            }
            if (*parsed->name != dir_name) {
                platform::LogSink::Instance().Warn(
                    "skills", PathToUtf8(skill_md) + " 的 name 与父目录名不一致，仍按 frontmatter 名加载");
            }
        }
        if (Utf8CharacterCount(*parsed->description) > 1024) {
            platform::LogSink::Instance().Warn(
                "skills", PathToUtf8(skill_md) + " 的 description 超过 1024 字符，仍按兼容模式加载");
        }
        SkillMeta meta;
        meta.name = package_id != nullptr ? (*package_id + ":" + *parsed->name) : *parsed->name;
        meta.description = Trim(*parsed->description);
        meta.dir_path = PathToUtf8(entry.path());
        meta.source_level = source_level;
        meta.managed_official_copy = IsManagedOfficialCopy(content, *parsed);
        meta.package_id = package_id != nullptr ? *package_id : std::string();
        metas.push_back(std::move(meta));
    }

    return metas;
}

std::vector<SkillMeta> ScanSkillsDir(const std::filesystem::path& skills_root, const std::string& source_level) {
    return ScanSkillsDirImpl(skills_root, source_level, /*package_id=*/nullptr);
}

std::vector<SkillMeta> ScanPackagedSkillsDir(const PackagedSkillRoot& root) {
    return ScanSkillsDirImpl(root.skills_dir, root.source_level, &root.package_id);
}

std::vector<SkillMeta> LoadSkills(const std::string& project_dir, const std::optional<std::string>& home_dir,
                                  const std::optional<std::string>& official_skills_dir,
                                  const std::vector<PackagedSkillRoot>& package_roots,
                                  bool report_collisions) {
    std::map<std::string, SkillMeta> merged;  // std::map 天然按 key 排序,输出顺序稳定

    const auto merge = [&](std::vector<SkillMeta> incoming) {
        for (auto& meta : incoming) {
            if (const auto previous = merged.find(meta.name); previous != merged.end()) {
                if (report_collisions) {
                    WarnCollision(previous->second, meta);
                }
            }
            merged[meta.name] = std::move(meta);
        }
    };

    if (official_skills_dir.has_value()) {
        merge(ScanSkillsDir(Utf8ToPath(*official_skills_dir), "官方"));
    }

    if (home_dir.has_value()) {
        const std::filesystem::path home_agents_root = Utf8ToPath(*home_dir) / ".agents" / "skills";
        merge(ScanSkillsDir(home_agents_root, "主目录级"));

        const std::filesystem::path home_skills_root = Utf8ToPath(*home_dir) / ".lubancode" / "skills";
        for (auto& meta : ScanSkillsDir(home_skills_root, "主目录级")) {
            if (meta.managed_official_copy && merged.contains(meta.name)) {
                continue;
            }
            if (const auto previous = merged.find(meta.name); previous != merged.end()) {
                if (report_collisions) {
                    WarnCollision(previous->second, meta);
                }
            }
            merged[meta.name] = std::move(meta);
        }
    }

    const std::filesystem::path project_root = Utf8ToPath(project_dir);
    merge(ScanSkillsDir(project_root / ".agents" / "skills", "项目级"));
    merge(ScanSkillsDir(project_root / ".lubancode" / "skills", "项目级"));

    // 包内技能(统一 Package 封装单阶段 3)最后并入:canonical 名带点带
    // 冒号,与裸名两套命名空间不相交(契约 packages.md §6),并入只添行
    // 不遮行。
    for (const PackagedSkillRoot& root : package_roots) {
        merge(ScanPackagedSkillsDir(root));
    }

    std::vector<SkillMeta> out;
    out.reserve(merged.size());
    for (auto& [name, meta] : merged) {
        (void)name;
        out.push_back(std::move(meta));
    }
    return out;
}

std::vector<SkillLayerEntry> EnumerateSkillLayers(const std::string& project_dir,
                                                  const std::optional<std::string>& home_dir,
                                                  const std::optional<std::string>& official_skills_dir) {
    std::vector<SkillLayerEntry> out;
    std::map<std::string, std::size_t> winner;  // 技能名 -> out 里现行胜者的下标

    // 与 LoadSkills 的合并逐条对齐:后到的同名顶掉先到的(先把旧胜者翻成
    // 被遮蔽)。唯一分岔是 <主目录>/.lubancode 里旧版播种的官方维护副本——
    // 遇已有胜者就让位、不顶替,与 LoadSkills 的"让位"分支同款。
    const auto absorb = [&](std::vector<SkillMeta> incoming, bool managed_copies_yield) {
        for (auto& meta : incoming) {
            SkillLayerEntry entry;
            entry.meta = meta;
            const auto previous = winner.find(meta.name);
            if (managed_copies_yield && meta.managed_official_copy && previous != winner.end()) {
                entry.active = false;
                entry.shadowed_by = out[previous->second].meta.source_level;
            } else {
                if (previous != winner.end()) {
                    out[previous->second].active = false;
                    out[previous->second].shadowed_by = meta.source_level;
                }
                winner[meta.name] = out.size();
            }
            out.push_back(std::move(entry));
        }
    };

    if (official_skills_dir.has_value()) {
        absorb(ScanSkillsDir(Utf8ToPath(*official_skills_dir), "官方"), false);
    }
    if (home_dir.has_value()) {
        const std::filesystem::path home = Utf8ToPath(*home_dir);
        absorb(ScanSkillsDir(home / ".agents" / "skills", "agents 共享"), false);
        absorb(ScanSkillsDir(home / ".lubancode" / "skills", "主目录级"), true);
    }
    const std::filesystem::path project_root = Utf8ToPath(project_dir);
    absorb(ScanSkillsDir(project_root / ".agents" / "skills", "agents 共享"), false);
    absorb(ScanSkillsDir(project_root / ".lubancode" / "skills", "项目级"), false);
    return out;
}

std::string BuildSkillsPromptSegment(const std::vector<SkillMeta>& skills) {
    if (skills.empty()) {
        return std::string();
    }
    std::string out =
        "技能目录约定:LubanCode 扫发行包官方 skills、~/.agents/skills、~/.lubancode/skills、"
        "<cwd>/.agents/skills 与 <cwd>/.lubancode/skills。跨客户端共享技能可放 .agents/skills；"
        "/skill install 默认装进 ~/.lubancode/skills。\n"
        "可用技能(用 skill 工具按名加载):\n";
    for (const auto& meta : skills) {
        out += "- " + meta.name + ": " + meta.description + "\n";
    }
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

std::optional<std::string> ReadSkillBody(const SkillMeta& meta) {
    const std::filesystem::path skill_md = Utf8ToPath(meta.dir_path) / "SKILL.md";
    std::error_code ec;
    if (!std::filesystem::exists(skill_md, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream file(skill_md, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();
    const auto parsed = ParseSkillMarkdown(content);
    return parsed.has_value() ? parsed->body : content;
}

}  // namespace lubancode::tools
