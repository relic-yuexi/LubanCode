// GatewayAutomationPump(常驻总装 V1 第一/第二/第四件事的总装):Gateway
// 有界主泵的真装配——实现 engine 层的 gateway::GatewayWorkPump 合同口。
//
// 一个泵实例 = 一只持锁 Gateway 的业务面:
//   - AutomationStore(任务账) + DurableReplyOutbox(投递账)+ Headless
//     执行工厂(backend/registry 借用);
//   - TickOnce 的推进次序(每 tick 有界):消费 job 控制命令 → 恢复扫描
//     (未结算 occurrence 的跨账裁决)→ outbox 投递 → 至多一枚新执行
//     (SessionWorkScheduler 公平泵取件,WorkKind::AutomationDue)。
//
// 恢复裁决(V1 面-V2 §八全表;从领域 bound 行定位原场,不扫全 workspace):
//   - bound 行在、V3 有 assistant、无 selection → 补 selection(冻结策略
//     重算同一 selectionId,不调模型);
//   - bound 行在、V3 selection 已在 → 补 outbox 投影;
//   - bound 行在、V3 无 assistant(未开跑/生成中断)→ needs_review
//     (V1 不盲目重跑:重派归 V2 的"核实无旧执行后重派同一 work");
//   - bound 行不在(claim 后崩)→ needs_review(V1 保守:核实手段太贵,
//     不猜"没开过场")。
//   执行成功与否与投递分开结算(§九:业务执行成功与投递失败分别显示)。
//
// 取件排序复用 SessionWorkScheduler(单子 V1 第一件:"复用现有
// SessionWorkScheduler"):候选包成 SessionWork{kind=AutomationDue},
// PumpNextWork 公平取一枚。V1 候选只有 automation 一族,排序泵走同一条
// 路——V2 加周期调度/渠道候选时不再改装配。
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "gateway/automation_store.hpp"
#include "gateway/profile.hpp"  // GatewayProfilePaths(automation/delivery 落位)
#include "gateway/reply_outbox.hpp"
#include "gateway/work_pump.hpp"
#include "runtime/headless_executor.hpp"
#include "workspace/identity.hpp"

namespace api {
class Backend;
}
namespace lubancode::tools {
class ToolRegistry;
}

namespace lubancode::runtime {

class GatewayAutomationPump : public gateway::GatewayWorkPump {
public:
    struct Options {
        gateway::GatewayProfilePaths paths;    // automation/delivery 落位
        std::filesystem::path workspaces_root; // 会话持久化根(递 SessionService)
        workspace::WorkspaceIdentity workspace_identity;  // 身份(key 进恢复 resolver)
        std::string cwd_utf8;
        std::string lubancode_version;
        std::string wire_name;
        std::string model;
        channel::ToolRoutePolicy tools;
        hooks::HookDispatcher* hook_dispatcher = nullptr;
        // 预算三根硬线(0 = 不设;生产装配应设)。
        int max_steps_per_turn = 0;
        int max_wall_secs = 0;
        std::int64_t max_total_tokens = 0;
        // 故障注入(测试专用;生产恒空)。fault_injection 透传执行器(生成
        // 结束/选择提交两窗);fault_after_enqueue 是泵自己的窗:入 outbox
        // 后、发布本地文件前"进程死掉"。返回非空即死。
        std::function<std::string(HeadlessExecutor::Options::FaultPoint)> fault_injection;
        std::function<std::string()> fault_after_enqueue;
    };

    // 打开两本领域账;打不开给 error(ok=false),装配层让 Gateway 起不来
    //(账都开不了,业务面不该空跑)。
    struct OpenResult {
        bool ok = false;
        std::string error;
    };
    static OpenResult Open(GatewayAutomationPump* out, api::Backend& backend,
                           tools::ToolRegistry& registry, Options options);

    ~GatewayAutomationPump() override;
    GatewayAutomationPump(GatewayAutomationPump&&) = delete;
    GatewayAutomationPump(const GatewayAutomationPump&) = delete;
    GatewayAutomationPump& operator=(const GatewayAutomationPump&) = delete;

    // ---- GatewayWorkPump 合同 ------------------------------------------------
    bool TickOnce(std::int64_t now_ms) override;
    void StopAccepting() override;
    bool Close(int grace_ms) override;

    // ---- 观测/测试 -----------------------------------------------------------
    // 泵内账的只读捷径(status 走 gateway::ProbeStatusSections,不经泵)。
    gateway::AutomationStore* store() { return store_ ? &*store_ : nullptr; }
    gateway::DurableReplyOutbox* outbox() { return outbox_ ? &*outbox_ : nullptr; }
    bool accepting() const { return accepting_.load(); }
    // ownerEpoch(= GatewayProcess 取锁后递进的 boot_id; fencing 代号)。
    void set_owner_epoch(const std::string& epoch) override { owner_epoch_ = epoch; }

private:
    struct RecoveryOutcome {
        bool progressed = false;  // 本轮有没有补齐/结算任何 occurrence
        std::string error;        // 账写不进等致命错(泵停)
    };
    RecoveryOutcome SweepRecovery(std::int64_t now_ms);
    // 单枚 occurrence 的恢复裁决(§八 V1 面)。返回空 = 无事可做;非空 =
    // 结算的 outcome(succeeded=补齐投递链,needs_review=停审)。
    std::optional<std::string> RecoverOccurrence(const gateway::AutomationOccurrence& occurrence,
                                                 std::int64_t now_ms, std::string* error);
    bool RunOneOccurrence(std::int64_t now_ms, std::string* error);

    api::Backend* backend_ = nullptr;
    tools::ToolRegistry* registry_ = nullptr;
    Options options_;
    std::optional<gateway::AutomationStore> store_;
    std::optional<gateway::DurableReplyOutbox> outbox_;
    std::atomic<bool> accepting_{true};
    std::atomic<bool> closed_{false};
    std::string owner_epoch_;
};

}  // namespace lubancode::runtime
