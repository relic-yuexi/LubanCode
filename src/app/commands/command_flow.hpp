// slash 命令处理器的返回语义:继续等下一行,还是这一行触发了 /exit、
// 外层循环该退出。取代原先裸 bool 的"继续/退出"约定。
#pragma once

namespace lubancode::app {

enum class CommandFlow {
    Continue,
    Exit,
};

}  // namespace lubancode::app
