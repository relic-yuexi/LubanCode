#include "hooks/detached.hpp"

namespace lubancode::hooks {

DetachedHookSession::DetachedHookSession(HookDispatcher* dispatcher, HookContext base_context)
    : dispatcher_(dispatcher), context_(std::move(base_context)) {
    if (dispatcher_ != nullptr) {
        snapshot_ = dispatcher_->PolicySnapshot();
    }
}

bool DetachedHookSession::HasHandlersFor(HookEvent event) const {
    for (const auto& def : snapshot_) {
        if (def.event == event) {
            return true;
        }
    }
    return false;
}

HookEventResult DetachedHookSession::Emit(HookEvent event, const HookPayload& payload) {
    if (dispatcher_ == nullptr || snapshot_.empty()) {
        HookEventResult empty;
        return empty;
    }
    HookEventResult merged = HookDispatcher::EmitDetached(snapshot_, event, payload, context_);
    dispatcher_->PostExternalRecords(std::move(merged.records));
    return merged;
}

void DetachedHookSession::PostWarning(const std::string& warning) {
    if (dispatcher_ != nullptr) {
        dispatcher_->PostExternalRecords({}, warning);
    }
}

}  // namespace lubancode::hooks
