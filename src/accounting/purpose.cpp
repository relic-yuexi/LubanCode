#include "accounting/purpose.hpp"

namespace lubancode::accounting {
namespace {

struct PurposeEntry {
    RequestPurpose purpose;
    const char* name;
};

constexpr PurposeEntry kEntries[] = {
    {RequestPurpose::MainTurn, "main_turn"},
    {RequestPurpose::SubagentTurn, "subagent_turn"},
    {RequestPurpose::WorkflowNode, "workflow_node"},
    {RequestPurpose::CompactMap, "compact_map"},
    {RequestPurpose::CompactReduce, "compact_reduce"},
    {RequestPurpose::MemoryExtract, "memory_extract"},
    {RequestPurpose::TitleRefine, "title_refine"},
    {RequestPurpose::DoctorProbe, "doctor_probe"},
    {RequestPurpose::GoalContinue, "goal_continue"},
    {RequestPurpose::LoopIteration, "loop_iteration"},
    {RequestPurpose::InsightsModelReview, "insights_model_review"},
    {RequestPurpose::OtherHostRequest, "other_host_request"},
};

}  // namespace

const char* PurposeName(RequestPurpose purpose) {
    for (const auto& entry : kEntries) {
        if (entry.purpose == purpose) {
            return entry.name;
        }
    }
    return "";
}

std::optional<RequestPurpose> PurposeFromName(std::string_view name) {
    for (const auto& entry : kEntries) {
        if (name == entry.name) {
            return entry.purpose;
        }
    }
    return std::nullopt;
}

const std::vector<RequestPurpose>& AllPurposes() {
    static const std::vector<RequestPurpose> purposes = [] {
        std::vector<RequestPurpose> result;
        result.reserve(sizeof(kEntries) / sizeof(kEntries[0]));
        for (const auto& entry : kEntries) {
            result.push_back(entry.purpose);
        }
        return result;
    }();
    return purposes;
}

}  // namespace lubancode::accounting
