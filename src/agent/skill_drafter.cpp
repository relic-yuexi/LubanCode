#include "agent/skill_drafter.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>
#include <utility>

#include "tools/skill_loader.hpp"

namespace lubancode::agent {

namespace {

namespace fs = std::filesystem;

std::string PathToUtf8(const fs::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

bool IsAsciiAlnum(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

// frontmatter 里的技能名:只留 ASCII 字母数字与 '-' '_' '.',其余(含中文、
// 空白)换 '-',连续并一、首尾剥。全没了给 "recorded-skill"。这是安装层
// SanitizeSkillDirectoryName 认的字汇,起草时就对齐,免得装的时候再撞墙。
std::string MakeSkillNameSlug(const std::string& name) {
    std::string out;
    bool last_dash = false;
    for (const char c : name) {
        if (IsAsciiAlnum(c) || c == '-' || c == '_' || c == '.') {
            out.push_back(c);
            last_dash = false;
        } else if (!last_dash && !out.empty()) {
            out.push_back('-');
            last_dash = true;
        }
    }
    while (!out.empty() && (out.front() == '-' || out.front() == '.')) {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    if (out.empty() || out == "." || out == "..") {
        return "recorded-skill";
    }
    return out;
}

// 单行化 + 截断,给 frontmatter 的 description 用:换行换空格,压掉连续
// 空白,剥首尾;超长掐到 max_chars。
std::string SingleLine(std::string text, std::size_t max_chars) {
    std::string out;
    out.reserve(text.size());
    bool last_space = true;  // 顺手剥掉首部空白
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
    if (out.size() > max_chars) {
        out.resize(max_chars);
        // 别掐在多字节字符中间:把残余的 UTF-8 序列(续字节连同引导字节)
        // 整段退掉再截。
        while (!out.empty()) {
            const unsigned char tail = static_cast<unsigned char>(out.back());
            if ((tail & 0xC0) == 0x80 || tail >= 0xC0) {
                out.pop_back();
            } else {
                break;
            }
        }
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
        out += "…";
    }
    return out;
}

// ---------------------------------------------------------------------------
// 具体值抽象
// ---------------------------------------------------------------------------

// 路径字符:路径游程里允许出现的字符(空白/引号/常见括号标点收尾)。
bool IsPathChar(char c) {
    switch (c) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case '"':
        case '\'':
        case '`':
        case ',':
        case ';':
        case ')':
        case ']':
        case '}':
        case '{':
            return false;
        default:
            return true;
    }
}

std::size_t PathRunEnd(const std::string& text, std::size_t begin) {
    std::size_t end = begin;
    while (end < text.size() && IsPathChar(text[end])) {
        ++end;
    }
    return end;
}

bool IEqualPrefix(const std::string& text, std::size_t at, const std::string& prefix) {
    if (at + prefix.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[at + i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

// 记一个新变量(去重):同名占位只留首例的 example。
void AddVariable(std::vector<DraftVariable>& extracted, const std::string& name,
                 const std::string& description, const std::string& example) {
    for (const DraftVariable& existing : extracted) {
        if (existing.name == name) {
            return;
        }
    }
    extracted.push_back({name, description, example});
}

bool IsDateAt(const std::string& text, std::size_t at) {
    if (at + 10 > text.size()) {
        return false;
    }
    for (std::size_t k = 0; k < 10; ++k) {
        const char c = text[at + k];
        const bool digit = std::isdigit(static_cast<unsigned char>(c)) != 0;
        const bool dash = c == '-';
        if (k == 4 || k == 7) {
            if (!dash) return false;
        } else if (!digit) {
            return false;
        }
        // 月份/日期粗校验,别把 2026-88-88 也当日期
        const int month = (text[at + 5] - '0') * 10 + (text[at + 6] - '0');
        const int day = (text[at + 8] - '0') * 10 + (text[at + 9] - '0');
        if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    }
    // 边界:前后紧贴字母数字或'.'的,多半是别的编号/文件名,不动;
    // '-'/'_'/'/' 这类分隔符前后的日期(如 out-2026-08-14)照提。
    const auto is_boundary = [](char c) { return IsAsciiAlnum(c) || c == '.'; };
    if (at > 0 && is_boundary(text[at - 1])) {
        return false;
    }
    if (at + 10 < text.size() && is_boundary(text[at + 10])) {
        return false;
    }
    return true;
}

}  // namespace

std::string AbstractConcreteValues(const std::string& text, const std::string& cwd,
                                   std::vector<DraftVariable>& extracted) {
    if (text.empty()) {
        return text;
    }
    // cwd 归一:剥尾部分隔符,小写比较用。
    std::string cwd_prefix = cwd;
    while (!cwd_prefix.empty() && (cwd_prefix.back() == '/' || cwd_prefix.back() == '\\')) {
        cwd_prefix.pop_back();
    }

    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];

        // 日期 → {{date}}
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 && IsDateAt(text, i)) {
            const std::string example = text.substr(i, 10);
            out += "{{date}}";
            AddVariable(extracted, "{{date}}", "演示中的日期,按本次运行日期替换", example);
            i += 10;
            continue;
        }

        // 网址 → {{url}}
        if (IEqualPrefix(text, i, "http://") || IEqualPrefix(text, i, "https://")) {
            std::size_t end = i;
            while (end < text.size() && IsPathChar(text[end])) {
                ++end;
            }
            AddVariable(extracted, "{{url}}", "演示中的网址,按本次目标地址替换", text.substr(i, end - i));
            out += "{{url}}";
            i = end;
            continue;
        }

        // Windows 盘符绝对路径:D:\... 或 D:/...
        if (IsAsciiAlnum(c) && i + 2 < text.size() && text[i + 1] == ':' &&
            (text[i + 2] == '\\' || text[i + 2] == '/')) {
            const std::size_t end = PathRunEnd(text, i);
            const std::string path = text.substr(i, end - i);
            if (!cwd_prefix.empty() && IEqualPrefix(path, 0, cwd_prefix) &&
                path.size() > cwd_prefix.size()) {
                // cwd 打头:剥掉前缀,留相对部分(相对路径是稳定输入,可以照写);
                // 相对部分里夹着的日期/网址再递归提一遍(cwd 传空,免得二级剥皮)。
                std::string relative = path.substr(cwd_prefix.size());
                while (!relative.empty() && (relative.front() == '/' || relative.front() == '\\')) {
                    relative.erase(relative.begin());
                }
                if (!relative.empty()) {
                    out += AbstractConcreteValues(relative, std::string(), extracted);
                    i = end;
                    continue;
                }
            }
            AddVariable(extracted, "{{path}}", "演示中的绝对路径,按本次环境替换", path);
            out += "{{path}}";
            i = end;
            continue;
        }

        // POSIX 绝对路径:/xxx/...,至少还有一级才算
        if (c == '/' && i + 1 < text.size() &&
            (IsAsciiAlnum(text[i + 1]) || text[i + 1] == '_' || text[i + 1] == '.')) {
            const std::size_t end = PathRunEnd(text, i);
            const std::string path = text.substr(i, end - i);
            if (path.find('/', 1) != std::string::npos) {
                if (!cwd_prefix.empty() && IEqualPrefix(path, 0, cwd_prefix) && path.size() > cwd_prefix.size()) {
                    std::string relative = path.substr(cwd_prefix.size());
                    while (!relative.empty() && relative.front() == '/') {
                        relative.erase(relative.begin());
                    }
                    if (!relative.empty()) {
                        out += AbstractConcreteValues(relative, std::string(), extracted);
                        i = end;
                        continue;
                    }
                }
                AddVariable(extracted, "{{path}}", "演示中的绝对路径,按本次环境替换", path);
                out += "{{path}}";
                i = end;
                continue;
            }
        }

        out.push_back(c);
        ++i;
    }
    return out;
}

namespace {

// 递归抽象 JSON 里的字符串值(键保留原样——键是工具的参数名,是稳定规矩)。
nlohmann::json AbstractJsonValues(const nlohmann::json& value, const std::string& cwd,
                                  std::vector<DraftVariable>& extracted) {
    if (value.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            out[it.key()] = AbstractJsonValues(it.value(), cwd, extracted);
        }
        return out;
    }
    if (value.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& item : value) {
            out.push_back(AbstractJsonValues(item, cwd, extracted));
        }
        return out;
    }
    if (value.is_string()) {
        return AbstractConcreteValues(value.get<std::string>(), cwd, extracted);
    }
    return value;
}

std::string TruncateUtf8Chars(std::string text, std::size_t max_chars) {
    std::size_t chars = 0;
    std::size_t bytes = 0;
    while (bytes < text.size() && chars < max_chars) {
        const unsigned char lead = static_cast<unsigned char>(text[bytes]);
        std::size_t len = 1;
        if ((lead & 0xE0) == 0xC0) {
            len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
        }
        if (bytes + len > text.size()) {
            break;
        }
        bytes += len;
        ++chars;
    }
    if (bytes >= text.size()) {
        return text;
    }
    return text.substr(0, bytes) + "…";
}

// ---------------------------------------------------------------------------
// 起草的内部件
// ---------------------------------------------------------------------------

struct DraftFacts {
    std::string name = "recorded-skill";
    std::string goal;
    std::vector<std::string> declared_variables;
    std::string acceptance;
    std::string cwd;
    std::vector<std::string> notes;
    std::vector<std::string> verifications;
};

struct StepAttempt {
    bool ok = false;
    std::string summary;
};

struct DraftStep {
    std::string tool;
    nlohmann::json input;
    std::vector<StepAttempt> attempts;
};

DraftFacts CollectFacts(const std::vector<RecordEvent>& events) {
    DraftFacts facts;
    for (const RecordEvent& event : events) {
        if (event.type == kEventRecordStart) {
            if (const auto it = event.data.find("name"); it != event.data.end() && it->is_string()) {
                facts.name = it->get<std::string>();
            }
            if (const auto it = event.data.find("cwd"); it != event.data.end() && it->is_string()) {
                facts.cwd = it->get<std::string>();
            }
            if (const auto it = event.data.find("acceptance"); it != event.data.end() && it->is_string()) {
                facts.acceptance = it->get<std::string>();
            }
            if (const auto it = event.data.find("goal"); it != event.data.end() && it->is_string()) {
                facts.goal = it->get<std::string>();
            }
            if (const auto it = event.data.find("variables"); it != event.data.end() && it->is_array()) {
                for (const auto& variable : *it) {
                    if (variable.is_string()) {
                        facts.declared_variables.push_back(variable.get<std::string>());
                    }
                }
            }
        } else if (event.type == kEventGoal) {
            if (const auto it = event.data.find("text"); it != event.data.end() && it->is_string()) {
                facts.goal = it->get<std::string>();  // 后说的盖先说的
            }
        } else if (event.type == kEventVariable) {
            if (const auto it = event.data.find("name"); it != event.data.end() && it->is_string()) {
                facts.declared_variables.push_back(it->get<std::string>());
            }
        } else if (event.type == kEventUserNote) {
            if (const auto it = event.data.find("text"); it != event.data.end() && it->is_string()) {
                facts.notes.push_back(it->get<std::string>());
            }
        } else if (event.type == kEventVerification) {
            if (const auto it = event.data.find("text"); it != event.data.end() && it->is_string()) {
                facts.verifications.push_back(it->get<std::string>());
            }
        }
    }
    return facts;
}

std::vector<DraftStep> CollectSteps(const std::vector<RecordEvent>& events) {
    std::vector<DraftStep> steps;
    for (const RecordEvent& event : events) {
        if (event.type == kEventToolCall) {
            const auto tool_it = event.data.find("tool");
            const auto input_it = event.data.find("input");
            if (tool_it == event.data.end() || !tool_it->is_string()) {
                continue;
            }
            nlohmann::json input = nlohmann::json::object();
            if (input_it != event.data.end() && input_it->is_object()) {
                input = input_it.value();
            }
            // 偶然的失败重试:同工具同入参紧挨着再来一次,折进同一步。
            if (!steps.empty() && steps.back().tool == tool_it->get<std::string>() &&
                steps.back().input == input) {
                continue;
            }
            DraftStep step;
            step.tool = tool_it->get<std::string>();
            step.input = std::move(input);
            steps.push_back(std::move(step));
        } else if (event.type == kEventToolResult) {
            const auto tool_it = event.data.find("tool");
            const auto ok_it = event.data.find("ok");
            const auto summary_it = event.data.find("summary");
            if (tool_it == event.data.end() || !tool_it->is_string()) {
                continue;
            }
            StepAttempt attempt;
            attempt.ok = ok_it != event.data.end() && ok_it->is_boolean() && ok_it->get<bool>();
            if (summary_it != event.data.end() && summary_it->is_string()) {
                attempt.summary = summary_it->get<std::string>();
            }
            if (!steps.empty()) {
                // 结果就近挂最后一步(工具在主循环里串行跑,天然成对;真对
                // 不上也不丢,起草器宁多记一句不少记)。
                steps.back().attempts.push_back(std::move(attempt));
            }
            // 步骤都还没有却先来了结果:没有可挂的步骤,丢弃这条孤儿结果。
        }
    }
    return steps;
}

// 一步的收尾描述:成功/失败/带一次失败重试。
std::string StepOutcomeText(const DraftStep& step) {
    const StepAttempt* last = step.attempts.empty() ? nullptr : &step.attempts.back();
    std::size_t failures = 0;
    std::string first_failure;
    for (const StepAttempt& attempt : step.attempts) {
        if (!attempt.ok) {
            ++failures;
            if (first_failure.empty()) {
                first_failure = attempt.summary;
            }
        }
    }
    const bool finally_ok = last != nullptr && last->ok;
    if (!finally_ok && last != nullptr) {
        return "若失败,分支处理: " + TruncateUtf8Chars(last->summary, 60);
    }
    if (last == nullptr) {
        return "(无结果记录)";
    }
    if (failures > 0) {
        return "成功(此前失败过 " + std::to_string(failures) + " 次: " + TruncateUtf8Chars(first_failure, 60) + ")";
    }
    return "成功: " + TruncateUtf8Chars(last->summary, 60);
}

}  // namespace

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------

std::expected<std::string, std::string> ValidateSkillMarkdownForInstall(const std::string& content) {
    const auto parsed = tools::ParseSkillMarkdown(content);
    if (!parsed.has_value()) {
        return std::unexpected("frontmatter 损坏(没有闭合的 ---),不予安装");
    }
    if (!parsed->name.has_value() || parsed->name->empty()) {
        return std::unexpected("frontmatter 缺 name,不予安装");
    }
    if (!parsed->description.has_value() || parsed->description->empty()) {
        return std::unexpected("frontmatter 缺 description,不予安装");
    }
    if (parsed->body.find("验收") == std::string::npos) {
        return std::unexpected("正文缺验收节,不予安装");
    }
    return *parsed->name;
}

// ---------------------------------------------------------------------------
// 起草
// ---------------------------------------------------------------------------

std::string ComposeSkillMarkdown(const std::vector<RecordEvent>& events) {
    const DraftFacts facts = CollectFacts(events);
    const std::vector<DraftStep> steps = CollectSteps(events);
    std::vector<DraftVariable> variables;
    for (const std::string& declared : facts.declared_variables) {
        const std::string clean = SingleLine(declared, 80);
        if (!clean.empty()) {
            AddVariable(variables, clean, "录制时用户口述的可变输入", "");
        }
    }

    const std::string skill_name = MakeSkillNameSlug(facts.name);
    // 目标先过一遍抽象(具体值提成变量),description 才不夹死值。
    const std::string goal_text = AbstractConcreteValues(facts.goal, facts.cwd, variables);

    // 步骤、验收、备注先抽象完再拼文档——抽象会往 variables 里补
    // {{date}}/{{url}}/{{path}},输入节要等变量攒齐了才渲染。
    std::vector<std::string> step_lines;
    std::vector<std::string> risky;
    for (const DraftStep& step : steps) {
        const nlohmann::json abstracted = AbstractJsonValues(step.input, facts.cwd, variables);
        std::string params = abstracted.dump();
        if (params == "{}") {
            params.clear();
        } else {
            params = " " + TruncateUtf8Chars(params, 120);
        }
        step_lines.push_back("用 " + step.tool + params + " — " + StepOutcomeText(step));
        if ((step.tool == "write_file" || step.tool == "edit_file" || step.tool == "run_command") &&
            std::find(risky.begin(), risky.end(), step.tool) == risky.end()) {
            risky.push_back(step.tool);
        }
    }
    const std::string acceptance = AbstractConcreteValues(facts.acceptance, facts.cwd, variables);
    std::string last_verification;
    if (!facts.verifications.empty()) {
        last_verification = AbstractConcreteValues(facts.verifications.back(), facts.cwd, variables);
    }
    std::vector<std::string> note_lines;
    for (const std::string& note : facts.notes) {
        note_lines.push_back(AbstractConcreteValues(note, facts.cwd, variables));
    }

    std::string description = SingleLine(goal_text, 70);
    if (description.empty()) {
        description = "由录制生成的技能";
    }

    std::string out;
    out += "---\n";
    out += "name: " + skill_name + "\n";
    out += "description: " + description + "\n";
    out += "---\n\n";
    out += "# " + skill_name + "\n\n";

    out += "## 何时用\n\n";
    out += goal_text.empty() ? "(录制时未口述目标。)\n" : goal_text + "\n";
    out += "\n";

    out += "## 输入\n\n";
    if (variables.empty()) {
        out += "- 无必填输入(演示里没有每回会变的值)。\n";
    } else {
        for (const DraftVariable& variable : variables) {
            out += "- " + variable.name + ": " + variable.description;
            if (!variable.example.empty()) {
                out += "(演示值: " + TruncateUtf8Chars(variable.example, 50) + ")";
            }
            out += "\n";
        }
    }
    out += "\n";

    out += "## 步骤\n\n";
    if (step_lines.empty()) {
        out += "1. (录制期间没有工具调用,按“何时用”的口述执行。)\n";
    } else {
        int index = 1;
        for (const std::string& line : step_lines) {
            out += std::to_string(index) + ". " + line + "\n";
            ++index;
        }
    }
    out += "\n";

    out += "## 风险动作与确认点\n\n";
    if (risky.empty()) {
        out += "- 本次演示未涉及写文件或跑命令;若临场需要,先向用户确认。\n";
    } else {
        for (const std::string& tool : risky) {
            if (tool == "run_command") {
                out += "- run_command: 执行前核对命令与参数;危险命令须用户确认。\n";
            } else {
                out += "- " + tool + ": 改文件前看清目标路径与改动范围;重要改动须用户确认。\n";
            }
        }
    }
    out += "\n";

    out += "## 验收\n\n";
    out += "- " + (acceptance.empty() ? "按“何时用”里口述的成事标准核验。" : acceptance) + "\n";
    if (!last_verification.empty()) {
        out += "- 演示中最后一次验证: " + last_verification + "\n";
    }
    out += "\n";

    if (!note_lines.empty()) {
        out += "## 备注\n\n";
        for (const std::string& note : note_lines) {
            out += "- " + note + "\n";
        }
    }
    return out;
}

namespace {

// 回炉的正文抢救:见 RepairSkillFrontmatter 公开注释。
std::string SalvageBody(const std::string& content) {
    if (const auto parsed = tools::ParseSkillMarkdown(content); parsed.has_value()) {
        return parsed->body;
    }
    std::size_t line_begin = 0;
    int delimiters = 0;
    std::size_t pos = 0;
    while (pos < content.size()) {
        const std::size_t line_end = content.find('\n', pos);
        const std::string line = content.substr(pos, line_end == std::string::npos ? std::string::npos
                                                                                   : line_end - pos);
        if (line == "---" || line == "---\r") {
            ++delimiters;
            if (delimiters == 2) {
                line_begin = line_end == std::string::npos ? content.size() : line_end + 1;
                break;
            }
        }
        if (line_end == std::string::npos) {
            break;
        }
        pos = line_end + 1;
    }
    return content.substr(line_begin);
}

}  // namespace

std::string RepairSkillFrontmatter(const std::string& content) {
    return "---\nname: recorded-skill\ndescription: 由录制生成的技能\n---\n" + SalvageBody(content);
}

std::expected<SkillDraftResult, std::string> WriteSkillDraft(const fs::path& recording_dir,
                                                             const std::vector<RecordEvent>& events) {
    if (events.empty()) {
        return std::unexpected("录制事件是空的,起不出草稿");
    }
    std::string content = ComposeSkillMarkdown(events);
    if (!ValidateSkillMarkdownForInstall(content).has_value()) {
        // 回炉一次:frontmatter 推倒重建,正文保住。
        content = RepairSkillFrontmatter(content);
        if (!ValidateSkillMarkdownForInstall(content).has_value()) {
            return std::unexpected("草稿 frontmatter 回炉后仍不合法,不予落盘");
        }
    }

    std::error_code ec;
    const fs::path draft_dir = recording_dir / "draft";
    fs::create_directories(draft_dir, ec);
    if (ec) {
        return std::unexpected("建草稿目录失败: " + PathToUtf8(draft_dir) + ": " + ec.message());
    }
    const fs::path skill_md = draft_dir / "SKILL.md";
    {
        std::ofstream file(skill_md, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return std::unexpected("写草稿失败: " + PathToUtf8(skill_md));
        }
        file << content;
        if (!file.good()) {
            return std::unexpected("写草稿失败: " + PathToUtf8(skill_md));
        }
    }
    SkillDraftResult result;
    result.draft_dir = draft_dir;
    result.files = {"SKILL.md"};
    return result;
}

}  // namespace lubancode::agent
