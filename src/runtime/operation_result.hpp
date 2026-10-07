#pragma once
#include <string>
#include <nlohmann/json.hpp>

namespace lubancode::runtime {
// The existing SDK result payload. Both persistence paths use these exact fields.
inline nlohmann::json MakeSdkOperationResultPayload(const std::string& operation_id,
    const std::string& turn_id, const std::string& final_text, const std::string& error, bool complete) {
    return {{"operationId", operation_id}, {"turnId", turn_id},
        {"finalText", final_text}, {"error", error}, {"complete", complete}};
}
} // namespace lubancode::runtime
