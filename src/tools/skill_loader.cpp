#include "tools/skill_loader.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>

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

    std::istringstream iss(frontmatter);
    std::string line;
    while (std::getline(iss, line)) {
        line = TrimTrailingCr(line);
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;  // 不是 "key: value" 形状的行,直接跳过,不当错误
        }
        const std::string key = Trim(line.substr(0, colon));
        const std::string value = StripQuotes(Trim(line.substr(colon + 1)));
        if (key == "name") {
            result.name = value;
        } else if (key == "description") {
            result.description = value;
        }
    }

    return result;
}

std::vector<SkillMeta> ScanSkillsDir(const std::filesystem::path& skills_root, const std::string& source_level) {
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
        SkillMeta meta;
        meta.name = parsed->name.value_or(dir_name);
        meta.description = parsed->description.value_or(std::string());
        meta.dir_path = PathToUtf8(entry.path());
        meta.source_level = source_level;
        meta.managed_official_copy = IsManagedOfficialCopy(content, *parsed);
        metas.push_back(std::move(meta));
    }

    return metas;
}

std::vector<SkillMeta> LoadSkills(const std::string& project_dir, const std::optional<std::string>& home_dir,
                                  const std::optional<std::string>& official_skills_dir) {
    std::map<std::string, SkillMeta> merged;  // std::map 天然按 key 排序,输出顺序稳定

    if (official_skills_dir.has_value()) {
        for (auto& meta : ScanSkillsDir(Utf8ToPath(*official_skills_dir), "官方")) {
            merged[meta.name] = std::move(meta);
        }
    }

    if (home_dir.has_value()) {
        const std::filesystem::path home_skills_root = Utf8ToPath(*home_dir) / ".lubancode" / "skills";
        for (auto& meta : ScanSkillsDir(home_skills_root, "主目录级")) {
            if (meta.managed_official_copy && merged.contains(meta.name)) {
                continue;
            }
            merged[meta.name] = std::move(meta);
        }
    }

    const std::filesystem::path project_skills_root = Utf8ToPath(project_dir) / ".lubancode" / "skills";
    for (auto& meta : ScanSkillsDir(project_skills_root, "项目级")) {
        merged[meta.name] = std::move(meta);  // 同名:项目级覆盖主目录级
    }

    std::vector<SkillMeta> out;
    out.reserve(merged.size());
    for (auto& [name, meta] : merged) {
        (void)name;
        out.push_back(std::move(meta));
    }
    return out;
}

std::string BuildSkillsPromptSegment(const std::vector<SkillMeta>& skills) {
    if (skills.empty()) {
        return std::string();
    }
    std::string out =
        "技能目录铁则:LubanCode 扫发行包官方 skills、~/.lubancode/skills/<技能名>/SKILL.md 与 "
        "<cwd>/.lubancode/skills/<技能名>/SKILL.md;绝不可把给 LubanCode 的技能装进 "
        ".codex/skills、.claude/skills 或 .agents/skills。本机来源可用 /skill install <目录或 SKILL.md> "
        "装进用户级目录。\n"
        "可用技能(用 skill 工具按名加载):\n";
    for (const auto& meta : skills) {
        out += "- " + meta.name + ": " + meta.description + "\n";
    }
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

}  // namespace lubancode::tools
