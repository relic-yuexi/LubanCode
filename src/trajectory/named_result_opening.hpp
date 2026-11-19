#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include "trajectory/named_result_blobs.hpp"

namespace lubancode::trajectory {
namespace v3 { struct V3Ledger; }

struct NamedResultOpening {
    NamedResultLease lease;
    std::optional<nlohmann::json> binding;
};
// Called only by the real locked owner, before Start/Continue and before a
// provider Open. The source must be the locked, verified same-ID volume.
std::expected<NamedResultOpening, std::string> OpenLockedNamedResults(
    const CasScope&, const std::filesystem::path&, const std::shared_ptr<NamedResultFactory>&,
    const v3::V3Ledger* source = nullptr);
std::expected<void, std::string> MergeNamedResultBinding(nlohmann::json& metadata,
    const std::optional<nlohmann::json>& binding);
bool HasExternalNamedResultBinding(const v3::V3Ledger&);

} // namespace lubancode::trajectory
