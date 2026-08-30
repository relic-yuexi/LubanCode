// EvolutionDrafter 的实现(自进化闭环阶段 2/5/6)。全文纯函数:输入一场录制
// (或一个同形簇),输出文本。密钥这道门由录制件本身把守(入盘前已过
// SanitizeToolInput/RedactSecrets),这里再过一遍 skills::RedactSecrets 兜底。
#include "evolution/drafter.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>

#include "evolution/observation.hpp"
#include "package/component.hpp"  // ParseMcpComponentYaml(MCP 草稿落盘前自验)
#include "package/manifest.hpp"
#include "runtime/plugin_contract.hpp"
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

// ---------------------------------------------------------------------------
// 阶段 6:代码档——process Plugin 草稿的三份文本
//
// 草稿三份文本都按"静态门能过、评测零进程"来写:不带网络原语、不带
// 路径逃逸、不带恶意形状、依赖清单为空。想干这些事的人得手工改草稿,
// 改完过四类夹具扫描,再走人工审查线——那正是设计要的摩擦。
// ---------------------------------------------------------------------------

// 工具名洗净成合法标识([A-Za-z0-9_],字母数字起头;空了落 tool)。
std::string SanitizeToolName(std::string name) {
    std::string out;
    for (const unsigned char raw : name) {
        const char c = static_cast<char>(raw);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.front() == '_') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.empty() || (out.front() >= '0' && out.front() <= '9')) {
        out = "tool_" + out;
    }
    if (out.size() > 64) {
        out.resize(64);
    }
    return out;
}

// 工具名 -> 插件目录名(小写 kebab):pdf_extract -> pdf-extract。
std::string SanitizePluginDirName(const std::string& tool_name) {
    std::string out;
    bool last_dash = true;  // 顶头横线剥掉
    for (const unsigned char raw : tool_name) {
        const char c = static_cast<char>(raw);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out.push_back(c);
            last_dash = false;
        } else if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
            last_dash = false;
        } else if (!out.empty() && !last_dash) {
            out.push_back('-');
            last_dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out;
}

// 大写下划线环境变量名:pdf-extract -> EVOLVE_PDF_EXTRACT_DRY_RUN。
std::string MakeDraftEnvName(const std::string& plugin_dir_name) {
    std::string upper;
    for (const unsigned char raw : plugin_dir_name) {
        const char c = static_cast<char>(raw);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            upper.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c));
        } else {
            upper.push_back('_');
        }
    }
    return "EVOLVE_" + upper + "_DRY_RUN";
}

// plugin.json(manifest v1,process)。形状照官方样例
// examples/packages/gui-agent/plugins/*/plugin.json 与阶段 0 夹具
// candidate-code-rejected:runtime.kind 只写 process,permissions.network
// 恒 false(草稿不开网),env 只记名不记值。
std::string ComposePluginJson(const std::string& plugin_dir_name, const std::string& tool_name,
                              const std::string& tool_description, const nlohmann::json& schema,
                              const std::string& env_name, int tasks_wanting) {
    nlohmann::json manifest;
    manifest["manifest_version"] = 1;
    manifest["id"] = plugin_dir_name;
    manifest["version"] = "0.1.0";
    manifest["language"] = "python";
    nlohmann::json runtime;
    runtime["kind"] = "process";
    runtime["command"] = "python";
    runtime["args"] = nlohmann::json::array({"${plugin_dir}/runner.py"});
    runtime["timeout_ms"] = 30000;
    manifest["runtime"] = runtime;
    nlohmann::json tool;
    tool["name"] = tool_name;
    tool["description"] = tool_description + "(process Plugin 草稿:源自 " +
                          std::to_string(tasks_wanting) +
                          " 场任务对同一件不存在工具的稳定需求;脚手架未实现,"
                          "补实现须过人工审查)";
    tool["input_schema"] = schema;
    manifest["tools"] = nlohmann::json::array({tool});
    nlohmann::json permissions;
    permissions["network"] = false;
    permissions["env"] = nlohmann::json::array({env_name});
    manifest["permissions"] = permissions;
    return manifest.dump(2) + "\n";
}

// runner.py 脚手架。协议铁律与 examples/plugins/local_math 同源:stdin 恰好
// 一份 JSON 请求,stdout 恰好一份 JSON 响应,日志只写 stderr。草稿的处理
// 函数全是诚实的"未实现"占位——它被起起来也只能说"没实现",干不了任何
// 事;评测也不起它(零进程)。
std::string ComposeRunnerPy(const std::string& plugin_dir_name, const std::string& tool_name,
                            const std::vector<std::string>& input_keys, int tasks_wanting) {
    std::string args_doc;
    for (const std::string& key : input_keys) {
        args_doc += (args_doc.empty() ? "" : ", ") + key;
    }
    if (args_doc.empty()) {
        args_doc = "(观察账里没记到入参)";
    }
    std::string out;
    out += "# -*- coding: utf-8 -*-\n";
    out += "\"\"\"" + plugin_dir_name + " process 插件 runner 脚手架(自进化闭环阶段 6 草稿)。\n";
    out += "\n";
    out += "这份文件是草稿,不是成品:\n";
    out += "  - 工具处理函数全是诚实的\"未实现\"占位,补实现须人工完成;\n";
    out += "  - 人工审查线:除 Package trust 外,还须人读一遍本文件与 plugin.json;\n";
    out += "  - 协议铁律:stdin 恰好一份 JSON 请求,读尽即答;stdout 恰好一份\n";
    out += "    JSON 响应,前后不许混任何字节;日志只写 stderr。\n";
    out += "\n";
    out += "来源:" + std::to_string(tasks_wanting) + " 场任务想用工具 " + tool_name +
           "(现有工具办不了,录到的是 registry.unknown_tool 失败)。\n";
    out += "观察到的入参键:" + args_doc + "。\n";
    out += "\"\"\"\n";
    out += "from __future__ import annotations\n";
    out += "\n";
    out += "import json\n";
    out += "import sys\n";
    out += "\n";
    out += "\n";
    out += "def draft_not_implemented(call_id: str, tool: str) -> dict:\n";
    out += "    return {\n";
    out += "        \"call_id\": call_id,\n";
    out += "        \"plugin\": \"" + plugin_dir_name + "\",\n";
    out += "        \"tool\": tool,\n";
    out += "        \"ok\": False,\n";
    out += "        \"error\": {\n";
    out += "            \"code\": \"execution_failed\",\n";
    out += "            \"message\": \"draft-not-implemented: 阶段 6 草稿,须人工补实现\",\n";
    out += "        },\n";
    out += "    }\n";
    out += "\n";
    out += "\n";
    out += "def main() -> int:\n";
    out += "    raw = sys.stdin.read()\n";
    out += "    try:\n";
    out += "        request = json.loads(raw)\n";
    out += "    except ValueError:\n";
    out += "        sys.stdout.write(json.dumps(draft_not_implemented(\"\", \"(bad-json)\"), ensure_ascii=False))\n";
    out += "        return 0\n";
    out += "    tool = str(request.get(\"tool\", \"\"))\n";
    out += "    call_id = str(request.get(\"call_id\", \"\"))\n";
    out += "    sys.stdout.write(json.dumps(draft_not_implemented(call_id, tool), ensure_ascii=False))\n";
    out += "    return 0\n";
    out += "\n";
    out += "\n";
    out += "if __name__ == \"__main__\":\n";
    out += "    raise SystemExit(main())\n";
    return out;
}

// requirements.txt 依赖清单。草稿不添任何第三方依赖:补实现时按需登记,
// 非注册表来源的依赖会被静态门的依赖投毒扫描拦下,再走人工审查。
// 注释行在扫描里跳过,但这里也只字面提到"直链",不摆形状。
std::string ComposeRequirementsTxt(const std::string& plugin_dir_name) {
    std::string out;
    out += "# " + plugin_dir_name + " 的依赖清单(自进化闭环阶段 6 草稿)。\n";
    out += "# 草稿零第三方依赖。补实现时按需登记:只许默认注册表来源,\n";
    out += "# 版本库直链、明文直链、本地路径与改信任源的开关一律过不了静态门。\n";
    return out;
}

// ---------------------------------------------------------------------------
// 阶段 6 收官:MCP server 草稿的三份文本。同尺三判据,选路的形状是
// "簇内同求而无人成功的工具 >=2 件"——缺的是一项服务,不是一条命令,
// 封一只 stdio server 合账。协议铁律同 examples/packages/browser 的
// mcp 组件:stdio、newline 分隔 JSON-RPC、stdout 只出协议信、日志走
// stderr。草稿同样零进程零挂载,工具处理全是诚实的"未实现"占位。
// ---------------------------------------------------------------------------

// mcp.yaml(schema 1,契约 §5 的形状)。command 用 python + ${package_dir}
// 占位(契约只认 ${package_dir}/${package_data},没有 ${plugin_dir});
// permissions.network 恒 false(草稿不开网);不声明 env(占位 server
// 不需要)。落盘前过 ParseMcpComponentYaml,过不了就地回落 Skill-only。
std::string ComposeMcpYamlText(const std::string& server_id, const std::string& description,
                               int tasks_wanting, int tool_count) {
    std::string out;
    out += "# " + server_id + ":自进化闭环阶段 6 起草的 MCP server 草稿(stdio)。\n";
    out += "# 簇内 " + std::to_string(tasks_wanting) + " 场任务同求 " + std::to_string(tool_count) +
           " 件不存在的工具——缺的是\n";
    out += "# 一项服务,封一只 server 合账。草稿零执行零挂载:server.py 是未实现\n";
    out += "# 占位,补实现须人工;启用走 Package trust 与人工审查线。\n";
    out += "schema: 1\n";
    out += "id: " + server_id + "\n";
    out += "description: " + YamlDoubleQuote(description) + "\n";
    out += "transport: stdio\n";
    out += "runtime:\n";
    out += "  command: python\n";
    out += "  args:\n";
    out += "    - \"${package_dir}/mcp/" + server_id + "/server.py\"\n";
    out += "  timeout_ms: 30000\n";
    out += "permissions:\n";
    out += "  network: false\n";
    return out;
}

// server.py 脚手架。stdio 上 newline 分隔的 JSON-RPC(与 LubanCode 的 MCP
// transport 同款框架):一行一信;stdout 只写协议响应,日志只写 stderr;
// notification(无 id)不答。initialize 认客户端给的协议版本(不猜);
// tools/list 如实亮草稿工具;tools/call 全是 draft-not-implemented 占位。
std::string ComposeMcpServerPy(const std::string& server_id,
                               const std::vector<nlohmann::json>& draft_tools, int tasks_wanting) {
    std::string names;
    for (const nlohmann::json& tool : draft_tools) {
        names += (names.empty() ? "" : ", ") + tool.value("name", std::string());
    }
    std::string out;
    out += "# -*- coding: utf-8 -*-\n";
    out += "\"\"\"" + server_id + " MCP server 脚手架(自进化闭环阶段 6 草稿)。\n";
    out += "\n";
    out += "这份文件是草稿,不是成品:\n";
    out += "  - tools/call 全是诚实的\"未实现\"占位,补实现须人工完成;\n";
    out += "  - 人工审查线:除 Package trust 外,还须人读一遍本文件与 mcp.yaml;\n";
    out += "  - 协议铁律(同 examples/packages/browser 的 mcp 组件):stdio 上\n";
    out += "    newline 分隔的 JSON-RPC,一行一信;stdout 只写协议响应,日志只\n";
    out += "    写 stderr;notification(无 id)不答;initialize 回认客户端的\n";
    out += "    协议版本,不猜。\n";
    out += "\n";
    out += "来源:" + std::to_string(tasks_wanting) + " 场任务想用工具 " + names +
           "(现有工具办不了,录到的是\n";
    out += "registry.unknown_tool 失败);同求多件,封一只 server 合账。\n";
    out += "\"\"\"\n";
    out += "from __future__ import annotations\n";
    out += "\n";
    out += "import json\n";
    out += "import sys\n";
    out += "\n";
    out += "\n";
    out += "DRAFT_TOOLS = ";
    out += nlohmann::json(draft_tools).dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    out += "\n";
    out += "\n";
    out += "\n";
    out += "def reply(message):\n";
    out += "    sys.stdout.write(json.dumps(message, ensure_ascii=False) + \"\\n\")\n";
    out += "    sys.stdout.flush()\n";
    out += "\n";
    out += "\n";
    out += "def not_implemented(call_id, tool):\n";
    out += "    return {\n";
    out += "        \"jsonrpc\": \"2.0\",\n";
    out += "        \"id\": call_id,\n";
    out += "        \"result\": {\n";
    out += "            \"content\": [{\"type\": \"text\",\n";
    out += "                          \"text\": \"draft-not-implemented: 阶段 6 草稿,须人工补实现\"}],\n";
    out += "            \"isError\": True,\n";
    out += "        },\n";
    out += "    }\n";
    out += "\n";
    out += "\n";
    out += "def handle(request):\n";
    out += "    method = str(request.get(\"method\", \"\"))\n";
    out += "    call_id = request.get(\"id\")\n";
    out += "    if method == \"initialize\":\n";
    out += "        params = request.get(\"params\") or {}\n";
    out += "        return {\n";
    out += "            \"jsonrpc\": \"2.0\",\n";
    out += "            \"id\": call_id,\n";
    out += "            \"result\": {\n";
    out += "                \"protocolVersion\": str(params.get(\"protocolVersion\", \"\")),\n";
    out += "                \"capabilities\": {},\n";
    out += "                \"serverInfo\": {\"name\": \"" + server_id + "\", \"version\": \"0.1.0\"},\n";
    out += "            },\n";
    out += "        }\n";
    out += "    if method == \"tools/list\":\n";
    out += "        return {\"jsonrpc\": \"2.0\", \"id\": call_id, \"result\": {\"tools\": DRAFT_TOOLS}}\n";
    out += "    if method == \"tools/call\":\n";
    out += "        params = request.get(\"params\") or {}\n";
    out += "        return not_implemented(call_id, str(params.get(\"name\", \"\")))\n";
    out += "    if call_id is None:\n";
    out += "        return None\n";
    out += "    return {\"jsonrpc\": \"2.0\", \"id\": call_id,\n";
    out += "            \"error\": {\"code\": -32601, \"message\": \"method not found: \" + method}}\n";
    out += "\n";
    out += "\n";
    out += "def main():\n";
    out += "    for line in sys.stdin:\n";
    out += "        line = line.strip()\n";
    out += "        if not line:\n";
    out += "            continue\n";
    out += "        try:\n";
    out += "            request = json.loads(line)\n";
    out += "        except ValueError:\n";
    out += "            sys.stderr.write(\"bad json line\\n\")\n";
    out += "            continue\n";
    out += "        response = handle(request) if isinstance(request, dict) else None\n";
    out += "        if response is not None:\n";
    out += "            reply(response)\n";
    out += "    return 0\n";
    out += "\n";
    out += "\n";
    out += "if __name__ == \"__main__\":\n";
    out += "    raise SystemExit(main())\n";
    return out;
}

// MCP 草稿的 requirements.txt(与 Plugin 草稿同款:零依赖,给补实现的
// 人一个显眼的登记位;非注册表来源过不了静态门)。
std::string ComposeMcpRequirementsTxt(const std::string& server_id) {
    std::string out;
    out += "# " + server_id + " MCP server 的依赖清单(自进化闭环阶段 6 草稿)。\n";
    out += "# 草稿零第三方依赖(stdio 上的 JSON-RPC 只用标准库)。补实现时按需\n";
    out += "# 登记:只许默认注册表来源,版本库直链、明文直链、本地路径与改信任\n";
    out += "# 源的开关一律过不了静态门。\n";
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

// ---------------------------------------------------------------------------
// 阶段 6:尺三(代码档)判定与草稿组装
// ---------------------------------------------------------------------------

// 一场里"想用而不可得"的工具名与首次入参(ok=false 且 error_code 是
// registry.unknown_tool 的 tool_result 所配对的 tool_call)。
struct WantedToolCall {
    std::string tool;
    nlohmann::json first_input;
};

std::vector<WantedToolCall> CollectWantedToolCalls(const std::vector<skills::RecordEvent>& events) {
    // 配对口径与 CollectFoldedRuns 同款:录制件是串行工具循环,结果挂回
    // 最近一枚同名待配对调用(挂上即闭,不重复配)。
    struct Pending {
        std::string tool;
        nlohmann::json input;
        bool pending = true;
    };
    std::vector<Pending> pending;
    std::vector<WantedToolCall> wanted;
    for (const skills::RecordEvent& event : events) {
        if (event.type == skills::kEventToolCall) {
            const auto tool_it = event.data.find("tool");
            if (tool_it == event.data.end() || !tool_it->is_string()) {
                continue;
            }
            Pending item;
            item.tool = tool_it->get<std::string>();
            if (const auto input_it = event.data.find("input");
                input_it != event.data.end() && input_it->is_object()) {
                item.input = input_it.value();
            }
            pending.push_back(std::move(item));
        } else if (event.type == skills::kEventToolResult) {
            const auto tool_it = event.data.find("tool");
            if (tool_it == event.data.end() || !tool_it->is_string()) {
                continue;
            }
            const bool ok = event.data.contains("ok") && event.data.at("ok").is_boolean() &&
                            event.data.at("ok").get<bool>();
            const auto code_it = event.data.find("error_code");
            const bool unknown = !ok && code_it != event.data.end() && code_it->is_string() &&
                                 code_it->get<std::string>() == kUnknownToolErrorCode;
            if (!unknown) {
                continue;
            }
            for (std::size_t i = pending.size(); i > 0; --i) {
                Pending& item = pending[i - 1];
                if (item.tool != tool_it->get<std::string>() || !item.pending) {
                    continue;
                }
                item.pending = false;
                WantedToolCall call;
                call.tool = item.tool;
                call.first_input = item.input;
                wanted.push_back(std::move(call));
                break;
            }
        }
    }
    return wanted;
}

CodeCapabilitySignal AssessCodeCapability(const std::vector<ClusterTaskMaterial>& tasks) {
    CodeCapabilitySignal signal;
    std::map<std::string, int> wanting;   // 工具名 -> 有信号的场数(同场去重)
    std::vector<std::string> order;       // 首见次序(稳定挑选)
    std::map<std::string, std::vector<nlohmann::json>> inputs;  // 工具名 -> 各场首枚入参
    std::set<std::string> ever_succeeded;  // 全簇里成功过的工具名
    for (const ClusterTaskMaterial& task : tasks) {
        std::set<std::string> wanted_this_task;
        for (const WantedToolCall& call : CollectWantedToolCalls(task.events)) {
            if (wanting.find(call.tool) == wanting.end()) {
                order.push_back(call.tool);
            }
            if (wanted_this_task.insert(call.tool).second) {
                wanting[call.tool] += 1;
                inputs[call.tool].push_back(call.first_input);
            }
        }
        for (const skills::RecordEvent& event : task.events) {
            if (event.type != skills::kEventToolResult ||
                !(event.data.contains("ok") && event.data.at("ok").is_boolean() &&
                  event.data.at("ok").get<bool>())) {
                continue;
            }
            if (const auto tool_it = event.data.find("tool");
                tool_it != event.data.end() && tool_it->is_string()) {
                ever_succeeded.insert(tool_it->get<std::string>());
            }
        }
    }
    // 挑场数最多的那件(并列取首见)。
    std::string best;
    int best_count = 0;
    for (const std::string& tool : order) {
        if (wanting[tool] > best_count) {
            best = tool;
            best_count = wanting[tool];
        }
    }
    if (best.empty()) {
        signal.why_not.push_back("簇内没有 registry.unknown_tool 的稳定失败——现有工具够用,"
                                 "不生 Plugin(§3.5:现有工具能办,只是提示词没写好)");
        return signal;
    }
    signal.wanted_tool = best;
    signal.tasks_wanting = best_count;
    // 全簇没人成功用过它(工具不存在,自然无人成功;有成功记录就不算新能力)。
    if (ever_succeeded.count(best) != 0) {
        signal.why_not.push_back("工具 " + best + " 在簇内成功过——现有工具办得了,不生 Plugin");
        return signal;
    }
    if (best_count < 2) {
        signal.why_not.push_back("只有 " + std::to_string(best_count) +
                                 " 场任务想用工具 " + best +
                                 "(>=2 场才起草插件草稿;单场偶发照旧 Skill-only)");
        return signal;
    }
    // 判据过了:同求而无人成功的全部工具名(首见次序)。>=2 件是"缺一项
    // 服务"的形状,走 MCP 路合账;恰一件走 process Plugin 路。
    for (const std::string& tool : order) {
        if (ever_succeeded.count(tool) == 0 && wanting[tool] >= 1) {
            signal.wanted_tools.push_back(tool);
        }
    }
    // 入参形状:各场首枚入参的键,按首见次序(给 runner/server 脚手架的文档行)。
    std::vector<std::string> keys;
    for (const nlohmann::json& input : inputs[best]) {
        if (!input.is_object()) {
            continue;
        }
        for (auto it = input.begin(); it != input.end(); ++it) {
            if (std::find(keys.begin(), keys.end(), it.key()) == keys.end()) {
                keys.push_back(it.key());
            }
        }
    }
    signal.inputs_note = keys;
    signal.eligible = true;
    return signal;
}

// 组装草稿三份文本与权限差异。落不成(wire 名超帽、名字洗不出)给 false,
// why_not 记话,候选照旧走组合档/最小档——不硬塞。
bool ComposePluginDraft(ComboCandidateDraft& draft, const std::vector<ClusterTaskMaterial>& tasks) {
    const CodeCapabilitySignal& signal = draft.code_signal;
    const std::string tool_name = SanitizeToolName(signal.wanted_tool);
    const std::string plugin_dir = SanitizePluginDirName(signal.wanted_tool);
    if (tool_name.empty() || plugin_dir.empty()) {
        draft.code_signal.why_not.push_back("想要的工具名洗不成合法插件名,草稿不硬塞");
        return false;
    }
    // wire 名(plugin__<包段>__<工具>)有 64 字符帽,超帽的草稿落了也过不了
    // doctor——在这里就收掉。
    const std::string wire = lubancode::runtime::BuildPackagedToolWireName(
        "plugin", draft.package_id, plugin_dir, tool_name);
    if (wire.size() > lubancode::runtime::kToolWireNameMaxLength) {
        draft.code_signal.why_not.push_back("工具 wire 名 \"" + wire + "\" 超 " +
                                            std::to_string(lubancode::runtime::kToolWireNameMaxLength) +
                                            " 字符帽,插件草稿不硬塞(改名重试)");
        return false;
    }
    // input schema:照观察到的入参形状(键 -> 类型);全簇都在的键 required。
    nlohmann::json schema;
    schema["type"] = "object";
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();
    std::map<std::string, int> key_tasks;  // 键 -> 出现场数
    std::vector<nlohmann::json> observed;  // 各场首枚入参
    for (const ClusterTaskMaterial& task : tasks) {
        for (const WantedToolCall& call : CollectWantedToolCalls(task.events)) {
            if (call.tool != signal.wanted_tool || !call.first_input.is_object()) {
                continue;
            }
            observed.push_back(call.first_input);
            break;  // 一场一枚
        }
    }
    for (const nlohmann::json& input : observed) {
        for (auto it = input.begin(); it != input.end(); ++it) {
            key_tasks[it.key()] += 1;
            if (properties.contains(it.key())) {
                continue;
            }
            nlohmann::json property;
            if (it.value().is_number_integer()) {
                property["type"] = "integer";
            } else if (it.value().is_number_float()) {
                property["type"] = "number";
            } else if (it.value().is_boolean()) {
                property["type"] = "boolean";
            } else {
                property["type"] = "string";
                property["description"] = "观察到的入参(草稿按各场实录记形状)";
            }
            properties[it.key()] = property;
        }
    }
    for (const auto& [key, count] : key_tasks) {
        if (count >= static_cast<int>(observed.size()) && !observed.empty()) {
            required.push_back(key);
        }
    }
    schema["properties"] = properties;
    if (!required.empty()) {
        schema["required"] = required;
    }
    schema["additionalProperties"] = false;

    const std::string env_name = MakeDraftEnvName(plugin_dir);
    std::string description = "从 " + std::to_string(signal.tasks_wanting) +
                              " 场任务观察到的执行能力需求(" + TruncateChars(signal.wanted_tool, 40) +
                              ");现有工具办不了";
    draft.plugin_id = plugin_dir;
    draft.plugin_json = ComposePluginJson(plugin_dir, tool_name, description, schema, env_name,
                                          signal.tasks_wanting);
    draft.plugin_runner = ComposeRunnerPy(plugin_dir, tool_name, signal.inputs_note,
                                          signal.tasks_wanting);
    draft.plugin_requirements = ComposeRequirementsTxt(plugin_dir);
    // 权限差异(evolution.json 的 changes;一条一权,只记名不记值)。
    draft.permissions_added.push_back("process:python");
    draft.permissions_added.push_back("env:" + env_name);
    for (const std::string& key : signal.inputs_note) {
        if (key == "path" || key == "file" || key == "dir" || key == "filename" ||
            key == "root" || key == "directory") {
            draft.permissions_added.push_back("fs_read:workspace");
            break;
        }
    }
    draft.tools_added.push_back(wire);
    draft.with_plugin_draft = true;
    return true;
}

// 一件工具的 input schema:照各场实录的入参形状(键 -> 类型),全簇都在
// 的键 required(与 Plugin 草稿同一口径,单件工具版)。
nlohmann::json BuildToolInputSchema(const std::vector<ClusterTaskMaterial>& tasks,
                                    const std::string& wanted_tool) {
    nlohmann::json schema;
    schema["type"] = "object";
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();
    std::map<std::string, int> key_tasks;
    std::vector<nlohmann::json> observed;
    for (const ClusterTaskMaterial& task : tasks) {
        for (const WantedToolCall& call : CollectWantedToolCalls(task.events)) {
            if (call.tool != wanted_tool || !call.first_input.is_object()) {
                continue;
            }
            observed.push_back(call.first_input);
            break;  // 一场一枚
        }
    }
    for (const nlohmann::json& input : observed) {
        for (auto it = input.begin(); it != input.end(); ++it) {
            key_tasks[it.key()] += 1;
            if (properties.contains(it.key())) {
                continue;
            }
            nlohmann::json property;
            if (it.value().is_number_integer()) {
                property["type"] = "integer";
            } else if (it.value().is_number_float()) {
                property["type"] = "number";
            } else if (it.value().is_boolean()) {
                property["type"] = "boolean";
            } else {
                property["type"] = "string";
                property["description"] = "观察到的入参(草稿按各场实录记形状)";
            }
            properties[it.key()] = property;
        }
    }
    for (const auto& [key, count] : key_tasks) {
        if (!observed.empty() && count >= static_cast<int>(observed.size())) {
            required.push_back(key);
        }
    }
    schema["properties"] = properties;
    if (!required.empty()) {
        schema["required"] = required;
    }
    schema["additionalProperties"] = false;
    return schema;
}

// 组装 MCP server 草稿三份文本与权限差异。落不成(wire 名超帽、名字洗
// 不出、mcp.yaml 过不了解析)给 false,why_not 记话,候选照旧走组合档/
// 最小档——不硬塞。MCP 草稿与 Plugin 草稿互斥:一只候选只带一种代码件。
bool ComposeMcpDraft(ComboCandidateDraft& draft, const std::vector<ClusterTaskMaterial>& tasks) {
    const CodeCapabilitySignal& signal = draft.code_signal;
    if (signal.wanted_tools.size() < 2) {
        draft.code_signal.why_not.push_back("同求而无人成功的工具不足两件——缺的是一条命令,"
                                            "不封 server(process Plugin 路)");
        return false;
    }
    // server 名照求的人最多的那件(与 Plugin 路同名同源,账好对)。
    const std::string server_id = SanitizePluginDirName(signal.wanted_tool);
    if (server_id.empty()) {
        draft.code_signal.why_not.push_back("想要的工具名洗不成合法 server 名,草稿不硬塞");
        return false;
    }
    // 先验 wire 名帽(mcp__<包段>__<server>__<工具>):超帽的草稿落了也
    // 过不了 doctor,在这里就收掉。
    for (const std::string& wanted : signal.wanted_tools) {
        const std::string wire = lubancode::runtime::BuildPackagedToolWireName(
            "mcp", draft.package_id, server_id, SanitizeToolName(wanted));
        if (wire.size() > lubancode::runtime::kToolWireNameMaxLength) {
            draft.code_signal.why_not.push_back("工具 wire 名 \"" + wire + "\" 超 " +
                                                std::to_string(
                                                    lubancode::runtime::kToolWireNameMaxLength) +
                                                " 字符帽,MCP 草稿不硬塞(改名重试)");
            return false;
        }
    }
    // tools/list 的草稿工具账:一件一行,入参形状照各场实录。
    std::vector<nlohmann::json> draft_tools;
    std::vector<std::string> tool_names;
    for (const std::string& wanted : signal.wanted_tools) {
        const std::string tool_name = SanitizeToolName(wanted);
        nlohmann::json tool;
        tool["name"] = tool_name;
        tool["description"] = "MCP server 草稿工具:源自 " + TruncateChars(wanted, 40) +
                              " 的稳定需求;脚手架未实现,补实现须过人工审查";
        tool["inputSchema"] = BuildToolInputSchema(tasks, wanted);
        draft_tools.push_back(std::move(tool));
        tool_names.push_back(tool_name);
    }

    std::string names_note;
    for (const std::string& name : tool_names) {
        names_note += (names_note.empty() ? "" : ", ") + name;
    }
    const std::string description = "从 " + std::to_string(signal.tasks_wanting) +
                                    " 场任务观察到的执行能力需求(" + TruncateChars(names_note, 60) +
                                    ");现有工具办不了,同求多件封一只 server";
    draft.mcp_id = server_id;
    draft.mcp_yaml = ComposeMcpYamlText(server_id, description, signal.tasks_wanting,
                                        static_cast<int>(tool_names.size()));
    // mcp.yaml 落盘前过严格解析(占位符/越界/字段形状;与盘上同一枚 parser,
    // 不另立规矩)。占位检查要包根,草稿层给字面占位即可。
    if (!lubancode::package::ParseMcpComponentYaml(
             draft.mcp_yaml, std::filesystem::path("package"))
             .has_value()) {
        draft.code_signal.why_not.push_back("mcp.yaml 草稿过不了严格解析,不硬塞");
        return false;
    }
    draft.mcp_server = ComposeMcpServerPy(server_id, draft_tools, signal.tasks_wanting);
    draft.mcp_requirements = ComposeMcpRequirementsTxt(server_id);
    // 权限差异(evolution.json 的 changes;一条一权,只记名不记值)。
    draft.permissions_added.push_back("process:python");
    for (const std::string& key : signal.inputs_note) {
        if (key == "path" || key == "file" || key == "dir" || key == "filename" ||
            key == "root" || key == "directory") {
            draft.permissions_added.push_back("fs_read:workspace");
            break;
        }
    }
    for (const std::string& tool_name : tool_names) {
        draft.tools_added.push_back(lubancode::runtime::BuildPackagedToolWireName(
            "mcp", draft.package_id, server_id, tool_name));
    }
    draft.with_mcp_draft = true;
    return true;
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

    // ---- 尺三(代码档,§3.5):判据过了选路——同求而无人成功的工具
    //      >=2 件封 MCP server 草稿(缺一项服务),恰一件落 process Plugin
    //      草稿(缺一条命令)。代码档不叠组合档:代码件还没真身,编排等
    //      它落地后的下一只候选;此刻最小答案是 Skill + 草稿。 ----
    draft.code_signal = AssessCodeCapability(tasks);
    if (draft.code_signal.eligible && draft.code_signal.wanted_tools.size() >= 2 &&
        ComposeMcpDraft(draft, tasks)) {
        return draft;
    }
    if (draft.code_signal.eligible && ComposePluginDraft(draft, tasks)) {
        return draft;
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
