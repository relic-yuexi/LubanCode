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
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/dispatcher.hpp"
#include "hooks/middleware.hpp"
#include "runtime/hook_host_services.hpp"
#include "trajectory/v3/hooks.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::runtime {

// Native event owner; deliberately no SDK operation identity or authority.
struct MiddlewareReceiptScope {
    std::string session_id, run_id, turn_id, step_id, action_id;
    std::uint64_t attempt = 1, registry_revision = 0;
    nlohmann::json terminal_ref, persisted_ref;
    std::size_t receipt_limit = 256;
};

enum class MiddlewareReceiptStage {
    Requested, Skipped, Started, Completed, Failed, Cancelled,
    OutputProposed, ContinuationConsumed, EffectsApplied, EffectsRejected
};
enum class MiddlewareReceiptGap {
    None, OwnerMismatch, InvalidSequence, Overflow, NativeUnconfirmed,
    NativeException, Abandoned
};
struct MiddlewareNativeReceipt {
    MiddlewareReceiptStage stage = MiddlewareReceiptStage::Requested;
    std::string dispatch_id, invocation_id, hook_id;
    // Empty when no native append returned. Never manufacture a receipt.
    std::optional<trajectory::v3::WriteReceipt> receipt;
    bool writer_broken = false;
};
struct MiddlewareReceiptSnapshot {
    MiddlewareReceiptScope scope;
    std::optional<hooks::middleware::DispatchMeta> dispatch;
    std::vector<MiddlewareNativeReceipt> receipts;
    std::optional<hooks::middleware::DispatchOutcome::Kind> outcome;
    MiddlewareReceiptGap gap = MiddlewareReceiptGap::None;
    bool finished = false, closed = false;
    // Owned live producer facts only; these tags are not stored V3 events.
    std::optional<hooks::middleware::DispatchCause> cause;
    std::optional<hooks::middleware::DispatchFailureSource> failure_source;
    // This is capture completeness, not successful Post settlement.
    bool complete() const { return finished && gap == MiddlewareReceiptGap::None; }
};
struct MiddlewareReceiptState;
struct CapturedMiddlewareSink;

class MiddlewareReceiptLease {
public:
    MiddlewareReceiptLease() = default;
    MiddlewareReceiptLease(MiddlewareReceiptLease&&) noexcept;
    MiddlewareReceiptLease& operator=(MiddlewareReceiptLease&&) noexcept;
    MiddlewareReceiptLease(const MiddlewareReceiptLease&) = delete;
    MiddlewareReceiptLease& operator=(const MiddlewareReceiptLease&) = delete;
    ~MiddlewareReceiptLease();

    MiddlewareReceiptSnapshot Snapshot() const;
    // Call only after Dispatch and all observer joins have actually returned.
    std::expected<void, std::string> Finish(const hooks::middleware::DispatchOutcome& outcome);
    void Close() noexcept;
private:
    friend std::expected<CapturedMiddlewareSink, std::string> CaptureMiddlewareReceipts(
        trajectory::v3::V3Writer&, MiddlewareReceiptScope);
    explicit MiddlewareReceiptLease(std::shared_ptr<MiddlewareReceiptState> state) : state_(std::move(state)) {}
    std::shared_ptr<MiddlewareReceiptState> state_;
};

class V3MiddlewareEventSink final : public hooks::middleware::MiddlewareEventSink {
public:
    explicit V3MiddlewareEventSink(trajectory::v3::V3Writer& writer) : writer_(&writer) {}
    ~V3MiddlewareEventSink() override;

    // 绑定的写者(BindMiddlewareSessionWriter 幂等判断用)。
    trajectory::v3::V3Writer* writer() const { return writer_; }

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
    struct CaptureAttempt;
    friend std::expected<CapturedMiddlewareSink, std::string> CaptureMiddlewareReceipts(
        trajectory::v3::V3Writer&, MiddlewareReceiptScope);
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
    MiddlewareNativeReceipt* PrepareCaptured(const hooks::middleware::DispatchMeta&, MiddlewareReceiptStage);
    MiddlewareNativeReceipt* PrepareCaptured(const hooks::middleware::InvocationMeta&, MiddlewareReceiptStage);
    void StoreCaptured(const char*, MiddlewareNativeReceipt*, trajectory::v3::WriteReceipt);
    void CaptureException() noexcept;

    trajectory::v3::V3Writer* writer_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<DispatchBook>> books_;
    std::vector<std::string> recent_errors_;
    std::shared_ptr<MiddlewareReceiptState> capture_;
};

struct CapturedMiddlewareSink {
    std::unique_ptr<V3MiddlewareEventSink> sink;
    MiddlewareReceiptLease lease;
};

// Caller holds the actual writer's host serial gate throughout construction and
// Dispatch. The returned lease/snapshot never owns or borrows that writer.
std::expected<CapturedMiddlewareSink, std::string> CaptureMiddlewareReceipts(
    trajectory::v3::V3Writer& writer, MiddlewareReceiptScope scope);

// ---------------------------------------------------------------------------
// 生产通电(LuaHook P0-B 遗留①,P1-C 补):dispatcher 的中间件事件 sink 与
// 服务中心的子执行账都绑到当前会话的 v3 主写者(参照 trajectory_session
// 的 v3_main_writer() 取用口)。每轮入口幂等调用——clear/resume 换场后
// 写者指针会变,换只新 sink,旧的不许留着指废账。writer=null 解绑(v2 场
// /未开卷);services 可空(只绑 sink,不动子执行账)。
// ---------------------------------------------------------------------------
void BindMiddlewareSessionWriter(hooks::HookDispatcher* dispatcher, HookHostServiceCenter* services,
                                 trajectory::v3::V3Writer* writer);

}  // namespace lubancode::runtime
