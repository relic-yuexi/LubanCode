// settings.local.json:项目级本地权限(不进版本库)。
// <cwd>/.lubancode/settings.local.json,照 Claude Code / Codex 的路数。
//
// 骨架拆解反弹·问题 7 自 config.cpp/config.hpp 拆出独立小档:主
// Config/env 解析/provider merge 与这份项目级权限文件是两摊事,拆开各
// 管各的。函数体原样搬来,行为一字未改。单测在
// tests/unit/config/test_config.cpp。

#pragma once

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::config {

// settings.local.json 里的 permissions 段,项目级本地权限(不进版本库)。
// 位置 <cwd>/.lubancode/settings.local.json,照 Claude Code / Codex 的路数:
//   - allow_tools:这些工具启动即注入会话"总是允许"集合,直接免确认;
//   - allow_commands:run_command 命令前缀白名单,auto 档里等价 command_safety
//     判成 Safe(补充白名单,不改 command_safety.cpp,在 main 的 auto 分流处
//     叠加判定);
//   - deny_commands:run_command 命令前缀黑名单,confirm/auto 档里前缀命中就
//     永远问一句(压过 allow_commands、压过会话"总是允许";yolo/--yes 是显式
//     全放,deny 不拦);
//   - default_confirm_mode:起手确认档(auto/yolo/confirm),优先级低于
//     --yes/LUBANCODE_CONFIRM_MODE,高于内置默认 confirm。
// 全部字段可选;坏 JSON 只告警跳过,不崩。
struct SettingsLocal {
    std::vector<std::string> allow_tools;
    std::vector<std::string> allow_commands;
    std::vector<std::string> deny_commands;
    std::optional<std::string> default_confirm_mode;  // auto / yolo / confirm
    // Plan 模式单:起手协作档(plan / default)。优先级低于 --mode 与
    // LUBANCODE_COLLABORATION_MODE(RunCli 的 ResolveStartupPlanMode)。
    std::optional<std::string> default_collaboration_mode;

    bool Empty() const {
        return allow_tools.empty() && allow_commands.empty() && deny_commands.empty() &&
               !default_confirm_mode.has_value() && !default_collaboration_mode.has_value();
    }
};

// <cwd>/.lubancode/settings.local.json 的路径(cwd_dir 传 CurrentDirUtf8())。
std::string SettingsLocalPath(const std::string& cwd_dir);

// 纯函数,不碰 IO:解析 settings.local.json 文本。顶层要有 "permissions"
// object(没有就返回空 SettingsLocal,不算错);其中 allow_tools /
// allow_commands / deny_commands 是字符串数组(非字符串元素跳过),
// default_confirm_mode 是字符串(auto/yolo/confirm,别的值原样留着交给
// 调用方判)。全部字段可选。坏 JSON、顶层不是 object 才返回错误(调用方
// 打一行警告后当没配置,不崩)。
std::expected<SettingsLocal, std::string> ParseSettingsLocal(const std::string& json_text,
                                                              const std::string& path_for_error);

// 读 <cwd>/.lubancode/settings.local.json。文件不存在返回 std::nullopt
// (不算错);读到了就解析。坏 JSON 把可读错误往上抛(调用方告警跳过)。
std::expected<std::optional<SettingsLocal>, std::string> LoadSettingsLocal(const std::string& cwd_dir);

// 把一个工具名永久写进 <cwd>/.lubancode/settings.local.json 的
// permissions.allow_tools(去重)。目录/文件不存在则按需创建(项目级
// .lubancode/ 只在这"首次持久化 permissions"时才落地),已有的别的字段
// (含不认得的)原样保留。成功返回写入的完整路径;建目录/写文件失败返回
// 可读错误。gitignore 由调用方另行处理(EnsureGitignoreCoversSettingsLocal),
// 好把"追加了一行 / 提示手动加"的反馈打给用户看。
std::expected<std::string, std::string> AddAllowedToolToSettingsLocal(const std::string& cwd_dir,
                                                                       const std::string& tool_name);

// 保证 <cwd>/.gitignore 挡住 .lubancode/settings.local.json:.gitignore 存在
// 且没挡就追加一行(返回 "appended");已经挡住返回 "already";.gitignore
// 压根不存在就不硬塞,返回一行给用户看的提示("hint:...",教他手动加)。
// 纯做文件这一件事,不抛错(读写失败也只是不动 .gitignore)。
std::string EnsureGitignoreCoversSettingsLocal(const std::string& cwd_dir);

}  // namespace lubancode::config
