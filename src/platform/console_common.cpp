#include "platform/console.hpp"

namespace lubancode::platform {

std::recursive_timed_mutex& ConsoleInputMutex() {
    static std::recursive_timed_mutex mutex;
    return mutex;
}

// 选路规矩的完整注释在 console.hpp 的 InlineRepaintPlan 一节(8.2 二轮
// 重裁后的表)。这里只落那张表的机械执行:Windows 真 console 一律原生
// WriteConsoleOutput 直写行,vt_batch 恒 false——8.1 高频轨迹实锤批内
// CUP 会把 buffer 光标搬去活动行,2026 只缓冲文本渲染救不了光标;POSIX
// 没有原生路,保留 VT 批(确认 2026 才包同步输出,低频档接受已知中间态)。
InlineRepaintPlan PlanInlineRepaint(const StdoutConsoleProbe& probe) {
    InlineRepaintPlan plan;
#ifdef _WIN32
    plan.vt_batch = false;
    plan.sync_output = false;
    // 原生行直写要真 console 的屏幕缓冲区;管道/重定向 GetConsoleScreen
    // BufferInfo 失败,WriteNativeRow 恒 false,那档没有扫光(扫光复活单)。
    plan.native_rows = probe.is_console;
#else
    plan.vt_batch = probe.vt_enabled;
    plan.sync_output = probe.vt_enabled && probe.sync_output;
    plan.native_rows = false;  // POSIX 无缓冲区直写 API,扫光档恒关
#endif
    return plan;
}

}  // namespace lubancode::platform
