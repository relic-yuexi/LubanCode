// 自进化闭环阶段 2:EvolutionDrafter 的纯函数芯——一场录制 -> 最小
// content-only Package 的两份文本(package.yaml 与 skills/<id>/SKILL.md)。
//
// 规矩(todo §八"如何从经验长成 Package"):
//   - 最小包:能靠一份 Skill 办成就只放 Skill,不添 Agent、不生 Plugin。
//   - 复用现有 Skill drafter:正文来自 skills::ComposeSkillMarkdown——
//     偶然值抽象(日期/网址/绝对路径→{{date}}/{{url}}/{{path}},cwd 剥成
//     相对路径)、失败重试折叠全在里头;这里只补一节"排错",收录录制里
//     连败不附成功的稳定失败路(skills::CollectStableFailureModes)。
//   - 落盘的事不归这里:碰磁盘的唯一写口是 EvolutionCoordinator。
//   - 产物必须过两道既有严格解析:package::ParsePackageManifest(schema 1)
//     与 skills::ValidateSkillMarkdownForInstall。过不了就在这里收掉,
//     绝不落盘。
#pragma once

#include <expected>
#include <string>
#include <vector>

#include "skills/workflow_recorder.hpp"

namespace lubancode::evolution {

// 起草产物:两份文本 + 演化账要用的身份字段。
struct SkillCandidateDraft {
    std::string skill_slug;       // skill 名(Agent Skills 规范 slug)= 包内目录名
    std::string skill_markdown;   // skills/<slug>/SKILL.md 全文
    std::string package_yaml;     // package/package.yaml 全文
    std::string package_id;       // "evolve.<slug>"(两段式)
    std::string package_version;  // 无父首版,固定 0.1.0
    std::string objective;        // evolution.json 的 objective(脱敏一句话)
};

// 从一场录制起草最小 content-only Package(纯函数,不碰磁盘)。
// 录制没录完(没有 record_stop)返回错误——半截示范不是可复用做法。
std::expected<SkillCandidateDraft, std::string> DraftSkillCandidate(const skills::RecordingStatus& status,
                                                                    const std::vector<skills::RecordEvent>& events);

}  // namespace lubancode::evolution
