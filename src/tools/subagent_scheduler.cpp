// subagent_scheduler.hpp 的实现。
#include "tools/subagent_scheduler.hpp"

#include <utility>

namespace lubancode::tools {

SubagentScheduler::Slot::Slot(std::atomic<int>* active, std::atomic<int>* depth)
    : active_(active), depth_(depth) {}

SubagentScheduler::Slot::~Slot() {
    if (depth_ != nullptr) {
        depth_->fetch_sub(1);
    }
    if (active_ != nullptr) {
        active_->fetch_sub(1);
    }
}

void SubagentScheduler::SetGovernance(int max_active, int max_depth) {
    max_active_ = max_active > 0 ? max_active : 1;
    max_depth_ = max_depth > 0 ? max_depth : 1;
}

std::unique_ptr<SubagentScheduler::Slot> SubagentScheduler::Enter(bool foreground,
                                                                  std::string* error_out) {
    const int active = active_.fetch_add(1) + 1;
    // Slot 的构造是私友(只归本类),make_unique 的构造点在库内部不在本类
    // 语境,走裸 new(构造表达式仍在 Enter 里,友元够得着)。
    std::unique_ptr<Slot> slot(
        new Slot(&active_, foreground ? &foreground_depth_ : nullptr));
    if (foreground) {
        foreground_depth_.fetch_add(1);
    }
    if (active > max_active_) {
        if (error_out != nullptr) {
            *error_out = "子代理并发槽已满(" + std::to_string(max_active_) +
                         " 路同时在跑,前台后台合计):请等一项收尾,或调大 subagent.max_active。";
        }
        return nullptr;
    }
    if (foreground) {
        const int depth = foreground_depth_.load();
        if (depth > max_depth_) {
            if (error_out != nullptr) {
                *error_out = "已达子代理派工深度上限(" + std::to_string(max_depth_) +
                             " 层,subagent.max_depth 可调):请把任务拆平后再派,或由当前代理直接完成。";
            }
            return nullptr;
        }
    }
    return slot;
}

}  // namespace lubancode::tools
