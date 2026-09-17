// GatewayAutomationPump(常驻总装 V1 第一/第二/第四件事 + V2 周期调度的
// 总装):Gateway 有界主泵的真装配——实现 engine 层的 gateway::
// GatewayWorkPump 合同口。
//
// 一个泵实例 = 一只持锁 Gateway 的业务面:
//   - AutomationStore(任务账) + DurableReplyOutbox(投递账)+ Headless
//     执行工厂(backend/registry 借用);
//   - TickOnce 的推进次序(每 tick 有界):消费 job 控制命令(含 V2 领域
//     操作 update/pause/resume/cancel/import-loop)→ 周期拍点生成
//     (SweepSchedule:misfire 政策/同 slot 合并/队列帽)→ 恢复扫描(未
//     结算 occurrence 的跨账裁决)→ outbox 投递 → 至多一枚新执行
//     (SessionWorkScheduler 公平泵取件,WorkKind::AutomationDue)。
//
// 恢复裁决(V2 面,§八表;从领域 bound 行定位原场,不扫全 workspace):
//   - bound 行不在(claim 后崩,无开轮事实——领域绑定先于一切模型/工具
//     动作)→ 重派同一 occurrence(attempt+1,occurrenceId 不洗;§八
//     "对账后重派同一 work,另记 attempt");attempt 帽(3)到顶 →
//     needs_review;任务已取消 → cancelled。
//   - bound 行在、V3 有 assistant、无 selection → 补 selection(冻结策略
//     重算同一 selectionId,不调模型);
//   - bound 行在、V3 selection 已在 → 补 outbox 投影;
//   - bound 行在、V3 无 assistant(开轮后崩,模型请求可能已发)→
//     needs_review(§八"未知副作用停住",不盲目重跑);
//   - heartbeat(notify_on_change)任务:正文与上次已通知版本相同 → 不投
//     递,记观察账;检查失败永远投递失败通知,不记"无变化"。
//   执行成功与否与投递分开结算(§九:业务执行成功与投递失败分别显示)。
//   多次 resume(泵销毁重建)保持同一 work/occurrence 身份、同一
//   deliveryId——结算过的不重开,不重跑已完成工作。
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
#include <vector>

#include "gateway/automation_store.hpp"
#include "gateway/profile.hpp"  // GatewayProfilePaths(automation/delivery 落位)
#include "gateway/reply_outbox.hpp"
#include "gateway/work_pump.hpp"
#include "runtime/headless_executor.hpp"
#include "workspace/identity.hpp"

// 前置声明(不拉 backend/registry 重头;注意必须住在 lubancode 内——
// 全局 namespace api 会与 using namespace lubancode 的调用方打歧义)。
namespace lubancode::api {
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
        std::string skills_prompt;
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
        // 工具确认注入口(常驻助理 Web 主界面单 W2):透传执行器。空 =
        // 既有无人值守合同(ChannelConfirmAllows,allow 名单没列 = 拒)。
        std::function<HeadlessExecutor::Options::ToolConfirmDecision(
            const std::string& tool_use_id, const std::string& name,
            const nlohmann::json& input)>
            on_tool_confirm;
        // 活模型名取值口(W2,助理宿主用):非空时每次执行取当前模型名
        //(宿主递 config 快照,首配后新任务吃新账);空 = options.model
        // 定死(V1 gateway 行为不变)。调用在泵线程(执行窗内),宿主的
        // 快照自身线程安全。在飞执行不追改——下一次 occurrence 生效。
        std::function<std::string()> model_provider;
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
    // 显式默认构造:下面的移动/拷贝删除声明会抑制隐式默认构造,而装配
    // 层用 std::optional<T>::emplace() 原地构造,需要它。
    GatewayAutomationPump();
    GatewayAutomationPump(GatewayAutomationPump&&) = delete;
    GatewayAutomationPump(const GatewayAutomationPump&) = delete;
    GatewayAutomationPump& operator=(const GatewayAutomationPump&) = delete;

    // ---- GatewayWorkPump 合同 ------------------------------------------------
    bool TickOnce(std::int64_t now_ms) override;
    void StopAccepting() override;
    bool Close(int grace_ms) override;
    // V4:Close 之后仍未收净的 occurrence id(同步泵下 TickOnce 已收口,
    // 恒空;异步泵接上后由宽限等待逻辑填充)。写进 shutdown 账行
    // uncollected_work,不结算成 cancelled。
    std::vector<std::string> UncollectedWorkIds() const override;

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
    // V4:当前执行中的 occurrence(RunOneOccurrence 的 claim→结算窗)与
    // Close 时未收净的清单(见 UncollectedWorkIds)。同步泵下主循环退出
    // 后 TickOnce 已收口,两者恒空——机制先立,异步化后生效。
    std::string in_flight_;
    std::vector<std::string> uncollected_;
};

}  // namespace lubancode::runtime
