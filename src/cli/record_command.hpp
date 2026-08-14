// /record 命令组(0.25.x"录一遍生成技能")的执行壳:问三句话、驱动
// agent::WorkflowRecorder 状态机、停止后起草/预览/确认安装。
//
// 纯逻辑(状态机、事件、脱敏、归纳起草、原子安装)分别在
// agent/workflow_recorder、agent/skill_drafter、config/skill_store,这里只
// 做 IO 接线——问话、打印、确认,不掺业务规矩。编进可执行文件(不进
// lubancode_core),跟 console_input 一个待遇:它要横跨 cli/agent/config
// 三层,单测钉的是那三层的纯函数,不钉这份壳。

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "agent/workflow_recorder.hpp"
#include "cli/theme.hpp"

namespace lubancode::cli {

struct RecordCommandContext {
    // 会话里那一场录制(InteractiveLoop 持有,这里借引用驱动)。
    std::optional<lubancode::agent::WorkflowRecorder>& recorder;
    std::filesystem::path recordings_root;  // <主目录>/.lubancode/recordings;空 = 无主目录
    std::filesystem::path project_skills_root;  // <cwd>/.lubancode/skills
    std::filesystem::path home_skills_root;     // <主目录>/.lubancode/skills
    std::function<void()> refresh_skills;       // 装好后刷新本场技能清单
};

// 解析与执行 /record 的二级命令。args 是 /record 之后的那一段。
void HandleRecordCommand(const std::string& args, RecordCommandContext& ctx, const Theme& theme);

// 状态栏 REC 短标记:录制中 "REC · 名字",暂停 "REC 已停"(i18n),没录空串。
std::string RecorderStatusMarker(const std::optional<lubancode::agent::WorkflowRecorder>& recorder);

}  // namespace lubancode::cli
