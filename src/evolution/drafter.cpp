// EvolutionDrafter 的实现(自进化闭环阶段 2)。全文纯函数:输入一场录制,
// 输出两份文本。密钥这道门由录制件本身把守(入盘前已过 SanitizeToolInput/
// RedactSecrets),这里再过一遍 skills::RedactSecrets 兜底。
#include "evolution/drafter.hpp"

#include "evolution/observation.hpp"
#include "package/manifest.hpp"
#include "skills/skill_drafter.hpp"

namespace lubancode::evolution {

namespace {

// 单行化 + 压空白(截长另算)。package.yaml 的标量值与摘要行共用。
std::string OneLine(std::string text) {
    std::string out;
    out.reserve(text.size());
    bool last_space = true;
    for (const char c : text) {
        const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (space) {
            if (!last_space) {
                out.push_back(' ');
                last_space = true;
            }
        } else {
            out.push_back(c);
            last_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

// UTF-8 字符数。
std::size_t CharCount(const std::string& text) {
    std::size_t count = 0;
    for (const unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::string TruncateChars(std::string text, std::size_t max_chars) {
    if (CharCount(text) <= max_chars) {
        return text;
    }
    std::size_t bytes = 0;
    std::size_t chars = 0;
    while (bytes < text.size() && chars < max_chars) {
        const unsigned char lead = static_cast<unsigned char>(text[bytes]);
        std::size_t len = 1;
        if ((lead & 0xE0) == 0xC0) len = 2;
        else if ((lead & 0xF0) == 0xE0) len = 3;
        else if ((lead & 0xF8) == 0xF0) len = 4;
        if (bytes + len > text.size()) break;
        bytes += len;
        ++chars;
    }
    return text.substr(0, bytes) + "…";
}

// 标量值洗成 YAML plain scalar 放得下的样子:':' 换全角(块语境里 ": " 会
// 撞映射键)、'#' 换全角(注释起头)、控制字符丢掉;首字符是 YAML 指示符
// ('-' '?' 引号 '&' '*' 等)的剥掉头部。中段的引号、花括号、方括号、逗号
// 在块语境 plain scalar 里都合法——{{date}} 这类占位符原样保留。
std::string SanitizeYamlScalar(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (c < 0x20 || c == 0x7F) {
            continue;  // 控制字符
        }
        if (raw == ':') {
            out += "：";
        } else if (raw == '#') {
            out += "＃";
        } else {
            out.push_back(raw);
        }
    }
    const std::string kLeading = "-?:\"'&*!|>%@`{},[] ";
    while (!out.empty() && kLeading.find(out.front()) != std::string::npos) {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

// 从事件流里取目标口述(record_start 先说,goal 事件后说盖先说)——与
// skill_drafter::CollectFacts 同一口径,这里只为 evolution.json 的 objective。
std::string CollectGoal(const std::vector<skills::RecordEvent>& events) {
    std::string goal;
    for (const skills::RecordEvent& event : events) {
        const char* key = nullptr;
        if (event.type == skills::kEventRecordStart) {
            key = "goal";
        } else if (event.type == skills::kEventGoal) {
            key = "text";
        }
        if (key == nullptr) {
            continue;
        }
        const auto it = event.data.find(key);
        if (it != event.data.end() && it->is_string() && !it->get<std::string>().empty()) {
            goal = it->get<std::string>();
        }
    }
    return goal;
}

std::string ComposePackageYaml(const std::string& package_id, const std::string& display_name,
                               const std::string& description) {
    std::string out;
    out += "schema: 1\n";
    out += "id: " + package_id + "\n";
    out += "version: 0.1.0\n";
    out += "name: " + display_name + "\n";
    out += "description: " + description + "\n";
    return out;
}

// 末尾补一节排错:稳定失败路(连败无成功的步子)留在纸面上,
// 换项目踩到同一个坑才知道先换路。没有就不写这节,不硬凑。
std::string AppendTroubleshooting(std::string content, const std::vector<skills::RecordEvent>& events) {
    const std::vector<skills::DraftFailureMode> modes = skills::CollectStableFailureModes(events);
    if (modes.empty()) {
        return content;
    }
    if (!content.empty() && content.back() != '\n') {
        content.push_back('\n');
    }
    content += "\n## 排错\n\n";
    content += "录制中这些走法始终没走通(连败无成功),遇到同样情形先换路:\n";
    for (const skills::DraftFailureMode& mode : modes) {
        std::string line = "- " + mode.tool + ": ";
        line += mode.summary.empty() ? "(无失败摘要)" : TruncateChars(mode.summary, 80);
        content += line + "\n";
    }
    return content;
}

}  // namespace

std::expected<SkillCandidateDraft, std::string> DraftSkillCandidate(const skills::RecordingStatus& status,
                                                                    const std::vector<skills::RecordEvent>& events) {
    if (events.empty()) {
        return std::unexpected("录制事件是空的,起不出候选");
    }
    bool finished = false;
    for (const skills::RecordEvent& event : events) {
        if (event.type == skills::kEventRecordStop) {
            finished = true;
            break;
        }
    }
    if (!finished) {
        return std::unexpected("录制件没录完(缺 record_stop),半截示范起不出候选");
    }

    // ---- SKILL.md:复用现有起草器,补排错节 ----
    std::string skill_markdown = skills::ComposeSkillMarkdown(events);
    auto skill_name = skills::ValidateSkillMarkdownForInstall(skill_markdown);
    if (!skill_name.has_value()) {
        skill_markdown = skills::RepairSkillFrontmatter(skill_markdown);
        skill_name = skills::ValidateSkillMarkdownForInstall(skill_markdown);
        if (!skill_name.has_value()) {
            return std::unexpected("SKILL 草稿回炉后仍过不了解析器: " + skill_name.error());
        }
    }
    skill_markdown = AppendTroubleshooting(std::move(skill_markdown), events);
    if (!skills::ValidateSkillMarkdownForInstall(skill_markdown).has_value()) {
        return std::unexpected("补排错节后 SKILL 草稿过不了解析器,不予落盘");
    }

    // ---- 身份 ----
    const std::string slug = *skill_name;
    const std::string package_id = "evolve." + slug;
    if (!package::IsValidPackageId(package_id)) {
        return std::unexpected("包 id 不合清单规矩: " + package_id);
    }

    // ---- package.yaml:最小五字段,先写再验,验不过就换素净文案 ----
    // 描述走一遍偶然值抽象(日期/网址/绝对路径 → {{date}}/{{url}}/{{path}}),
    // 清单里不焊死演示现场的值;cwd 从 record_start 取,现场内的路径剥成相对。
    const std::string goal = CollectGoal(events);
    std::string cwd;
    for (const skills::RecordEvent& event : events) {
        if (event.type == skills::kEventRecordStart) {
            const auto it = event.data.find("cwd");
            if (it != event.data.end() && it->is_string()) {
                cwd = it->get<std::string>();
            }
        }
    }
    std::vector<skills::DraftVariable> abstract_vars;
    const std::string abstract_goal = skills::AbstractConcreteValues(OneLine(goal), cwd, abstract_vars);
    std::string display_name = TruncateChars(OneLine(status.name), 60);
    std::string description = TruncateChars(abstract_goal, 100);
    std::string yaml = ComposePackageYaml(package_id, SanitizeYamlScalar(display_name.empty() ? slug : display_name),
                                          SanitizeYamlScalar(description.empty() ? "由录制生成的候选技能包"
                                                                                  : description));
    if (!package::ParsePackageManifest(yaml).has_value()) {
        yaml = ComposePackageYaml(package_id, slug, "由录制生成的候选技能包");
        if (!package::ParsePackageManifest(yaml).has_value()) {
            return std::unexpected("package.yaml 最小形态过不了严格解析,不予落盘");
        }
    }

    SkillCandidateDraft draft;
    draft.skill_slug = slug;
    draft.skill_markdown = std::move(skill_markdown);
    draft.package_yaml = std::move(yaml);
    draft.package_id = package_id;
    draft.package_version = "0.1.0";
    draft.objective = SanitizeObservationText(goal, 200);
    if (draft.objective.empty()) {
        draft.objective = "把录制 \"" + TruncateChars(OneLine(status.name), 60) + "\" 沉淀成可复用技能";
    }
    return draft;
}

}  // namespace lubancode::evolution
