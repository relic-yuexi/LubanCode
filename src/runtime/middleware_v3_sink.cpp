// V3 事件账适配器实现(LuaHook 单 P0-B)。事件序与执行核同步:
// requested -> started -> [proposed -> settled/consumed]* -> completed/…;
// 嵌套(invocation i 的 next 里跑 i+1)由 NestedHookDispatchSession 容纳。
#include "runtime/middleware_v3_sink.hpp"

#include <utility>
#include <exception>
#include <algorithm>

#include "trajectory/v3/reader.hpp"

namespace lubancode::runtime {

struct MiddlewareReceiptState {
    std::mutex mutex;
    MiddlewareReceiptSnapshot snapshot;
};

MiddlewareReceiptLease::MiddlewareReceiptLease(MiddlewareReceiptLease&&) noexcept = default;
MiddlewareReceiptLease& MiddlewareReceiptLease::operator=(MiddlewareReceiptLease&& other) noexcept {
    if (this != &other) { Close(); state_ = std::move(other.state_); }
    return *this;
}
MiddlewareReceiptLease::~MiddlewareReceiptLease() { Close(); }
void MiddlewareReceiptLease::Close() noexcept {
    if (!state_) return;
    const std::lock_guard lock(state_->mutex);
    state_->snapshot.closed = true;
    if (!state_->snapshot.finished && state_->snapshot.gap == MiddlewareReceiptGap::None)
        state_->snapshot.gap = MiddlewareReceiptGap::Abandoned;
}
MiddlewareReceiptSnapshot MiddlewareReceiptLease::Snapshot() const {
    if (!state_) { MiddlewareReceiptSnapshot out; out.closed = true; return out; }
    const std::lock_guard lock(state_->mutex);
    return state_->snapshot;
}
std::expected<void, std::string> MiddlewareReceiptLease::Finish(
    const hooks::middleware::DispatchOutcome& outcome) {
    if (!state_) return std::unexpected("middleware.capture.no_lease");
    const std::lock_guard lock(state_->mutex);
    auto& snap = state_->snapshot;
    if (snap.closed || snap.finished) return std::unexpected("middleware.capture.closed");
    if (!snap.dispatch || outcome.dispatch_id != snap.dispatch->dispatch_id ||
        outcome.registry_revision != snap.dispatch->registry_revision)
        return std::unexpected("middleware.capture.foreign_outcome");
    if (snap.gap == MiddlewareReceiptGap::None) {
        using S = MiddlewareReceiptStage;
        const auto count = [&](const std::string& hook, S stage) {
            return std::count_if(snap.receipts.begin(), snap.receipts.end(), [&](const auto& item) {
                return item.hook_id == hook && item.stage == stage;
            });
        };
        bool complete = !snap.receipts.empty();
        for (const auto& item : snap.receipts) {
            if (!item.receipt || item.receipt->status != trajectory::v3::WriteReceipt::Status::Committed || item.writer_broken)
                complete = false;
            if (item.stage == S::Started && count(item.hook_id, S::Completed) + count(item.hook_id, S::Failed) +
                count(item.hook_id, S::Cancelled) != 1) complete = false;
        }
        for (const auto& record : outcome.records) {
            const bool skipped = record.outcome.starts_with("skipped_");
            const bool failed = record.outcome == "failed";
            const bool completed = record.outcome == "completed" || record.outcome == "completed_short_circuit" ||
                record.outcome == "denied";
            if ((!skipped && !failed && !completed) || count(record.key, S::Started) != (skipped ? 0 : 1) ||
                count(record.key, S::Failed) != (failed ? 1 : 0) || count(record.key, S::Completed) != (completed ? 1 : 0) ||
                count(record.key, S::ContinuationConsumed) != (record.next_consumed ? 1 : 0)) complete = false;
        }
        if (!complete) snap.gap = MiddlewareReceiptGap::InvalidSequence;
    }
    snap.outcome = outcome.kind;
    snap.cause = outcome.cause;
    snap.failure_source = outcome.failure_source;
    snap.finished = true;
    return {};
}

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

std::expected<CapturedMiddlewareSink, std::string> CaptureMiddlewareReceipts(
    trajectory::v3::V3Writer& writer, MiddlewareReceiptScope scope) {
    using namespace trajectory::v3;
    if (scope.receipt_limit == 0 || scope.receipt_limit > 4096 || scope.registry_revision == 0 ||
        scope.attempt == 0 || scope.session_id.empty() || scope.run_id.empty() ||
        scope.turn_id.empty() || scope.step_id.empty() || scope.action_id.empty())
        return std::unexpected("middleware.capture.invalid_scope");
    if (writer.closed() || writer.broken() || writer.session_id() != scope.session_id || writer.run_id() != scope.run_id)
        return std::unexpected("middleware.capture.writer_unavailable");
    const auto ledger = ReadV3Ledger(writer.path());
    if (!ledger || ledger->lines + 1 != writer.next_seq())
        return std::unexpected("middleware.capture.invalid_prefix");
    const auto last = ledger->LastEntry();
    const auto last_hash = !last ? std::string() : last->is_message
        ? ledger->messages.at(last->index).line_hash : ledger->events.at(last->index).line_hash;
    if (last_hash != writer.last_line_hash()) return std::unexpected("middleware.capture.invalid_prefix");
    const auto terminal_ref = ParseCrossSessionRef(scope.terminal_ref);
    const auto persisted_ref = ParseCrossSessionRef(scope.persisted_ref);
    const auto matches = [&](const CrossSessionRef& ref, const EventLine* event) {
        return event && ref.session_id == event->session_id && ref.run_id == event->run_id &&
            ref.id == event->event_id && ref.seq == event->seq && ref.hash == event->line_hash;
    };
    if (!terminal_ref || !persisted_ref || terminal_ref->session_id != scope.session_id ||
        persisted_ref->session_id != scope.session_id || terminal_ref->run_id != scope.run_id ||
        persisted_ref->run_id != scope.run_id)
        return std::unexpected("middleware.capture.invalid_source");
    const auto* terminal = ledger->FindEvent(terminal_ref->id);
    const auto* raw = ledger->FindEvent(persisted_ref->id);
    const auto owned = [&](const EventLine& event) {
        return event.session_id == scope.session_id && event.run_id == scope.run_id &&
            event.turn_id == scope.turn_id && event.step_id == scope.step_id && event.action_id == scope.action_id &&
            event.payload.contains("attempt") && event.payload.at("attempt") == nlohmann::json(scope.attempt);
    };
    using K = EventKindV3;
    if (!matches(*terminal_ref, terminal) || !matches(*persisted_ref, raw) || !owned(*terminal) || !owned(*raw) ||
        (terminal->kind != K::ToolExecutionFinished && terminal->kind != K::ToolExecutionFailed &&
         terminal->kind != K::ToolExecutionCancelled) || raw->kind != K::ToolResultPersisted ||
        !(terminal->seq < raw->seq)) return std::unexpected("middleware.capture.invalid_source");
    const auto execution = raw->payload.find("executionEventRef");
    const auto execution_ref = execution == raw->payload.end() ? std::optional<CrossSessionRef>() : ParseCrossSessionRef(*execution);
    if (execution == raw->payload.end() || (execution->is_string() ? execution->get<std::string>() != terminal->event_id
        : !execution_ref || !matches(*execution_ref, terminal)))
        return std::unexpected("middleware.capture.invalid_source");
    std::size_t started = 0;
    for (const auto& event : ledger->events)
        if (event.kind == K::ToolExecutionStarted && owned(event) && event.seq < terminal->seq) ++started;
    if (started != 1) return std::unexpected("middleware.capture.invalid_started");
    // Retain only the actual, verified five keys, not extra caller JSON.
    const auto actual_terminal = MakeOwnedJobReference(*ledger, terminal->event_id);
    const auto actual_raw = MakeOwnedJobReference(*ledger, raw->event_id);
    if (!actual_terminal || !actual_raw) return std::unexpected("middleware.capture.invalid_source");
    scope.terminal_ref = *actual_terminal;
    scope.persisted_ref = *actual_raw;
    auto state = std::make_shared<MiddlewareReceiptState>();
    state->snapshot.scope = std::move(scope);
    state->snapshot.receipts.reserve(state->snapshot.scope.receipt_limit);
    auto sink = std::make_unique<V3MiddlewareEventSink>(writer);
    sink->capture_ = state;
    return CapturedMiddlewareSink{std::move(sink), MiddlewareReceiptLease(std::move(state))};
}

V3MiddlewareEventSink::~V3MiddlewareEventSink() {
    if (!capture_) return;
    const std::lock_guard lock(capture_->mutex);
    auto& snap = capture_->snapshot;
    snap.closed = true;
    if (!snap.finished && snap.gap == MiddlewareReceiptGap::None) snap.gap = MiddlewareReceiptGap::Abandoned;
}

struct V3MiddlewareEventSink::CaptureAttempt {
    V3MiddlewareEventSink& sink;
    int exceptions = std::uncaught_exceptions();
    ~CaptureAttempt() {
        if (std::uncaught_exceptions() > exceptions) sink.CaptureException();
    }
};

void V3MiddlewareEventSink::CaptureException() noexcept {
    if (!capture_) return;
    const std::lock_guard lock(capture_->mutex);
    if (capture_->snapshot.gap == MiddlewareReceiptGap::None)
        capture_->snapshot.gap = MiddlewareReceiptGap::NativeException;
}

MiddlewareNativeReceipt* V3MiddlewareEventSink::PrepareCaptured(const DispatchMeta& meta, MiddlewareReceiptStage stage) {
    if (!capture_) return nullptr;
    const std::lock_guard lock(capture_->mutex);
    auto& snap = capture_->snapshot;
    if (snap.closed || snap.finished || snap.gap != MiddlewareReceiptGap::None) return nullptr;
    if (writer_->closed() || writer_->broken() || writer_->session_id() != snap.scope.session_id ||
        writer_->run_id() != snap.scope.run_id) {
        snap.gap = MiddlewareReceiptGap::OwnerMismatch; return nullptr;
    }
    if (meta.hook_point != "PostAction" || meta.dispatch_id.empty() || meta.registry_revision != snap.scope.registry_revision ||
        meta.turn_id != snap.scope.turn_id || meta.step_id != snap.scope.step_id || meta.action_id != snap.scope.action_id) {
        snap.gap = MiddlewareReceiptGap::OwnerMismatch; return nullptr;
    }
    if (snap.dispatch) { snap.gap = MiddlewareReceiptGap::InvalidSequence; return nullptr; }
    if (snap.receipts.size() >= snap.scope.receipt_limit) { snap.gap = MiddlewareReceiptGap::Overflow; return nullptr; }
    snap.dispatch = meta;
    auto& slot = snap.receipts.emplace_back();
    slot.stage = stage; slot.dispatch_id = meta.dispatch_id;
    return &slot;
}

MiddlewareNativeReceipt* V3MiddlewareEventSink::PrepareCaptured(const InvocationMeta& meta, MiddlewareReceiptStage stage) {
    if (!capture_) return nullptr;
    const std::lock_guard lock(capture_->mutex);
    auto& snap = capture_->snapshot;
    if (snap.closed || snap.finished || snap.gap != MiddlewareReceiptGap::None) return nullptr;
    if (writer_->closed() || writer_->broken() || writer_->session_id() != snap.scope.session_id ||
        writer_->run_id() != snap.scope.run_id) {
        snap.gap = MiddlewareReceiptGap::OwnerMismatch; return nullptr;
    }
    const auto* book = LockBookFor(meta);
    const auto known = book ? book->specs.find(meta.hook_id) : std::map<std::string, HookHandlerSpec>::const_iterator{};
    if (!snap.dispatch || snap.dispatch->dispatch_id != meta.dispatch_id || !book || known == book->specs.end() ||
        meta.invocation_id != hooks::middleware::NextMiddlewareInvocationId(meta.dispatch_id, meta.definition_order) ||
        meta.definition_hash != known->second.definition_hash ||
        meta.handler_kind != known->second.handler_kind || meta.definition_order != known->second.definition_order) {
        snap.gap = MiddlewareReceiptGap::OwnerMismatch; return nullptr;
    }
    const auto same_invocation = [&](const auto& entry) {
        return entry.invocation_id == meta.invocation_id && entry.hook_id == meta.hook_id;
    };
    const bool has_started = std::any_of(snap.receipts.begin(), snap.receipts.end(), [&](const auto& entry) {
        return entry.stage == MiddlewareReceiptStage::Started && same_invocation(entry);
    });
    if ((stage == MiddlewareReceiptStage::Started) == has_started) {
        snap.gap = MiddlewareReceiptGap::InvalidSequence; return nullptr;
    }
    const auto terminal = [](MiddlewareReceiptStage value) {
        return value == MiddlewareReceiptStage::Completed || value == MiddlewareReceiptStage::Failed ||
            value == MiddlewareReceiptStage::Cancelled;
    };
    const bool duplicate = std::any_of(snap.receipts.begin(), snap.receipts.end(), [&](const auto& entry) {
        return same_invocation(entry) && ((terminal(stage) && terminal(entry.stage)) ||
            (stage == MiddlewareReceiptStage::ContinuationConsumed && entry.stage == stage));
    });
    if (duplicate) { snap.gap = MiddlewareReceiptGap::InvalidSequence; return nullptr; }
    if (snap.receipts.size() >= snap.scope.receipt_limit) { snap.gap = MiddlewareReceiptGap::Overflow; return nullptr; }
    auto& slot = snap.receipts.emplace_back();
    slot.stage = stage; slot.dispatch_id = meta.dispatch_id; slot.invocation_id = meta.invocation_id; slot.hook_id = meta.hook_id;
    return &slot;
}

void V3MiddlewareEventSink::StoreCaptured(const char* where, MiddlewareNativeReceipt* slot, WriteReceipt receipt) {
    if (slot) {
        const std::lock_guard lock(capture_->mutex);
        slot->receipt.emplace(std::move(receipt));
        slot->writer_broken = writer_->broken();
        if (slot->receipt->status != WriteReceipt::Status::Committed || slot->writer_broken)
            capture_->snapshot.gap = MiddlewareReceiptGap::NativeUnconfirmed;
    }
    NoteError(where, slot ? *slot->receipt : receipt);
}

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
    CaptureAttempt attempt{*this};
    auto* slot = PrepareCaptured(meta, MiddlewareReceiptStage::Requested);
    if (capture_ && !slot) return;
    std::vector<HookHandlerSpec> specs;
    specs.reserve(handlers.size());
    for (const auto& handler : handlers) {
        specs.push_back(ToSpec(handler));
    }
    WriteReceipt requested;
    auto session = capture_
        ? NestedHookDispatchSession::OpenChecked(*writer_, meta.dispatch_id, meta.hook_point, meta.turn_id,
            meta.step_id, meta.action_id, specs,
            nlohmann::json{{"executionEventRef", capture_->snapshot.scope.terminal_ref},
                           {"resultEventRef", capture_->snapshot.scope.persisted_ref}}, requested, Durability::PowerLoss)
        : NestedHookDispatchSession::Open(*writer_, meta.dispatch_id, meta.hook_point, meta.turn_id,
            meta.step_id, meta.action_id, specs, /*input_ref=*/std::nullopt, Durability::ProcessCrash);
    if (capture_) StoreCaptured("hook.requested", slot, std::move(requested));
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
    CaptureAttempt attempt{*this};
    auto* slot = PrepareCaptured(meta, MiddlewareReceiptStage::Skipped);
    if (capture_ && !slot) return;
    auto receipt = capture_
        ? NestedHookDispatchSession::WriteSkipWithInput(*writer_, meta.dispatch_id, meta.hook_point, std::string(reason),
            meta.turn_id, meta.step_id, meta.action_id,
            nlohmann::json{{"executionEventRef", capture_->snapshot.scope.terminal_ref},
                           {"resultEventRef", capture_->snapshot.scope.persisted_ref}}, Durability::PowerLoss)
        : NestedHookDispatchSession::WriteSkip(*writer_, meta.dispatch_id, meta.hook_point, std::string(reason),
            meta.turn_id, meta.step_id, meta.action_id, Durability::ProcessCrash);
    StoreCaptured("hook.skipped", slot, std::move(receipt));
}

void V3MiddlewareEventSink::OnInvocationStarted(const InvocationMeta& meta) {
    const std::lock_guard<std::mutex> lock(mutex_);
    CaptureAttempt attempt{*this};
    auto* slot = PrepareCaptured(meta, MiddlewareReceiptStage::Started);
    if (capture_ && !slot) return;
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
    auto receipt = book->session.BeginInvocation(*writer_, meta.invocation_id, spec,
        capture_ ? Durability::PowerLoss : Durability::ProcessCrash);
    StoreCaptured("hook.started", slot, std::move(receipt));
}

void V3MiddlewareEventSink::OnInvocationCompleted(const InvocationMeta& meta,
                                                  std::optional<std::string> decision, std::uint64_t duration_ms) {
    const std::lock_guard<std::mutex> lock(mutex_);
    CaptureAttempt attempt{*this};
    auto* slot = PrepareCaptured(meta, MiddlewareReceiptStage::Completed);
    if (capture_ && !slot) return;
    if (DispatchBook* book = LockBookFor(meta); book != nullptr) {
        auto receipt = book->session.CompleteInvocation(
            *writer_, meta.invocation_id, meta.hook_id, std::move(decision),
            /*output_ref=*/std::nullopt, duration_ms);
        StoreCaptured("hook.completed", slot, std::move(receipt));
    }
}

void V3MiddlewareEventSink::OnInvocationFailed(const InvocationMeta& meta, std::string_view error_code,
                                               std::uint64_t duration_ms) {
    const std::lock_guard<std::mutex> lock(mutex_);
    CaptureAttempt attempt{*this};
    auto* slot = PrepareCaptured(meta, MiddlewareReceiptStage::Failed);
    if (capture_ && !slot) return;
    if (DispatchBook* book = LockBookFor(meta); book != nullptr) {
        auto receipt = book->session.FailInvocation(*writer_, meta.invocation_id,
                                                                  std::string(error_code), duration_ms);
        StoreCaptured("hook.failed", slot, std::move(receipt));
    }
}

void V3MiddlewareEventSink::OnInvocationCancelled(const InvocationMeta& meta, std::string_view reason) {
    const std::lock_guard<std::mutex> lock(mutex_);
    CaptureAttempt attempt{*this};
    auto* slot = PrepareCaptured(meta, MiddlewareReceiptStage::Cancelled);
    if (capture_ && !slot) return;
    if (DispatchBook* book = LockBookFor(meta); book != nullptr) {
        auto receipt = book->session.CancelInvocation(*writer_, meta.invocation_id,
                                                                    std::string(reason));
        StoreCaptured("hook.cancelled", slot, std::move(receipt));
    }
}

void V3MiddlewareEventSink::OnEffectSettled(const InvocationMeta& meta, std::string_view effect_type,
                                            bool applied, std::string_view reason, const nlohmann::json& value) {
    const std::lock_guard<std::mutex> lock(mutex_);
    CaptureAttempt attempt{*this};
    auto* slot = PrepareCaptured(meta, applied ? MiddlewareReceiptStage::EffectsApplied : MiddlewareReceiptStage::EffectsRejected);
    if (capture_ && !slot) return;
    DispatchBook* book = LockBookFor(meta);
    if (book == nullptr) {
        return;
    }
    auto receipt =
        applied
            ? book->session.ApplyEffect(*writer_, meta.invocation_id, std::string(effect_type),
                                        /*applied_value_ref=*/std::optional<nlohmann::json>{std::in_place, value},
                                        Durability::PowerLoss)
            : book->session.RejectEffect(*writer_, meta.invocation_id, std::string(effect_type),
                                         std::string(reason), Durability::PowerLoss);
    StoreCaptured(applied ? "hook.effects.applied" : "hook.effects.rejected", slot, std::move(receipt));
}

void V3MiddlewareEventSink::OnContinuationConsumed(const InvocationMeta& meta) {
    const std::lock_guard<std::mutex> lock(mutex_);
    CaptureAttempt attempt{*this};
    auto* slot = PrepareCaptured(meta, MiddlewareReceiptStage::ContinuationConsumed);
    if (capture_ && !slot) return;
    if (DispatchBook* book = LockBookFor(meta); book != nullptr) {
        auto receipt = book->session.ContinuationConsumed(*writer_, meta.invocation_id,
            capture_ ? Durability::PowerLoss : Durability::ProcessCrash);
        StoreCaptured("hook.continuation.consumed", slot, std::move(receipt));
    }
}

void V3MiddlewareEventSink::OnOutputProposed(const InvocationMeta& meta, std::string_view phase,
                                             const nlohmann::json& candidate) {
    const std::lock_guard<std::mutex> lock(mutex_);
    CaptureAttempt attempt{*this};
    auto* slot = PrepareCaptured(meta, MiddlewareReceiptStage::OutputProposed);
    if (capture_ && !slot) return;
    if (DispatchBook* book = LockBookFor(meta); book != nullptr) {
        auto receipt = book->session.OutputProposed(
            *writer_, meta.invocation_id, std::string(phase), candidate,
            capture_ ? Durability::PowerLoss : Durability::ProcessCrash);
        StoreCaptured("hook.output.proposed", slot, std::move(receipt));
    }
}

std::vector<std::string> V3MiddlewareEventSink::recent_errors() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return recent_errors_;
}

}  // namespace lubancode::runtime
