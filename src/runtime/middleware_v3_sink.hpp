// MiddlewareEventSink -> trajectory::v3 hooks 事件账的适配器(LuaHook 单
// P0-B,§7.1 统一事实账):中间件核的派发事实经会话的 V3Writer 落账——
// 一个 writer,hook 事件不留旁路。事件合同 = src/trajectory/v3/hooks.cpp
// (NestedHookDispatchSession:洋葱嵌套版)。
//
// 线程合同:观察者可能在短命线程里并发回调(middleware.hpp 的 sink 合同),
// 本类自带互斥;writer 自身单写者语义由调用方(会话)保证——同一时刻只
// 有一只 sink 绑一只 writer。
//
// 落账失败(writer broken / IoFailed):本类记 recent_errors 后继续(事件
// 账是非关键投影:§7.1"任何日志故障都不能触发下游重复执行");调用方从
// recent_errors() 读诊断。
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/middleware.hpp"
#include "trajectory/v3/hooks.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::runtime {

class V3MiddlewareEventSink final : public hooks::middleware::MiddlewareEventSink {
public:
    explicit V3MiddlewareEventSink(trajectory::v3::V3Writer& writer) : writer_(&writer) {}

    // ---- dispatch 层 ----
    void OnDispatchRequested(const hooks::middleware::DispatchMeta& meta,
                             const std::vector<hooks::middleware::HandlerSnapshot>& handlers) override;
    void OnSkipped(const hooks::middleware::DispatchMeta& meta, std::string_view reason) override;

    // ---- invocation 层(嵌套安全:显式 invocation_id)----
    void OnInvocationStarted(const hooks::middleware::InvocationMeta& meta) override;
    void OnInvocationCompleted(const hooks::middleware::InvocationMeta& meta,
                               std::optional<std::string> decision, std::uint64_t duration_ms) override;
    void OnInvocationFailed(const hooks::middleware::InvocationMeta& meta, std::string_view error_code,
                            std::uint64_t duration_ms) override;
    void OnInvocationCancelled(const hooks::middleware::InvocationMeta& meta,
                               std::string_view reason) override;

    // ---- 效果与洋葱语义 ----
    void OnEffectSettled(const hooks::middleware::InvocationMeta& meta, std::string_view effect_type,
                         bool applied, std::string_view reason, const nlohmann::json& value) override;
    void OnContinuationConsumed(const hooks::middleware::InvocationMeta& meta) override;
    void OnOutputProposed(const hooks::middleware::InvocationMeta& meta, std::string_view phase,
                          const nlohmann::json& candidate) override;

    // 诊断:最近的落账失败(稳定码/人话;测试与 /doctor 用)。
    std::vector<std::string> recent_errors() const;

private:
    struct DispatchBook {
        trajectory::v3::NestedHookDispatchSession session;
        // invocation_id -> started 快照(failurePolicy 随行)。
        std::map<std::string, trajectory::v3::HookHandlerSpec> specs;
        explicit DispatchBook(trajectory::v3::NestedHookDispatchSession in) : session(std::move(in)) {}
    };

    // 每枚回调的公共出入:锁 + 按 meta 找/建 dispatch 台账。
    DispatchBook* LockBookFor(const hooks::middleware::InvocationMeta& meta);
    DispatchBook* LockBookFor(const hooks::middleware::DispatchMeta& meta);

    void NoteError(const char* where, const trajectory::v3::WriteReceipt& receipt);

    trajectory::v3::V3Writer* writer_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<DispatchBook>> books_;
    std::vector<std::string> recent_errors_;
};

}  // namespace lubancode::runtime
