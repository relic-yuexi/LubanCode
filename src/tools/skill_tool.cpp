#include "tools/skill_tool.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "tools/path_utils.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

std::string SkillTool::name() const {
    return "skill";
}

std::string SkillTool::description() const {
    // 文案在 src/prompts/tools/<语言>/skill.md,兜底是迁移前的原文。
    return ToolText("skill", "description",
                    "按名字加载一份已发现的技能(SKILL.md),拿到它的完整使用说明。技能是预先写好的一套具体做法"
                    "(比如某种文体的写作规范、某类任务的固定流程),系统提示里列出的技能名/说明跟当前任务对得上时,"
                    "先调用这个工具把说明读进来,再照着做。");
}

nlohmann::json SkillTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json name_prop = nlohmann::json::object();
    name_prop["type"] = "string";
    name_prop["description"] = ToolText("skill", "param.name", "要加载的技能名,跟系统提示里列出的名字一致");
    properties["name"] = name_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"name"});

    return schema;
}

Tool::Result SkillTool::execute(const nlohmann::json& input) {
    if (!input.contains("name") || !input.at("name").is_string()) {
        return {"缺少必填参数 name(字符串)", true};
    }
    const std::string name = input.at("name").get<std::string>();

    const auto it = std::find_if(skills_.begin(), skills_.end(),
                                  [&](const SkillMeta& meta) { return meta.name == name; });
    if (it == skills_.end()) {
        if (skills_.empty()) {
            return {"没有名叫 " + name + " 的技能——当前没有扫描到任何技能。", true};
        }
        std::string available;
        for (const auto& meta : skills_) {
            if (!available.empty()) {
                available += "、";
            }
            available += meta.name;
        }
        return {"没有名叫 " + name + " 的技能,可用的有: " + available, true};
    }

    const std::filesystem::path skill_md = Utf8ToPath(it->dir_path) / "SKILL.md";
    std::ifstream file(skill_md, std::ios::binary);
    if (!file.is_open()) {
        return {"技能目录还在,但 SKILL.md 读不到了: " + it->dir_path, true};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();

    const auto parsed = ParseSkillMarkdown(content);
    const std::string body = parsed.has_value() ? parsed->body : content;

    const std::string result = "技能目录: " + it->dir_path + "(技能内相对路径以此为基准)\n" + body;
    return {result, false};
}

}  // namespace lubancode::tools
