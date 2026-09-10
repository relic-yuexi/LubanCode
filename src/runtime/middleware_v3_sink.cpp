// V3 事件账适配器实现(LuaHook 单 P0-B)。事件序与执行核同步:
// requested -> started -> [proposed -> settled/consumed]* -> completed/…;
// 嵌套(invocation i 的 next 里跑 i+1)由 NestedHookDispatchSession 容纳。
#include "runtime/middleware_v3_sink.hpp"

#include <utility>

namespace lubancode::runtime {

namespace {

using hooks::middleware::DispatchMeta;
using hooks::middleware::HandlerSnapshot;
using hooks::middleware::InvocationMeta;
using trajectory::v3::Durability;
using trajectory::v3::HookHandlerSpec;
using trajectory::v3::NestedHookDispatchSession;
using trajectory::v3::WriteReceipt;

HookHandlerSpec ToSpec(const HandlerSnapshot& snapshot) {
    HookHandlerSpec spec;
    spec.hook_id = snapshot.hook_id;
    spec.definition_hash = snapshot.definition_hash;
    spec.handler_kind = snapshot.handler_kind;
    spec.definition_order = snapshot.definition_order;
    spec.failure_policy = snapshot.failure_policy;
    return spec;
}

// P0-A 的 failure_policy 枚举名(abort/keep_original)对 v3 合同
// (block/keep_original/fallback)的映射:abort = block(失败即拦)。
std::string NormalizeFailurePolicy(const std::string& policy) {
    if (policy == "abort") {
        return "block";
    }
    return policy;
}

}  // namespace

V3MiddlewareEventSink::DispatchBook* V3MiddlewareEventSink::LockBookFor(const InvocationMeta& meta) {
    // 调用方已持锁(私有口,只在各回调里用)。
    const auto it = books_.find(meta.dispatch_id);
    return it == books_.end() ? nullptr : it->second.get();
}

V3MiddlewareEventSink::DispatchBook* V3MiddlewareEventSink::LockBookFor(const DispatchMeta& meta) {
    const auto it = books_.find(meta.dispatch_id);
    return it == books_.end() ? nullptr : it->second.get();
}

void V3MiddlewareEventSink::NoteError(const char* where, const WriteReceipt& receipt) {
    if (receipt.status == WriteReceipt::Status::Committed) {
        return;
    }
    recent_errors_.push_back(std::string(where) + ": " + receipt.error_code + " " + receipt.error_message);
    // 有界:只留最近 16 条,诊断够用不刷屏。
    if (recent_errors_.size() > 16) {
        recent_errors_.erase(recent_errors_.begin(), recent_errors_.end() - 16);
    }
}

void V3MiddlewareEventSink::OnDispatchRequested(const DispatchMeta& meta,
                                                const std::vector<HandlerSnapshot>& handlers) {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<HookHandlerSpec> specs;
    specs.reserve(handlers.size());
    for (const auto& handler : handlers) {
        specs.push_back(ToSpec(handler));
    }
    NestedHookDispatchSession session =
        NestedHookDispatchSession::Open(*writer_, meta.dispatch_id, meta.hook_point, meta.turn_id,
                                        meta.step_id, meta.action_id, specs,
                                        /*input_ref=*/std::nullopt, Durability::ProcessCrash);
    auto book = std::make_shared<DispatchBook>(std::move(session));
    for (const auto& spec : specs) {
        book->specs[spec.hook_id] = spec;  // 同 hook_id 只留最新快照(failurePolicy 随行)
    }
    books_[meta.dispatch_id] = std::move(book);
    if (books_.size() > 64) {
        // 有界驻留:老 dispatch 的台账回收(事件已落账,内存簿只服务进行中者)。
        books_.erase(books_.begin());
    }
}

void V3MiddlewareEventSink::OnSkipped(const DispatchMeta& meta, std::string_view reason) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const WriteReceipt receipt = NestedHookDispatchSession::WriteSkip(
        *writer_, meta.dispatch_id, meta.hook_point, std::string(reason), meta.turn_id,
        meta.step_id, meta.action_id, Durability::ProcessCrash);
    NoteError("hook.skipped", receipt);
}

void V3MiddlewareEventSink::OnInvocationStarted(const InvocationMeta& meta) {
    const std::lock_guard<std::mutex> lock(mutex_);
    DispatchBook* book = LockBookFor(meta);
    if (book == nullptr) {
        return;
    }
    HookHandlerSpec spec;
    spec.hook_id = meta.hook_id;
    spec.handler_kind = meta.handler_kind;
    spec.definition_hash = meta.definition_hash;
    spec.definition_order = meta.definition_order;
    const auto known = book->specs.find(meta.hook_id);
    spec.failure_policy = known != book->specs.end() ? NormalizeFailurePolicy(known->second.failure_policy)
                                                     : "block";
    const WriteReceipt receipt = book->session.BeginInvocation(*writer_, meta.invocation_id, spec,
                                                               Durability::ProcessCrash);
    NoteError("hook.started", receipt);
}

void V3MiddlewareEventSink::OnInvocationCompleted(const InvocationMeta& meta,
                                                  std::optional<std::string> decision, std::uint64_t duration_ms) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (DispatchBook* book = LockBookFor(meta); book != nullptr) {
        const WriteReceipt receipt = book->session.CompleteInvocation(
            *writer_, meta.invocation_id, meta.hook_id, std::move(decision),
            /*output_ref=*/std::nullopt, duration_ms);
        NoteError("hook.completed", receipt);
    }
}

void V3MiddlewareEventSink::OnInvocationFailed(const InvocationMeta& meta, std::string_view error_code,
                                               std::uint64_t duration_ms) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (DispatchBook* book = LockBookFor(meta); book != nullptr) {
        const WriteReceipt receipt = book->session.FailInvocation(*writer_, meta.invocation_id,
                                                                  std::string(error_code), duration_ms);
        NoteError("hook.failed", receipt);
    }
}

void V3MiddlewareEventSink::OnInvocationCancelled(const InvocationMeta& meta, std::string_view reason) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (DispatchBook* book = LockBookFor(meta); book != nullptr) {
        const WriteReceipt receipt = book->session.CancelInvocation(*writer_, meta.invocation_id,
                                                                    std::string(reason));
        NoteError("hook.cancelled", receipt);
    }
}

void V3MiddlewareEventSink::OnEffectSettled(const InvocationMeta& meta, std::string_view effect_type,
                                            bool applied, std::string_view reason, const nlohmann::json& value) {
    const std::lock_guard<std::mutex> lock(mutex_);
    DispatchBook* book = LockBookFor(meta);
    if (book == nullptr) {
        return;
    }
    const WriteReceipt receipt =
        applied
            ? book->session.ApplyEffect(*writer_, meta.invocation_id, std::string(effect_type),
                                        /*applied_value_ref=*/value, Durability::PowerLoss)
            : book->session.RejectEffect(*writer_, meta.invocation_id, std::string(effect_type),
                                         std::string(reason), Durability::PowerLoss);
    NoteError(applied ? "hook.effects.applied" : "hook.effects.rejected", receipt);
}

void V3MiddlewareEventSink::OnContinuationConsumed(const InvocationMeta& meta) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (DispatchBook* book = LockBookFor(meta); book != nullptr) {
        const WriteReceipt receipt =
            book->session.ContinuationConsumed(*writer_, meta.invocation_id, Durability::ProcessCrash);
        NoteError("hook.continuation.consumed", receipt);
    }
}

void V3MiddlewareEventSink::OnOutputProposed(const InvocationMeta& meta, std::string_view phase,
                                             const nlohmann::json& candidate) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (DispatchBook* book = LockBookFor(meta); book != nullptr) {
        const WriteReceipt receipt = book->session.OutputProposed(
            *writer_, meta.invocation_id, std::string(phase), candidate, Durability::ProcessCrash);
        NoteError("hook.output.proposed", receipt);
    }
}

std::vector<std::string> V3MiddlewareEventSink::recent_errors() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return recent_errors_;
}

void BindMiddlewareSessionWriter(hooks::HookDispatcher* dispatcher, HookHostServiceCenter* services,
                                 trajectory::v3::V3Writer* writer) {
    if (dispatcher == nullptr) {
        return;
    }
    // 子执行账(hook 工具桥的 v3 记账)与服务束共用同一枚写者指针。
    if (services != nullptr) {
        services->SetSubExecutionWriter(writer);
    }
    auto* current = dynamic_cast<V3MiddlewareEventSink*>(dispatcher->middleware_sink());
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

}  // namespace lubancode::runtime
