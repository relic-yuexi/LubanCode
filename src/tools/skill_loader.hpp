// M9:技能系统。技能是一份预先写好的说明书(SKILL.md),按需被模型通过
// skill 工具加载进来,再照着做——用途上类似"给模型的一份操作手册",跟
// hooks(外部命令钩子)是两回事。
//
// 目录约定:
//   <程序资源目录>/skills/<技能名>/SKILL.md       (官方级,随发行包更新)
//   <主目录>/.lubancode/skills/<技能名>/SKILL.md   (主目录级,所有项目都能用)
//   <cwd>/.lubancode/skills/<技能名>/SKILL.md       (项目级,只在当前项目生效)
// 同名时项目级覆盖主目录级,主目录级覆盖官方级。旧版曾播种到主目录的
// 官方维护副本不算用户覆盖,加载时会让位给发行包里的新版本。
//
// 放在 tools/ 而不是 cli/ 的理由:skill 工具本身(下面的 SkillTool,见
// skill_tool.hpp)执行时要读取已扫描到的清单,是 tools 层的东西;main.cpp
// 在启动时调 LoadSkills() 一次,用结果同时喂给系统提示词(agent 层)和
// SkillTool 的构造(tools 层)、以及 /skills 命令(cli 层展示),tools 是
// 三者共同的下游,放这里不产生新的层间依赖(cli/agent 本来就可以用
// tools 的东西,tools 不需要认得 cli/agent)。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::tools {

// 一份技能的清单信息(不含正文——正文按需现读,清单只管"有哪些技能、
// 叫什么、干什么用")。
struct SkillMeta {
    std::string name;
    std::string description;
    std::string dir_path;      // 技能目录的绝对路径(UTF-8),SKILL.md 就在这个目录下
    std::string source_level;  // "官方"、"项目级" 或 "主目录级",/skills 展示用
    bool managed_official_copy = false;  // 旧版播种进主目录的官方维护副本
};

// SKILL.md 的 frontmatter 解析结果。name/description 缺失时是
// std::nullopt(调用方决定怎么兜底:name 缺省用目录名,description 缺省是
// 空串)。body 是 frontmatter 之后的正文;没有 frontmatter 时 body 就是
// 整篇原文。
struct ParsedSkillFile {
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::string body;
};

// 手写的小 YAML frontmatter 解析器,只认 --- 定界的块、块内 name/
// description 两个 key(冒号分隔,值两端的引号会被去掉)。
// 返回 std::nullopt 专指"起了 --- 头却找不到闭合的 ---"这种损坏结构——
// 调用方应该跳过整个技能、打一条警告;内容压根不是以 --- 开头,视为
// "没有 frontmatter",body 就是整篇原文,不算错。
std::optional<ParsedSkillFile> ParseSkillMarkdown(const std::string& content);

// 扫描一个 skills 根目录(<root>/<技能名>/SKILL.md),source_level 是打进
// SkillMeta 里的来源标签。根目录不存在,原样返回空 vector,不报错。单个
// 技能的 SKILL.md 读不到/frontmatter 损坏,跳过那一个、打一行警告
// (std::cerr),不影响其余技能、不崩。
std::vector<SkillMeta> ScanSkillsDir(const std::filesystem::path& skills_root, const std::string& source_level);

// 主入口:official_skills_dir(发行包官方技能,可能没有)、home_dir(主目录,
// 可能没有)与 project_dir(cwd)三层合并。优先级:项目 > 主目录 > 官方。
// 旧版落在主目录的官方维护副本会自动让位。返回结果按名字排序。
std::vector<SkillMeta> LoadSkills(const std::string& project_dir, const std::optional<std::string>& home_dir,
                                  const std::optional<std::string>& official_skills_dir = std::nullopt);

// 系统提示词里"可用技能"这一段。skills 为空时返回空串——一个字都不注入,
// 不影响没配技能的既有场景。
std::string BuildSkillsPromptSegment(const std::vector<SkillMeta>& skills);

}  // namespace lubancode::tools
