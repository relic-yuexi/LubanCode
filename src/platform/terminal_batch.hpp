#pragma once

#include <string>
#include <string_view>

namespace lubancode::platform {

// 攒一帧 VT 输出。调用方传 screen-buffer 坐标，类内先减 viewport 左上角，
// 再发 CUP；定位、擦行、正文与最终光标全进同一段字节，Flush 时才一次
// 写到 stdout，免得每画一行便在 C++ 流与 Console API 之间来回同步。
class TerminalBatch {
public:
    explicit TerminalBatch(int viewport_x = 0, int viewport_y = 0,
                           bool synchronized_output = true);

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
