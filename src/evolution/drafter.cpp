// EvolutionDrafter 的实现(自进化闭环阶段 2/5)。全文纯函数:输入一场录制
// (或一个同形簇),输出文本。密钥这道门由录制件本身把守(入盘前已过
// SanitizeToolInput/RedactSecrets),这里再过一遍 skills::RedactSecrets 兜底。
#include "evolution/drafter.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>

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

// ---------------------------------------------------------------------------
// 阶段 5:同形多场 -> 组合包
// ---------------------------------------------------------------------------

namespace {

// workflow 档的最少步数:单步的稳定做法归 Skill,不算"编排"。
constexpr int kMinWorkflowSteps = 2;

// YAML 双引号标量:把任意文本安全写进 workflow.yaml/agent.yaml。控制字符
// 与引号反斜杠转义;其余(含 UTF-8 原文)原样。起草器不赌值长什么样。
std::string YamlDoubleQuote(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char raw : text) {
        const char c = static_cast<char>(raw);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (raw < 0x20 || raw == 0x7F) {
                    char buffer[8]{};
                    std::snprintf(buffer, sizeof(buffer), "\\x%02x", raw);
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    out += "\"";
    return out;
}

// workflow 输入变量名:step<n>_<key 洗净>(小写字母数字下划线,数字起头
// 补前缀,空了落 arg)。
std::string MakeInputVarName(int step_index, const std::string& key) {
    std::string clean;
    for (const unsigned char raw : key) {
        const char c = static_cast<char>(raw);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            clean.push_back(c);
        }
    }
    if (clean.empty()) {
        clean = "arg";
    }
    if (clean[0] >= '0' && clean[0] <= '9') {
        clean = "arg" + clean;
    }
    return "step" + std::to_string(step_index) + "_" + clean;
}

// workflow id:小写字母数字与 '-'(契约:小写字母起头)。skill slug 已是
// kebab,这里再洗一遍兜底,顶头横线剥掉。
std::string SanitizeFlowId(std::string slug) {
    std::string out;
    bool last_dash = false;
    for (const unsigned char raw : slug) {
        const char c = static_cast<char>(raw);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out.push_back(c);
            last_dash = false;
        } else if (c == '-' && !out.empty() && !last_dash) {
            out.push_back('-');
            last_dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "evolve-flow";
    }
    return out;
}

// 标量值 -> inputs.properties 的 type 字样。
std::string JsonTypeToInputType(const nlohmann::json& value) {
    if (value.is_number_integer()) return "integer";
    if (value.is_number_float()) return "number";
    if (value.is_boolean()) return "boolean";
    return "string";
}

// 一场录制的折叠段:连续同名 tool_call 折成一段,结果挂到最近一段同名
// 待配对段上(录制件是串行工具循环,天然成对)。段的成色 = 段内最后一
// 枚结果的 ok。
struct FoldedRun {
    std::string tool;
    nlohmann::json first_input;
    bool has_result = false;
    bool last_ok = false;
    bool pending = true;  // 等结果;来了结果就闭
};

std::vector<FoldedRun> CollectFoldedRuns(const std::vector<skills::RecordEvent>& events) {
    std::vector<FoldedRun> runs;
    for (const skills::RecordEvent& event : events) {
        if (event.type == skills::kEventToolCall) {
            const auto tool_it = event.data.find("tool");
            if (tool_it == event.data.end() || !tool_it->is_string()) {
                continue;
            }
            nlohmann::json input = nlohmann::json::object();
            if (const auto input_it = event.data.find("input");
                input_it != event.data.end() && input_it->is_object()) {
                input = input_it.value();
            }
            if (!runs.empty() && runs.back().tool == tool_it->get<std::string>()) {
                continue;  // 连续同名折进同段
            }
            FoldedRun run;
            run.tool = tool_it->get<std::string>();
            run.first_input = std::move(input);
            runs.push_back(std::move(run));
        } else if (event.type == skills::kEventToolResult) {
            const auto tool_it = event.data.find("tool");
            if (tool_it == event.data.end() || !tool_it->is_string()) {
                continue;
            }
            const bool ok = event.data.contains("ok") && event.data.at("ok").is_boolean() &&
                            event.data.at("ok").get<bool>();
            for (std::size_t i = runs.size(); i > 0; --i) {
                FoldedRun& run = runs[i - 1];
                if (run.tool == tool_it->get<std::string>() && run.pending) {
                    run.has_result = true;
                    run.last_ok = ok;
                    run.pending = false;
                    break;
                }
            }
        }
    }
    return runs;
}

// 各场首枚 record_start 的 cwd(抽象入参字面量时用;拿不到给空)。
std::string CwdOf(const std::vector<skills::RecordEvent>& events) {
    for (const skills::RecordEvent& event : events) {
        if (event.type == skills::kEventRecordStart) {
            const auto it = event.data.find("cwd");
            if (it != event.data.end() && it->is_string()) {
                return it->get<std::string>();
            }
        }
    }
    return std::string();
}

// 一个折叠段的入参,按"各场对齐"合成节点的 input:
//   - 键在各场都在且标量值(抽象后)相等 -> 留字面量(抽象形态);
//   - 键是标量但各场不同 -> 提成 workflow 输入 ${inputs.step<N>_<key>},
//     并登记进 vars(名字、type、示例描述);
//   - 非标量(对象/数组)只在各场深度相等时留(JSON 流式),否则丢(草稿
//     宁少勿错;丢的进 dropped 注记)。
void MergeNodeInput(const std::vector<nlohmann::json>& inputs, const std::vector<std::string>& cwds,
                    int step_index, nlohmann::json& out_input,
                    std::vector<std::pair<std::string, nlohmann::json>>& vars,
                    std::vector<std::string>& dropped) {
    if (inputs.empty() || !inputs.front().is_object()) {
        return;
    }
    // 键次序照首场;各场缺的键按"值不同"办(不能当稳定字面量)。
    for (auto it = inputs.front().begin(); it != inputs.front().end(); ++it) {
        const std::string& key = it.key();
        std::vector<const nlohmann::json*> values;
        bool all_present = true;
        for (const nlohmann::json& input : inputs) {
            if (!input.is_object() || !input.contains(key)) {
                all_present = false;
                break;
            }
            values.push_back(&input.at(key));
        }
        if (!all_present) {
            dropped.push_back("step" + std::to_string(step_index) + " 入参 " + key + "(各场不一致,未焊死)");
            continue;
        }
        const bool all_scalar = std::all_of(values.begin(), values.end(), [](const nlohmann::json* v) {
            return v->is_string() || v->is_number() || v->is_boolean();
        });
        if (all_scalar) {
            // 标量:抽象后再比(同一句相对路径两场一样才算稳;绝对路径/
            // 日期/网址抽象不掉就当各场不同,提输入)。
            std::vector<std::string> abstracted;
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (values[i]->is_string()) {
                    std::vector<skills::DraftVariable> scratch;
                    abstracted.push_back(
                        skills::AbstractConcreteValues(values[i]->get<std::string>(), cwds[i], scratch));
                } else {
                    abstracted.push_back(values[i]->dump());
                }
            }
            const bool stable = std::all_of(abstracted.begin(), abstracted.end(),
                                            [&](const std::string& v) { return v == abstracted.front(); });
            if (stable) {
                out_input[key] = abstracted.front();
                continue;
            }
            const std::string var = MakeInputVarName(step_index, key);
            nlohmann::json property;
            property["type"] = JsonTypeToInputType(*values.front());
            std::string examples;
            for (const std::string& v : abstracted) {
                if (examples.size() >= 60) break;
                examples += (examples.empty() ? "" : " / ") + TruncateChars(v, 30);
            }
            property["description"] = "各场不同的值(示例: " + examples + ")";
            vars.emplace_back(var, std::move(property));
            out_input[key] = "${inputs." + var + "}";
            continue;
        }
        // 非标量:深度相等才留。
        const bool deep_equal = std::all_of(values.begin(), values.end(), [&](const nlohmann::json* v) {
            return *v == *values.front();
        });
        if (deep_equal) {
            out_input[key] = *values.front();
        } else {
            dropped.push_back("step" + std::to_string(step_index) + " 入参 " + key + "(各场不一致,未焊死)");
        }
    }
}

// workflow.yaml 的正文。节点 = 成功路折叠步;失败路(簇内连败不附成功的
// 工具)写进头注释与 description——编排只走成功路,连败的工具不上链。
std::string ComposeWorkflowYaml(const std::string& flow_id, const std::string& display_name,
                                const std::string& description, int cluster_size,
                                const std::vector<std::pair<std::string, nlohmann::json>>& vars,
                                const std::vector<SequencedToolStep>& steps,
                                const std::vector<std::map<std::string, std::string>>& failure_notes,
                                const std::vector<std::string>& dropped) {
    std::string out;
    out += "# 自进化闭环阶段 5 起草:从 " + std::to_string(cluster_size) +
           " 场同形任务提炼的稳定编排。\n";
    out += "# 节点 = 成功路折叠步(各场工具名序列经连续同名折叠后同形);各场\n";
    out += "# 不同的入参提成 inputs,同值的留字面量——不把录制现场焊死。\n";
    if (!failure_notes.empty()) {
        out += "# 已知失败路(阶段 2 抽的稳定失败模式,连败无成功;遇此先换路,\n";
        out += "# 别硬走;排错细节见同包 SKILL 的\"排错\"节):\n";
        for (const auto& note : failure_notes) {
            for (const auto& [tool, summary] : note) {
                out += "#   - " + tool + ": " + TruncateChars(summary, 70) + "\n";
            }
        }
    }
    if (!dropped.empty()) {
        out += "# 各场不一致、未焊死的入参:\n";
        for (const std::string& item : dropped) {
            out += "#   - " + item + "\n";
        }
    }
    out += "# 评测分家:本组件只做静态校验与来源回放的夹具;评测计划的执行\n";
    out += "# 不起这份 workflow 自己跑(README\"评测 Workflow 与被测 Workflow\n";
    out += "# 分家\")。\n";
    out += "schema_version: 1\n";
    out += "id: " + flow_id + "\n";
    out += "version: 1.0.0\n";
    out += "name: " + YamlDoubleQuote(display_name) + "\n";
    out += "alias: " + flow_id + "\n";
    out += "description: " + YamlDoubleQuote(description) + "\n";
    out += "enabled: true\n";
    if (!vars.empty()) {
        out += "\ninputs:\n";
        out += "  type: object\n";
        out += "  required:\n";
        for (const auto& [name, property] : vars) {
            out += "    - " + name + "\n";
        }
        out += "  properties:\n";
        for (const auto& [name, property] : vars) {
            out += "    " + name + ":\n";
            out += "      type: " + property.value("type", std::string("string")) + "\n";
            out += "      description: " + YamlDoubleQuote(property.value("description", std::string())) +
                   "\n";
        }
    }
    out += "\nentry: step_1\n";
    out += "\nlimits:\n";
    out += "  max_concurrency: 2\n";
    out += "  max_nodes: 24\n";
    out += "  max_steps: 48\n";
    out += "  timeout: 10m\n";
    out += "  tool_calls: 60\n";
    out += "\nnodes:\n";
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const int index = static_cast<int>(i) + 1;
        out += "  step_" + std::to_string(index) + ":\n";
        out += "    type: tool\n";
        out += "    label: " + YamlDoubleQuote("步 " + std::to_string(index) + " · " + steps[i].tool) + "\n";
        out += "    tool: " + YamlDoubleQuote(steps[i].tool) + "\n";
        if (steps[i].merged_input.is_object() && !steps[i].merged_input.empty()) {
            out += "    input:\n";
            for (auto it = steps[i].merged_input.begin(); it != steps[i].merged_input.end(); ++it) {
                if (it.value().is_string()) {
                    const std::string& text = it.value().get<std::string>();
                    if (text.rfind("${inputs.", 0) == 0 && text.back() == '}') {
                        out += "      " + YamlDoubleQuote(it.key()) + ": " + text + "\n";
                    } else {
                        out += "      " + YamlDoubleQuote(it.key()) + ": " + YamlDoubleQuote(text) + "\n";
                    }
                } else if (it.value().is_number_integer()) {
                    out += "      " + YamlDoubleQuote(it.key()) + ": " +
                           std::to_string(it.value().get<std::int64_t>()) + "\n";
                } else if (it.value().is_number_float()) {
                    out += "      " + YamlDoubleQuote(it.key()) + ": " + it.value().dump() + "\n";
                } else if (it.value().is_boolean()) {
                    out += "      " + YamlDoubleQuote(it.key()) + ": " +
                           (it.value().get<bool>() ? "true" : "false") + "\n";
                } else {
                    // 非标量:JSON 流式(YAML 兼容)。
                    out += "      " + YamlDoubleQuote(it.key()) + ": " + it.value().dump() + "\n";
                }
            }
        } else {
            out += "    input: {}\n";
        }
    }
    out += "  done:\n";
    out += "    type: end\n";
    out += "    label: " + YamlDoubleQuote("完成(按同包 SKILL 验收节核验)") + "\n";
    out += "\nedges:\n";
    for (std::size_t i = 0; i + 1 < steps.size(); ++i) {
        out += "  - { from: step_" + std::to_string(i + 1) + ", on: success, to: step_" +
               std::to_string(i + 2) + " }\n";
    }
    out += "  - { from: step_" + std::to_string(steps.size()) + ", on: success, to: done }\n";
    out += "\nresult:\n";
    out += "  last_output: \"${nodes.step_" + std::to_string(steps.size()) + ".output}\"\n";
    return out;
}

std::string ComposeAgentYaml(const std::string& agent_name, const std::string& description,
                             const std::string& skill_slug, const std::vector<std::string>& face,
                             int cluster_size) {
    std::string out;
    out += "# " + agent_name + ":自进化闭环阶段 5 从 " + std::to_string(cluster_size) +
           " 场同形任务提炼的稳定角色。\n";
    out += "# 门槛:全场工具面(含失败尝试)在多场间同形才起草——一次任务\n";
    out += "# 不造 Agent(README 五档决策表)。工具面照观察到的实际面收窄,\n";
    out += "# 预装同包 Skill。\n";
    out += "schema: 1\n";
    out += "name: " + agent_name + "\n";
    out += "description: " + YamlDoubleQuote(description) + "\n";
    out += "skills:\n";
    out += "  preload:\n";
    out += "    - " + skill_slug + "\n";
    out += "tools:\n";
    out += "  allow:\n";
    for (const std::string& tool : face) {
        out += "    - " + YamlDoubleQuote(tool) + "\n";
    }
    return out;
}

}  // namespace

std::vector<SequencedToolStep> SuccessPathSteps(const std::vector<skills::RecordEvent>& events) {
    std::vector<SequencedToolStep> out;
    for (const FoldedRun& run : CollectFoldedRuns(events)) {
        if (!run.has_result || !run.last_ok) {
            continue;  // 连败不附成功:不在成功路上
        }
        SequencedToolStep step;
        step.tool = run.tool;
        step.first_input = run.first_input;
        out.push_back(std::move(step));
    }
    return out;
}

std::vector<std::string> ToolFace(const std::vector<skills::RecordEvent>& events) {
    std::vector<std::string> face;
    std::set<std::string> seen;
    for (const FoldedRun& run : CollectFoldedRuns(events)) {
        if (seen.insert(run.tool).second) {
            face.push_back(run.tool);
        }
    }
    return face;
}

ComboThreshold AssessComboThreshold(const std::vector<ClusterTaskMaterial>& tasks) {
    ComboThreshold verdict;
    verdict.cluster_size = static_cast<int>(tasks.size());
    if (tasks.size() < 2) {
        verdict.why_not.push_back("簇内只有 " + std::to_string(tasks.size()) +
                                  " 场独立任务,不够 Workflow 门槛(>=2;单场照旧 Skill-only)");
        return verdict;
    }
    // 尺一之前半:各场成功路折叠序列同形。
    std::vector<std::vector<std::string>> sequences;
    for (const ClusterTaskMaterial& task : tasks) {
        std::vector<std::string> names;
        for (const SequencedToolStep& step : SuccessPathSteps(task.events)) {
            names.push_back(step.tool);
        }
        sequences.push_back(std::move(names));
    }
    verdict.sequences_stable = std::all_of(sequences.begin(), sequences.end(),
                                           [&](const std::vector<std::string>& s) {
                                               return s == sequences.front();
                                           });
    verdict.workflow_steps = static_cast<int>(sequences.front().size());
    if (!verdict.sequences_stable) {
        verdict.why_not.push_back("各场成功路折叠序列不同形(同工具名经连续折叠后次序/次数不一,"
                                  "组合不稳),照旧 Skill-only");
        return verdict;
    }
    if (verdict.workflow_steps < kMinWorkflowSteps) {
        verdict.why_not.push_back("成功路只有 " + std::to_string(verdict.workflow_steps) +
                                  " 步,单步的稳定做法归 Skill,不算编排");
        return verdict;
    }
    verdict.workflow_eligible = true;
    // 尺二:各场全场工具面同形(编排看成功路,角色看整个工具面)。
    std::vector<std::vector<std::string>> faces;
    for (const ClusterTaskMaterial& task : tasks) {
        faces.push_back(ToolFace(task.events));
    }
    verdict.faces_stable = std::all_of(faces.begin(), faces.end(), [&](const std::vector<std::string>& f) {
        return f == faces.front();
    });
    if (verdict.faces_stable) {
        verdict.agent_eligible = true;
    } else {
        verdict.why_not.push_back("各场全场工具面不同形(失败重试里摸过的工具不一致),"
                                  "不封同一只 Agent——Workflow 照起,Agent 不添");
    }
    return verdict;
}

std::expected<ComboCandidateDraft, std::string> DraftEvolutionCandidate(
    const std::vector<ClusterTaskMaterial>& tasks) {
    if (tasks.empty()) {
        return std::unexpected("簇是空的,起不出候选");
    }
    for (const ClusterTaskMaterial& task : tasks) {
        bool finished = false;
        for (const skills::RecordEvent& event : task.events) {
            if (event.type == skills::kEventRecordStop) {
                finished = true;
                break;
            }
        }
        if (!finished) {
            return std::unexpected("录制件 \"" + task.status.id + "\" 没录完(缺 record_stop),半截示范起不出候选");
        }
    }

    // ---- 最小包部分:照首场(点名场)起,与阶段 2 同一条路 ----
    auto base = DraftSkillCandidate(tasks.front().status, tasks.front().events);
    if (!base.has_value()) {
        return std::unexpected(base.error());
    }
    ComboCandidateDraft draft;
    draft.skill_slug = base->skill_slug;
    draft.skill_markdown = std::move(base->skill_markdown);
    draft.package_yaml = std::move(base->package_yaml);
    draft.package_id = base->package_id;
    draft.package_version = base->package_version;
    draft.objective = base->objective;
    for (const ClusterTaskMaterial& task : tasks) {
        draft.recording_ids.push_back(task.status.id);
    }

    // ---- 两把尺 ----
    draft.threshold = AssessComboThreshold(tasks);
    if (!draft.threshold.workflow_eligible) {
        return draft;  // 最小可行包仍是默认答案
    }

    // ---- workflow.yaml:节点 = 成功路折叠步,入参按各场对齐合成 ----
    const std::vector<SequencedToolStep> first_steps = SuccessPathSteps(tasks.front().events);
    std::vector<std::pair<std::string, nlohmann::json>> vars;
    std::vector<std::string> dropped;
    std::vector<SequencedToolStep> merged_steps;
    std::vector<std::string> cwds;
    for (const ClusterTaskMaterial& task : tasks) {
        cwds.push_back(CwdOf(task.events));
    }
    for (std::size_t i = 0; i < first_steps.size(); ++i) {
        SequencedToolStep merged;
        merged.tool = first_steps[i].tool;
        std::vector<nlohmann::json> inputs;
        for (const ClusterTaskMaterial& task : tasks) {
            const std::vector<SequencedToolStep> steps = SuccessPathSteps(task.events);
            inputs.push_back(i < steps.size() ? steps[i].first_input : nlohmann::json::object());
        }
        MergeNodeInput(inputs, cwds, static_cast<int>(i) + 1, merged.merged_input, vars, dropped);
        merged_steps.push_back(std::move(merged));
    }

    // 失败路:簇内各场的稳定失败模式(连败无成功)并账,按工具收首条摘要。
    std::map<std::string, std::string> failure_by_tool;
    for (const ClusterTaskMaterial& task : tasks) {
        for (const skills::DraftFailureMode& mode : skills::CollectStableFailureModes(task.events)) {
            failure_by_tool.emplace(mode.tool, mode.summary.empty() ? "(无失败摘要)" : mode.summary);
        }
    }
    std::vector<std::map<std::string, std::string>> failure_notes;
    if (!failure_by_tool.empty()) {
        failure_notes.push_back(failure_by_tool);
    }

    const std::string flow_id = SanitizeFlowId(draft.skill_slug + "-flow");
    const std::string display_name = TruncateChars(OneLine(tasks.front().status.name), 60);
    std::string description = TruncateChars(SanitizeObservationText(draft.objective, 120), 120);
    if (description.empty()) {
        description = "从 " + std::to_string(tasks.size()) + " 场同形任务提炼的稳定编排";
    }
    description += ";自进化阶段 5 组合候选(" + std::to_string(draft.threshold.workflow_steps) + " 步)";
    if (!failure_by_tool.empty()) {
        description += ";已知连败路 " + std::to_string(failure_by_tool.size()) + " 处见头注释";
    }
    draft.workflow_id = flow_id;
    draft.workflow_yaml = ComposeWorkflowYaml(flow_id,
                                              display_name.empty() ? flow_id : display_name, description,
                                              static_cast<int>(tasks.size()), vars, merged_steps,
                                              failure_notes, dropped);

    // ---- Agent:尺二过了才添(tools.allow 照观察到的实际面、预装 Skill)----
    if (draft.threshold.agent_eligible) {
        const std::string agent_name = draft.skill_slug + "-agent";
        const std::vector<std::string> face = ToolFace(tasks.front().events);
        std::string agent_description = TruncateChars(
            SanitizeObservationText(draft.objective, 140), 140);
        if (agent_description.empty()) {
            agent_description = "从 " + std::to_string(tasks.size()) + " 场同形任务提炼的稳定角色";
        }
        agent_description += ";工具面照观察到的 " + std::to_string(face.size()) +
                             " 件收窄,预装同包 Skill " + draft.skill_slug;
        draft.with_agent = true;
        draft.agent_name = agent_name;
        draft.agent_yaml =
            ComposeAgentYaml(agent_name, agent_description, draft.skill_slug, face,
                             static_cast<int>(tasks.size()));
    }
    return draft;
}

}  // namespace lubancode::evolution
