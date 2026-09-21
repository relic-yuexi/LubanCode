// SV-09(2026-09-21 架构审查)拆出的外部管理适配(内部件):$VISUAL/
// $EDITOR 的选择与编辑器进程的交互启动。门面不再直接起编辑器——拉起、
// 超时与 spawn 失败的原始事实都从这走,错误文案的拼装留在调用方。

#pragma once

#include <filesystem>
#include <string>

namespace lubancode::memory::editor {

// 编辑器选择:$VISUAL 压过 $EDITOR;都没给就退平台缺省(Windows 记事本、
// 类 Unix vi)。命令行里带空格的路径由进程启动层负责引号。
std::string PickEditorProgram();

// 一次编辑器交互的原始结局(spawn 失败/超时/正常退出)。
struct EditorRunResult {
    bool spawn_failed = false;
    bool timed_out = false;
    std::string spawn_error;  // spawn_failed 时的原因
};

// 拉起编辑器等用户改完退出(30 分钟上限)。超时按 timed_out 报,不杀。
EditorRunResult RunEditorForFile(const std::string& program, const std::filesystem::path& file);

}  // namespace lubancode::memory::editor
