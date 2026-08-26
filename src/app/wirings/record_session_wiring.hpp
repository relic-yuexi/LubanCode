// 录制接线器(会话终章):/record 命令组的会话件(一场至多一场录制)自
// TerminalSessionController 大类外迁,归这一只。控制器持句柄调。
//
// 状态归属:录制器(可选)与录制件根目录跟接线器走;装技能的两级根与
// 装后刷新由会话借来(Host 全借用)。解析/问话/起草/安装在
// cli/record_command(presenter),这里只递材料。
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "cli/record_command.hpp"        // RecordCommandContext/HandleRecordCommand
#include "skills/workflow_recorder.hpp"

namespace lubancode::app {

class RecordSessionWiring {
public:
    // 会话借给接线器的材料(全借用,接线器不拥有)。
    struct Host {
        const std::filesystem::path* recordings_root = nullptr;    // <主目录>/recordings
        const std::filesystem::path* project_skills_root = nullptr;  // <cwd>/.lubancode/skills
        const std::filesystem::path* global_skills_root = nullptr;   // <主目录>/skills
        std::function<void()> refresh_skills;  // install 后刷新本场技能清单
    };

    RecordSessionWiring() = default;
    explicit RecordSessionWiring(Host host);
    void AttachHost(Host host) { host_ = std::move(host); }

    // /record 的命令材料包(分派位递给 cli::HandleRecordCommand)。
    lubancode::cli::RecordCommandContext MakeCommandContext();

    // 回合要挂的录制器(没在录给 nullptr)。
    lubancode::skills::WorkflowRecorder* recorder() {
        return recorder_.has_value() ? &*recorder_ : nullptr;
    }
    // 状态栏 REC 短标记的材料(没在录的 optional)。
    const std::optional<lubancode::skills::WorkflowRecorder>& recorder_optional() const { return recorder_; }

private:
    Host host_;
    std::optional<lubancode::skills::WorkflowRecorder> recorder_;
};

}  // namespace lubancode::app
