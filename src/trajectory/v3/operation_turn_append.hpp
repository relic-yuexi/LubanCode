#pragma once
#include <string>
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::trajectory::v3 {
// Only the existing native append half. Admission, pop and receipt publication
// stay with their owning producer; no callback or second queue lives here.
inline EventDraft PrepareMainOperationTurnBinding(const OperationTurnBindingFacts& facts) {
    EventDraft event;
    event.kind = EventKindV3::SdkOperationTurnBound;
    event.turn_id = facts.turn_id;
    event.payload = {{"layout", std::string(facts.provenance_hash.empty()
        ? kSdkMainOperationTurnLayout : kManagedMainOperationTurnLayout)}, {"version", 1},
        {"operationId", facts.operation_id}, {"inputId", facts.input_id}, {"payloadHash", facts.payload_hash}};
    if (!facts.provenance_hash.empty()) event.payload["provenanceHash"] = facts.provenance_hash;
    return event;
}
inline WriteReceipt AppendMainOperationTurnBinding(V3Writer& writer, EventDraft event) {
    return writer.AppendEvent(std::move(event), Durability::PowerLoss);
}
} // namespace lubancode::trajectory::v3
