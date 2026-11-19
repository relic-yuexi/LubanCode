// Host/service coordination belongs to runtime. The engine's V3 sink must
// not refer back to runtime archives: GNU linking resolves them only once.
#include "runtime/middleware_v3_sink.hpp"

#include <chrono>
#include <memory>

namespace lubancode::runtime {
namespace {
constexpr int kSessionSwitchDrainTimeoutMs = 10'000;
} // namespace

void BindMiddlewareSessionWriter(hooks::HookDispatcher* dispatcher, HookHostServiceCenter* services,
                                 trajectory::v3::V3Writer* writer) {
    if (dispatcher == nullptr) {
        return;
    }
    auto* current = dynamic_cast<V3MiddlewareEventSink*>(dispatcher->middleware_sink());
    const bool switching = writer != nullptr && (current == nullptr || current->writer() != writer);
    // LuaHook P1-D(§六 clear/exit 排空):换写者(= 换会话/换场)先排空——
    // 停接新工作、等在途收口,再换绑。同写者幂等直过;null 解绑是进程收尾,
    // 不在此等(退出路径自理)。到点未归零按"无法核实保留 unknown"放行
    // 换绑,不无限等。两层各等各的:dispatch 账保旧 sink 不被拆引用
    //(在途 dispatch 持它的裸指针);中心账保 Lua invocation 的 Host API
    // 与子执行账收口(它们钉着旧写者)。
    if (switching) {
        (void)dispatcher->WaitForMiddlewareDrain(std::chrono::milliseconds(kSessionSwitchDrainTimeoutMs));
        if (services != nullptr) {
            services->BeginDrain("session_writer_switch");
            (void)services->WaitForDrain(std::chrono::milliseconds(kSessionSwitchDrainTimeoutMs));
            services->EndDrain();
        }
    }
    // 子执行账(hook 工具桥的 v3 记账)与服务束共用同一枚写者指针。
    if (services != nullptr) {
        services->SetSubExecutionWriter(writer);
    }
    if (writer == nullptr) {
        if (current != nullptr) {
            dispatcher->SetMiddlewareSink(nullptr);
        }
        return;
    }
    if (current != nullptr && current->writer() == writer) {
        return;  // 同一写者:幂等,不重建 sink
    }
    dispatcher->SetMiddlewareSink(std::make_shared<V3MiddlewareEventSink>(*writer));
}

} // namespace lubancode::runtime
