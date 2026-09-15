#pragma once

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "api/types.hpp"

namespace lubancode::agent {

// First-use byte budgets only. The caller serializes the selected adapter input,
// subtracts fixed JSON bytes and separately reserved output/protocol capacity.
// JSON escaping is budgeted explicitly by the caller, never hidden in an online
// token correction. Final input is measured once with ceil(total UTF-8 bytes/4).
struct ToolBatchBudgetPlan {
    std::vector<std::size_t> preview_bytes;
    std::size_t total_preview_bytes = 0;
    bool reduced = false;
    std::string error;
};

inline ToolBatchBudgetPlan PlanToolBatchBudget(const api::Message& results,
                                               std::size_t available) {
    ToolBatchBudgetPlan plan;
    std::set<std::string> ids;
    for (const auto& block : results.content) {
        const auto* result = std::get_if<api::ToolResultBlock>(&block);
        if (result == nullptr || result->tool_use_id.empty() ||
            !ids.insert(result->tool_use_id).second) {
            plan.error = "tool_batch.invalid_pairing";
            return plan;
        }
        // Tiny bodies still need a nonzero cap; an empty success is legitimate.
        plan.preview_bytes.push_back(std::min<std::size_t>(32768,
                                                          std::max<std::size_t>(result->capture_complete ? 1 : 1024,
                                                                                result->content.size())));
    }
    if (plan.preview_bytes.empty()) return plan;
    // Water filling keeps small results whole, shares the remaining budget among
    // larger calls and never merges identities or edits a previously sent result.
    std::size_t low = 0, high = 32768;
    const auto fits = [&](std::size_t cap) {
        std::size_t left = available;
        for (auto desired : plan.preview_bytes) {
            const auto allocated = std::min(desired, cap);
            if (allocated > left) return false;
            left -= allocated;
        }
        return true;
    };
    while (low < high) {
        const auto middle = low + (high - low + 1) / 2;
        if (fits(middle)) low = middle;
        else high = middle - 1;
    }
    for (auto& desired : plan.preview_bytes) {
        // Below 1 KiB, fail explicitly rather than discard evidence paths and
        // execution status. The bridge can still reject an unrepresentable cap.
        if (std::min(desired, low) < std::min<std::size_t>(desired, 1024)) {
            plan.error = "tool_batch.minimum_preview_exceeds_capacity";
            return plan;
        }
        plan.reduced = plan.reduced || desired > low;
        desired = std::min(desired, low);
        plan.total_preview_bytes += desired;
    }
    return plan;
}

// Validate the closed group before publication and again after the preview hook.
inline bool ToolBatchPairingMatches(const api::Message& calls, const api::Message& results) {
    std::set<std::string> expected;    // 须配对的调用
    std::set<std::string> deferrable;  // async 标记:可欠账,也可本批就配
    for (const auto& block : calls.content) {
        if (const auto* call = std::get_if<api::ToolUseBlock>(&block)) {
            if (call->id.empty() || !expected.insert(call->id).second) return false;
            // 异步工具单 §4:provider 标了 async 的原生调用允许本批欠账
            // (native_deferred 的最终结果只配原 call,由规划器在下一次
            // 请求边界投递;job_handle 接单结果本批就配也合法)。没标
            // async 或标记不符的一律按普通同步配对,不许凭本地配置留悬
            // 空调用。
            if (call->async_call) {
                deferrable.insert(call->id);
            }
        }
    }
    for (const auto& block : results.content) {
        const auto* result = std::get_if<api::ToolResultBlock>(&block);
        if (result == nullptr || expected.erase(result->tool_use_id) != 1) return false;
    }
    for (const auto& id : expected) {
        if (deferrable.count(id) == 0) return false;
    }
    return true;
}

}  // namespace lubancode::agent
