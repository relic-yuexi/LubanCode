// ApprovalChannel 的实现(按代理状态投影单 P2:审批的独立响应通道)。

#include "cli/approval_channel.hpp"

namespace lubancode::cli {

ApprovalChannel& SessionApprovalChannel() {
    static ApprovalChannel channel;
    return channel;
}

std::optional<std::future<bool>> ApprovalChannel::Submit(int owner_task_id, std::string tool_name,
                                                         std::function<bool()> presenter) {
    auto decision = std::make_shared<std::promise<bool>>();
    std::future<bool> future = decision->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!server_present_) {
            return std::nullopt;  // 没人可问:调用方就地跑 presenter(旧路)
        }
        Slot slot;
        slot.id = next_id_++;
        slot.owner_task_id = owner_task_id;
        slot.tool_name = std::move(tool_name);
        slot.presenter = std::move(presenter);
        slot.decision = std::move(decision);
        pending_.push_back(std::move(slot));
    }
    return future;
}

std::optional<ApprovalChannel::Pending> ApprovalChannel::TakeForViewer(int viewed_task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->owner_task_id != viewed_task_id || it->presenter == nullptr) {
            continue;  // 不是这页的,或已取出在答(只剩回执)——不重复问
        }
        Pending taken;
        taken.id = it->id;
        taken.owner_task_id = it->owner_task_id;
        taken.tool_name = std::move(it->tool_name);
        taken.presenter = std::move(it->presenter);
        // promise 留在表里(Resolve 按 id 找);表项改成"已取出、只剩回执"。
        it->presenter = nullptr;
        it->tool_name.clear();
        return taken;
    }
    return std::nullopt;
}

void ApprovalChannel::Resolve(std::uint64_t id, bool allowed) {
    std::shared_ptr<std::promise<bool>> decision;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->id != id) {
                continue;
            }
            decision = it->decision;
            pending_.erase(it);
            break;
        }
    }
    if (decision != nullptr) {
        decision->set_value(allowed);
    }
}

bool ApprovalChannel::HasPendingOutside(int task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Slot& slot : pending_) {
        if (slot.owner_task_id != task_id) {
            return true;
        }
    }
    return false;
}

std::size_t ApprovalChannel::PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

void ApprovalChannel::DenyAllPending() {
    std::vector<std::shared_ptr<std::promise<bool>>> decisions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        decisions.reserve(pending_.size());
        for (Slot& slot : pending_) {
            decisions.push_back(slot.decision);
        }
        pending_.clear();
    }
    for (const auto& decision : decisions) {
        decision->set_value(false);
    }
}

void ApprovalChannel::RegisterServer() {
    std::lock_guard<std::mutex> lock(mutex_);
    server_present_ = true;
}

void ApprovalChannel::ClearServer() {
    std::lock_guard<std::mutex> lock(mutex_);
    server_present_ = false;
}

}  // namespace lubancode::cli
