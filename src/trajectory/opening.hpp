#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::trajectory {
namespace v3 {
struct V3Ledger;
}

struct V3OpeningContext {
    std::filesystem::path session_dir;
    std::string session_id;
    // Only borrowed during this invocation, after the session file lock is held.
    // A participant must not retain this pointer or invoke another opening.
    const v3::V3Ledger* source = nullptr;
};

// Internal, synchronous pre-publication gate. It may commit owned host metadata,
// but must not scan resources, run factories or connect to external services.
// The returned object may contain only an object-valued hostBindings member.
using V3OpeningParticipant = std::function<std::expected<nlohmann::json, std::string>(
    const V3OpeningContext&)>;

}  // namespace lubancode::trajectory
