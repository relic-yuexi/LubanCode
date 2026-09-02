#pragma once

#include <string>
#include <string_view>

namespace lubancode::platform {

// 攒一帧 VT 输出。调用方传 screen-buffer 坐标，类内先减 viewport 左上角，
// 再发 CUP；定位、擦行、正文与最终光标全进同一段字节，Flush 时才一次
// 写到 stdout，免得每画一行便在 C++ 流与 Console API 之间来回同步。
//
// synchronized_output 是显式能力入参(终端思考活动条单·P0 治根):批外包
// `CSI ?2026 h/l` 把这一帧钉成原子提交。调用方必须传探测结论
// (platform::ProbeSyncOutputSupport() / PlanInlineRepaint().sync_output),
// 不设默认值——没确认 DEC 2026 的宿主也默认包上,就是拿"终端多半会静默
// 吞掉"赌运气,静默假装支持;不支持的宿主那一帧里的中间 CUP 没有原子
// 提交保证,正是实体光标露中间态的帮凶。
class TerminalBatch {
public:
    TerminalBatch(int viewport_x, int viewport_y, bool synchronized_output);

    void MoveTo(int x, int y);
    void EraseCharacters(int count);
    void ClearRowFrom(int x, int y, int count);
    void ClearRowHardFrom(int x, int y, int count);
    void Write(std::string_view text);
    void HideCursor();
    void ShowCursor();

    bool has_commands() const { return has_commands_; }
    const std::string& Finish();
    void Flush();

private:
    void EnsureOpen() const;

    std::string bytes_;
    bool synchronized_output_ = true;
    int viewport_x_ = 0;
    int viewport_y_ = 0;
    bool has_commands_ = false;
    bool finished_ = false;
};

}  // namespace lubancode::platform
