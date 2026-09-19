#include "platform/console.hpp"

#include <memory>

namespace lubancode::platform {

std::recursive_timed_mutex& ConsoleInputMutex() {
    static std::recursive_timed_mutex mutex;
    return mutex;
}

// ---------------------------------------------------------------------------
// 控制台原语的测试替身(合同见 console.hpp 的 ConsoleTestHooks 一节)。
// 存储:原子指针指向堆上的替身表(安装时整份换,旧表泄漏到进程结束——
// 安装只发生在测试 setup/teardown,几次指针的寿命不作讲究)。读侧无锁。
// ---------------------------------------------------------------------------
namespace {
std::atomic<const ConsoleTestHooks*> g_test_hooks{nullptr};
}

namespace detail {
const ConsoleTestHooks* ConsoleTestHooksIfAny() {
    return g_test_hooks.load(std::memory_order_acquire);
}
}  // namespace detail

void SetConsoleTestHooks(ConsoleTestHooks hooks) {
    const bool any = hooks.get_screen_info || hooks.set_cursor_pos || hooks.clear_row_from ||
                     hooks.clear_row_hard_from || hooks.pan_viewport_down || hooks.console_width ||
                     hooks.read_row_text || hooks.override_screen_repaint;
    const ConsoleTestHooks* next = nullptr;
    if (any) {
        next = new ConsoleTestHooks(std::move(hooks));
    }
    const ConsoleTestHooks* previous = g_test_hooks.exchange(next, std::memory_order_acq_rel);
    // 旧表延迟释放有竞态(热路径可能正读着),直接放弃——测试级频率,
    // 几张表的内存不作讲究,绝不悬垂。
    (void)previous;
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
