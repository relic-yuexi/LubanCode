#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <lubancore/api.hpp>
#include "runtime/session_service.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancore::detail {
// The existing strict SDK operation-fact validation, shared by recovery
// preflight, locked opening and the post-assembly recovery loader.
Result<void> ValidateOperationLedger(const std::filesystem::path& session_dir);

enum class OperationTurnKnowledge {
    NotConsumed, ConsumedNoAnchor, AppendUnconfirmed, CommittedPublicationGap, Bound
};
struct MainOperationTurnStart {
    OperationTurnKnowledge knowledge = OperationTurnKnowledge::NotConsumed;
    std::optional<lubancode::runtime::SessionService::QueuedInput> input;
    std::string turn_id;
    std::optional<lubancode::trajectory::v3::WriteReceipt> receipt;
    std::optional<lubancode::trajectory::v3::OperationTurnBindingFacts> facts;
    Error error;
    bool ready() const noexcept { return knowledge == OperationTurnKnowledge::Bound; }
};
// Internal, explicit producer. Pops once; no caller identity DTO and no default
// SDK/CLI registration. Publisher failure preserves the committed first receipt.
MainOperationTurnStart BeginMainOperationTurn(lubancode::runtime::SessionService& service,
    const std::function<void(const lubancode::trajectory::v3::OperationTurnBindingFacts&)>& publish = {});

enum class OperationTurnMaterialState { NotApplicable, Incomplete, Rejected, Validated };
struct OperationTurnMaterialCheck {
    OperationTurnMaterialState state = OperationTurnMaterialState::NotApplicable;
    std::vector<lubancode::trajectory::v3::OperationTurnBindingFacts> facts;
    Error error;
};
// No live authority, writer, repairs, final synthesis or dispatch on this path.
OperationTurnMaterialCheck CheckMainOperationTurnBindings(const std::filesystem::path& session_dir,
    const lubancode::trajectory::v3::V3Ledger& ledger);
} // namespace lubancore::detail
