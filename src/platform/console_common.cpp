#include "platform/console.hpp"

namespace lubancode::platform {

std::recursive_timed_mutex& ConsoleInputMutex() {
    static std::recursive_timed_mutex mutex;
    return mutex;
}

// 选路规矩的完整注释在 console.hpp 的 InlineRepaintPlan 一节。这里只落
// 那张表的机械执行:确认 2026 才许 VT 批包同步输出;没确认时,Windows
// 退原生控制台 API(不搬实体光标的屏幕写入),POSIX 退 VT 批不包 2026。
InlineRepaintPlan PlanInlineRepaint(const StdoutConsoleProbe& probe) {
    InlineRepaintPlan plan;
    plan.sync_output = probe.vt_enabled && probe.sync_output;
#ifdef _WIN32
    plan.vt_batch = plan.sync_output;
#else
    plan.vt_batch = probe.vt_enabled;
#endif
    return plan;
}

}  // namespace lubancode::platform
