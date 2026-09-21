// editor_adapter.hpp 的实现。SV-09 拆分前住 project_memory.cpp,搬来逻辑
// 一字未动;30 分钟上限与 RunProcess 的调用收进 adapter,门面只拿结论。

#include "memory/editor_adapter.hpp"

#include "memory/internal.hpp"  // PathUtf8:文件路径过进程启动层的窄边界
#include "platform/paths.hpp"   // GetEnvVar:$VISUAL/$EDITOR
#include "platform/process.hpp"

namespace lubancode::memory::editor {

std::string PickEditorProgram() {
    if (const auto visual = platform::GetEnvVar("VISUAL"); visual.has_value()) {
        return *visual;
    }
    if (const auto editor = platform::GetEnvVar("EDITOR"); editor.has_value()) {
        return *editor;
    }
#ifdef _WIN32
    return "notepad";
#else
    return "vi";
#endif
}

EditorRunResult RunEditorForFile(const std::string& program, const std::filesystem::path& file) {
    const int kEditorTimeoutMs = 30 * 60 * 1000;
    const auto ran = platform::RunProcess({program, PathUtf8(file)}, kEditorTimeoutMs);
    EditorRunResult result;
    result.spawn_failed = ran.spawn_failed;
    result.timed_out = ran.timed_out;
    result.spawn_error = ran.spawn_error;
    return result;
}

}  // namespace lubancode::memory::editor
