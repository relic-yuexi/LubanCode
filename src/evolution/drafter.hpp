// 自进化闭环阶段 2/5:EvolutionDrafter 的纯函数芯。
//   阶段 2:一场录制 -> 最小 content-only Package 的两份文本(package.yaml
//           与 skills/<id>/SKILL.md)。
//   阶段 5:同 fingerprint 簇攒够门槛且步骤序列同形 -> 组合包(Skill +
//           Workflow [+ Agent]);够不着门槛的照旧最小包。
//
// 规矩(todo §三/§八"如何从经验长成 Package"):
//   - 最小包:能靠一份 Skill 办成就只放 Skill,不添 Agent、不生 Plugin。
//     Agent 是升档不是标配——全场工具面在多场间同形才提炼。
//   - 复用现有 Skill drafter:正文来自 skills::ComposeSkillMarkdown——
//     偶然值抽象(日期/网址/绝对路径→{{date}}/{{url}}/{{path}},cwd 剥成
//     相对路径)、失败重试折叠全在里头;这里只补一节"排错",收录录制里
//     连败不附成功的稳定失败路(skills::CollectStableFailureModes)。
//   - 组合件的偶然值也抽:各场同值的入参留字面量,异值的提成 workflow
//     输入(${inputs.…});不把录制现场焊死进 workflow。
//   - 落盘的事不归这里:碰磁盘的唯一写口是 EvolutionCoordinator。组合件
//     落盘后还须过 AnalyzePackage(引用闭合、canonical 名、无越界),
//     过不了由写口降回 Skill-only。
//   - 产物必须过两道既有严格解析:package::ParsePackageManifest(schema 1)
//     与 skills::ValidateSkillMarkdownForInstall。过不了就在这里收掉,
//     绝不落盘。
#pragma once

#include <expected>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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

// ---------------------------------------------------------------------------
// 阶段 5:同形多场 -> 组合包(Skill + Workflow [+ Agent])
//
// 提炼门槛两把尺(todo §三/§八、README 五档决策表):
//   尺一(Workflow 档):簇 >= 2 场独立任务,且各场的"成功路折叠序列"同形
//     ——工具名序列经连续同名折叠后完全一致(名字、次数、次序),且只数
//     最终成功的步子;连败不附成功的工具不在成功路上,进失败路。步数
//     不足两步不算"编排"(单步的稳定做法归 Skill)。
//   尺二(Agent 档,更严):在尺一之上,各场的"全场工具面"也同形——含
//     失败尝试在内出现过的每一个工具名都一致。编排看成功路,角色看整个
//     工具面:失败重试里摸过的工具也是这只角色的习惯,面不同就不封同一
//     只 Agent。一次任务不造 Agent(todo §3.4)。
// 门槛只是起草门;过门仍要走评测与批准。够不着门槛的,最小可行包
// (Skill-only)仍是默认答案。
// ---------------------------------------------------------------------------

// 簇内一场任务的输入材料(status 携 id/name,events 是整场事件流)。
struct ClusterTaskMaterial {
    skills::RecordingStatus status;
    std::vector<skills::RecordEvent> events;
};

// 成功路上的一步:折叠段(连续同名 tool_call)折成一步,留首枚入参。
struct SequencedToolStep {
    std::string tool;
    nlohmann::json first_input = nlohmann::json::object();   // 已脱敏(录制件入盘前过 SanitizeToolInput)
    nlohmann::json merged_input = nlohmann::json::object();  // 各场对齐合成后的节点入参(起草内部用)
};

// 一场录制的成功路折叠序列(纯函数;与观察指纹同款"连续同名折叠"口径,
// 再按"该步最后一次结果成败"筛掉连败不附成功的工具)。
std::vector<SequencedToolStep> SuccessPathSteps(const std::vector<skills::RecordEvent>& events);

// 一场录制的全场工具面:含失败尝试在内出现过的每一个工具名,保序去重。
std::vector<std::string> ToolFace(const std::vector<skills::RecordEvent>& events);

// 两把尺的判定账(纯函数;人话逐条,给 diff/show 与降档诊断用)。
struct ComboThreshold {
    int cluster_size = 0;
    bool sequences_stable = false;  // 尺一之前半:各场成功路折叠序列同形
    bool faces_stable = false;      // 尺二:各场全场工具面同形
    bool workflow_eligible = false; // 尺一:sequences_stable + 簇>=2 + 步数>=2
    bool agent_eligible = false;    // 尺二:workflow_eligible + faces_stable
    int workflow_steps = 0;         // 首场成功路的步数(同形即各场一致)
    std::vector<std::string> why_not;  // 不够格的人话(逐条;够格则空)
};
ComboThreshold AssessComboThreshold(const std::vector<ClusterTaskMaterial>& tasks);

// 组合起草产物:SkillCandidateDraft 的超集。workflow/agent 空串 = 没起草
// (门槛不够,或组合只在 Coordinator 落盘后经静态门降档——降档是写口的事,
// 纯函数层不知道)。落盘前必须过 AnalyzePackage(引用闭合、canonical 名、
// 无越界);过不了的草稿由 Coordinator 降回 Skill-only,不硬塞。
struct ComboCandidateDraft {
    // 最小包部分(与 SkillCandidateDraft 同款字段)
    std::string skill_slug;
    std::string skill_markdown;
    std::string package_yaml;
    std::string package_id;
    std::string package_version;
    std::string objective;
    // 组合部分
    std::string workflow_id;         // workflows/<id>/ 的 <id>;空 = 不带 workflow
    std::string workflow_yaml;       // workflows/<id>/workflow.yaml 全文
    bool with_agent = false;         // 尺二过了才置真
    std::string agent_name;          // agents/<name>.yaml 的 name;空 = 不带 Agent
    std::string agent_yaml;          // agents/<name>.yaml 全文
    // 起草账
    ComboThreshold threshold;
    std::vector<std::string> recording_ids;  // 簇内全部来源(顺序:点名场在前)
};

// 从一个簇起草候选(纯函数,不碰磁盘)。簇须同 fingerprint(写口会验;
// 这里只看形状)。首场(点名场)当代表:SKILL 正文照它起,组合件从各场
// 对齐合成。录制没录完返回错误——半截示范不是可复用做法。
std::expected<ComboCandidateDraft, std::string> DraftEvolutionCandidate(
    const std::vector<ClusterTaskMaterial>& tasks);

}  // namespace lubancode::evolution
