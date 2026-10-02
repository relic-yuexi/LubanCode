#pragma once

#include <filesystem>
#include <lubancore/api.hpp>

namespace lubancore::detail {
// The existing strict SDK operation-fact validation, shared by recovery
// preflight, locked opening and the post-assembly recovery loader.
Result<void> ValidateOperationLedger(const std::filesystem::path& session_dir);
} // namespace lubancore::detail
