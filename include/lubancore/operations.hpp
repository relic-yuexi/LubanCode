#pragma once

#include <string>

namespace lubancore {

// operation_id is Session scoped; external callers address (session_id, operation_id).
struct Receipt { std::string operation_id; std::string input_id; bool duplicate = false; };
enum class OperationState { Accepted, Running, Succeeded, Failed, Cancelled, Indeterminate };
struct Operation {
    std::string operation_id;
    std::string turn_id;
    OperationState state = OperationState::Accepted;
    std::string final_text;
    std::string error;
    bool result_persisted = false;
};
} // namespace lubancore
