// ApprovalChannel 的实现(按代理状态投影单 P2:审批的独立响应通道;
// P3 补 owner/turn/generation 三层目标绑定)。

#include "cli/approval_channel.hpp"

namespace lubancode::cli {

ApprovalChannel& SessionApprovalChannel() {
    static ApprovalChannel channel;
    return channel;
}

std::optional<std::future<bool>> ApprovalChannel::Submit(int owner_task_id, std::string tool_name,
                                                         std::function<bool()> presenter,
                                                         std::uint64_t session_generation, std::string turn_id) {
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
        slot.session_generation = session_generation;
        slot.turn_id = std::move(turn_id);
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

std::vector<std::string> ApprovalChannel::PendingOwnerLabelsOutside(int viewed_task_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> labels;
    for (const Slot& slot : pending_) {
        if (slot.owner_task_id == viewed_task_id || slot.presenter == nullptr) {
            continue;  // 本页的、或已取出在答的,不进通知位
        }
        std::string label = slot.owner_task_id == 0 ? std::string("main")
                                                   : "#" + std::to_string(slot.owner_task_id);
        bool seen = false;
        for (const std::string& existing : labels) {
            if (existing == label) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            labels.push_back(std::move(label));
        }
    }
    return labels;
}

std::size_t ApprovalChannel::PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

void ApprovalChannel::DenyWhere(const std::function<bool(const Slot&)>& matches) {
    std::vector<std::shared_ptr<std::promise<bool>>> decisions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        decisions.reserve(pending_.size());
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (matches(*it)) {
                decisions.push_back(it->decision);
                it = pending_.erase(it);
                continue;
            }
            ++it;
        }
    }
    for (const auto& decision : decisions) {
        decision->set_value(false);
    }
}

void ApprovalChannel::DenyAllPending() {
    DenyWhere([](const Slot&) { return true; });
}

void ApprovalChannel::DenyPendingForOwner(int owner_task_id) {
    DenyWhere([owner_task_id](const Slot& slot) { return slot.owner_task_id == owner_task_id; });
}

void ApprovalChannel::DenyPendingForTurn(const std::string& turn_id) {
    if (turn_id.empty()) {
        return;
    }
    DenyWhere([&turn_id](const Slot& slot) { return slot.turn_id == turn_id; });
}

void ApprovalChannel::DenyStaleGenerations(std::uint64_t current_generation) {
    DenyWhere([current_generation](const Slot& slot) {
        // 0 = 未绑定(单测/旧路),不掺和换代收口。
        return slot.session_generation != 0 && slot.session_generation != current_generation;
    });
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
