// 向导面板(向导重排单):一块上下分隔线包住的稳定交互区。上下各一条线,
// 标题+进度、正文说明、错误、输入行、按键提示各守其位;换步或重试时先清
// 掉旧区域、再从同一处整帧画回——八步走完屏上始终只有一块当前面板,试填
// 不往下堆,也不写进永久 transcript。
//
// 绘制全走既有原语(platform::SetCursorPos/ClearRowHardFrom/GetScreenInfo、
// cli::ReadLine、cli::ReadChoiceMenu),不在 provider 逻辑里手写光标转义。
// 终端宽度每次 Draw 现查:缩放或说明软换行后按新宽度整帧重画,不残留半截
// 边线。真终端不可用(管道/重定向/POSIX 无重画支持)时 Available() 为假,
// 调用方退化朴素逐行。/provider switch 选择器与补钥页同用这一块面板。

#pragma once

#include <optional>
#include <string>

#include "cli/console_input.hpp"
#include "cli/setup_wizard.hpp"

namespace lubancode::cli {

class WizardPanel {
public:
    // 真终端且支持原地重画(stdin 逐键 + stdout 控制台 + SupportsScreenRepaint)
    // 才开面板;否则调用方走朴素逐行。
    static bool Available();

    WizardPanel();
    ~WizardPanel();  // 收面板:清掉整块区域,光标归回起点

    WizardPanel(const WizardPanel&) = delete;
    WizardPanel& operator=(const WizardPanel&) = delete;

    // 清旧画新。reserve_rows:面板内还要腾给后续内容的行数(选择菜单的
    // 选项数等),一次连着预留,免得后面画菜单时把缓冲区顶滚、面板起点
    // 失锚。文本帧画完光标停在 prompt 行末;选择帧画完光标停在预留区首行,
    // 交给 ReadChoiceMenu 画菜单。
    void Draw(const WizardFrame& frame, int reserve_rows = 0);

    // 在面板的 prompt 行上读一行(Esc=返回、Ctrl+C=取消,reason 分清)。
    // prompt 文字已由 Draw 印在面板里,这里传空串给 ReadLine,不让它再打。
    std::optional<std::string> ReadText(ReadExitReason* reason);

    // 显式收面板(向导正常走完/取消时调用;析构里也会兜一道)。
    void Finish();

private:
    bool active_ = false;
    int start_row_ = 0;   // 面板首行(上分隔线)的绝对行号
    int rows_drawn_ = 0;  // 面板当前占的总行数(含预留区)
    int prompt_row_ = -1; // 文本帧的输入行号(相对面板首行);选择帧为 -1
    int menu_top_ = -1;   // 选择帧预留区首行(相对面板首行);文本帧为 -1
    int width_ = 80;      // 最近一帧的终端宽度
    std::string prompt_;  // ReadLine 重画输入行时须带回这段静态前缀
};

}  // namespace lubancode::cli
