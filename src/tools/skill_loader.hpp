// M9:技能系统。技能是一份预先写好的说明书(SKILL.md),按需被模型通过
// skill 工具加载进来,再照着做——用途上类似"给模型的一份操作手册",跟
// hooks(外部命令钩子)是两回事。
//
// 目录约定:
//   <程序资源目录>/skills/<技能名>/SKILL.md       (官方级,随发行包更新)
//   <主目录>/.agents/skills/<技能名>/SKILL.md      (跨客户端主目录级)
//   <主目录>/.lubancode/skills/<技能名>/SKILL.md   (LubanCode 主目录级)
//   <cwd>/.agents/skills/<技能名>/SKILL.md          (跨客户端项目级)
//   <cwd>/.lubancode/skills/<技能名>/SKILL.md       (LubanCode 项目级)
// 同名时项目级覆盖主目录级,主目录级覆盖官方级;同一层里 LubanCode 原生
// 目录覆盖 .agents 共享目录。旧版曾播种到主目录的官方维护副本不算用户
// 覆盖,加载时会让位给发行包里的新版本。
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
// std::nullopt。body 是 frontmatter 之后的正文;没有 frontmatter 时 body
// 就是整篇原文,由扫描层按缺必填元数据处理。
struct ParsedSkillFile {
    std::optional<std::string> name;
    std::optional<std::string> description;
    std::string body;
};

// 用 yaml-cpp 解析 --- 定界的 YAML frontmatter,提取 name/description。
// 对别家客户端遗留的“description: 值里另有冒号”坏 YAML 留一条扁平
// key:value 回退,提高跨客户端兼容。结构彻底损坏时返回 std::nullopt。
std::optional<ParsedSkillFile> ParseSkillMarkdown(const std::string& content);

// Agent Skills 规范里的 name 语法:1-64 个 ASCII 小写字母/数字/横线，
// 横线不顶头、不收尾、不连写。扫描层拿它做诊断；录制起草拿它硬校验。
bool IsValidAgentSkillName(const std::string& name);

// 扫描一个 skills 根目录(<root>/<技能名>/SKILL.md),source_level 是打进
// SkillMeta 里的来源标签。根目录不存在,原样返回空 vector,不报错。单个
// 技能的 SKILL.md 读不到、frontmatter 损坏或缺必填元数据,跳过那一个、
// 打一行警告,不影响其余技能。name 的格式或目录名不合规范时宽容加载,
// 但也记警告,与 Agent Skills 客户端接入指南一致。
std::vector<SkillMeta> ScanSkillsDir(const std::filesystem::path& skills_root, const std::string& source_level);

// 主入口:official_skills_dir(发行包官方技能,可能没有)、home_dir(主目录,
// 可能没有)与 project_dir(cwd)合并官方、.agents 与 .lubancode 五处。
// 优先级:项目原生 > 项目共享 > 用户原生 > 用户共享 > 官方。旧版落在
// 主目录的官方维护副本会自动让位。返回结果按名字排序。
std::vector<SkillMeta> LoadSkills(const std::string& project_dir, const std::optional<std::string>& home_dir,
                                  const std::optional<std::string>& official_skills_dir = std::nullopt);

// 系统提示词里"可用技能"这一段。skills 为空时返回空串——一个字都不注入,
// 不影响没配技能的既有场景。
std::string BuildSkillsPromptSegment(const std::vector<SkillMeta>& skills);

// 读一份技能的正文(frontmatter 之后的 body)。读不到、frontmatter 损坏都
// 返回 nullopt——预装侧(自定义 Agent 的 skills.preload)据此降级:只登记
// 名字不注正文,不挡派发。
std::optional<std::string> ReadSkillBody(const SkillMeta& meta);

}  // namespace lubancode::tools
