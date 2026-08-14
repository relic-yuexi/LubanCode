// 录一遍生成技能(0.25.x):把录制件(events.jsonl)归纳成 SKILL.md 草稿。
//
// 起草器做的是归纳,不是照抄:
//   - 演示里的具体值(日期、网址、cwd 外的绝对路径)提成输入变量,正文
//     只留 {{date}}/{{url}}/{{path}} 占位,cwd 打头的绝对路径改写成相对路径;
//   - 偶然的失败重试剔掉(同工具同入参"失败→成功"折成一步,失败摘要在
//     括号里留一句);连败不附成功的,写成"若失败"分支;
//   - 模型的长篇回答一个字不进正文——工具结果只用了录制件里那行短摘要;
//   - 每份 skill 都有验收节(开录口述的成事标准 + 最后一次验证结果),
//     没有验收的草稿过不了 ValidateSkillMarkdownForInstall,装不进 skills。
//
// 产物先过现有 skill 解析器(tools::ParseSkillMarkdown):frontmatter 损坏
// 或缺 name/description 时回炉一次(按干净数据重建 frontmatter),再不过
// 就报错,绝不落盘。
//
// 分层:Compose/Abstract/Validate 全是纯函数(单测钉死);WriteSkillDraft
// 是碰磁盘的薄壳,只写 <录制件>/draft/SKILL.md。安装另走
// config::InstallDraftSkill(skill_store),这里不碰 skills 目录。

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "agent/workflow_recorder.hpp"

namespace lubancode::agent {

// 草稿"要填的输入"一项:占位名 + 说明 + 演示里的具体值(已脱敏、已抽象)。
struct DraftVariable {
    std::string name;         // 如 "{{date}}"
    std::string description;  // 什么时候要换它
    std::string example;      // 演示里出现过的值
};

// 具体值抽象(纯函数):
//   - 日期 \d{4}-\d{2}-\d{2}(与 /\d{4}\/\d{2}\/\d{2}/)→ {{date}}
//   - http(s) 网址 → {{url}}
//   - cwd 打头的绝对路径 → 剥掉 cwd 留相对部分
//   - 其余绝对路径(盘符:\ 或 / 开头的字词)→ {{path}}
// 新发现的变量补进 extracted(去重,example 记首次出现的值)。cwd 为空时
// 绝对路径一律按"其余"办。
std::string AbstractConcreteValues(const std::string& text, const std::string& cwd,
                                   std::vector<DraftVariable>& extracted);

// 校验一份 SKILL.md 全文能不能装(纯函数):过 tools::ParseSkillMarkdown,
// frontmatter 完好、name/description 齐全,且正文有"验收"节。合格返回
// frontmatter 里的 name(缺省回落到空串由调用方再兜),不合格返回错误。
std::expected<std::string, std::string> ValidateSkillMarkdownForInstall(const std::string& content);

// 归纳起草(纯函数):录制事件流 → SKILL.md 全文。事件流为空返回空串
// (调用方判空)。不碰磁盘。
std::string ComposeSkillMarkdown(const std::vector<RecordEvent>& events);

// 回炉(纯函数):frontmatter 推倒重建(名字回落 recorded-skill、描述回落
// 内置句),正文尽力保住——过得了解析器就用解析出的 body,过不了就掐掉
// 头两个 '---' 行之间的东西。
std::string RepairSkillFrontmatter(const std::string& content);

// 落草稿:Compose + Validate(不合法回炉一次),过了写
// <recording_dir>/draft/SKILL.md(整文件覆盖),返回草稿目录与将随 skill
// 带走的文件清单(现版只有 SKILL.md 一份,后头脚本/模板也走这个口)。
struct SkillDraftResult {
    std::filesystem::path draft_dir;
    std::vector<std::string> files;  // 相对 draft_dir,正斜杠分隔
};
std::expected<SkillDraftResult, std::string> WriteSkillDraft(const std::filesystem::path& recording_dir,
                                                             const std::vector<RecordEvent>& events);

}  // namespace lubancode::agent
