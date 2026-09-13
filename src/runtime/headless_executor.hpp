// HeadlessExecutor(常驻总装 V1 第三/第四件事):从 ChannelSessionHost/
// AgentChannelEngine/SessionService 提炼的共用 headless 装配——Gateway
// 自动任务一次执行的完整执行器。
//
// 与既有两路的关系(单子 §二:"提炼共同装配,补持久受理、完整工具账
// 与恢复入口";本单 §2.1:更大的中立 RuntimeAssembly 抽取归 LubanCore
// 单阶段 B,这里只做 Gateway 需要的那份,不建大框架):
//   - SessionService:开场/输入接纳/收口走三端同一服务路(持久受理四病
//     修复后的账路,operations.jsonl + operations-inputs/ 原件);
//   - AgentChannelEngine:执行接线(TurnEventAdapter/TurnWiring/hook 四
//     点/工具 fail closed)按同款搬来并补齐它的遗留——工具栅栏接
//     ToolTraceHub(真实工具 Action 落账),预算三根硬线经
//     AgentRuntimeProfile,取消链经 AgentLoop::Run 的 cancel 旗;
//   - 每次执行一场 fresh V3 会话(cron 默认 fresh session 的 V1 落法;
//     continuation 归 V2)。
//
// V3 强制:开场不是 v3 格式即明败(单子 §二:"Gateway 新执行固定要求
// V3;遇 v2 新写配置明报拒绝,不偷偷双写")。
//
// reply selection(单子 §九,V1 冻结策略):turn 成功后按冻结策略从已
// 提交事实唯一定位最终正文——本轮最后一条 assistant 消息的 text 块全
// 文;selectionId = "sel-" + turnId(恢复器按同一纯函数重算,同一 id,
// 不调模型)。原件先落稳(replies/<selectionId>.txt),选择事实
// (reply.selection.committed)后提交——顺序就是栅栏,崩溃窗口靠恢复器
// 幂等补齐。PlanReplySelection 是纯函数,执行路与恢复路共用一份。
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "api/backend.hpp"
#include "channel/channel_router.hpp"
#include "hooks/dispatcher.hpp"
#include "runtime/session_runtime.hpp"
#include "runtime/session_service.hpp"
#include "tools/registry.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::runtime {

// work↔turn 绑定事实(gateway.work.bound)的载荷。workId/sourceId 全局
// 带域(§11.1);ownerEpoch 拦旧 worker 迟到提交;attempt 从 1 起。
struct HeadlessWorkBinding {
    std::string work_id;       // 统一执行意图 id(V1 = occurrenceId)
    std::string source_kind;   // 触发来源(V1 = "automation")
    std::string source_id;     // 来源 id(V1 = jobId)
    std::string owner_epoch;   // fencing 代号(= 持锁 boot_id)
    std::uint64_t attempt = 1;
};

// 冻结的回复选择策略(V1 版):从已提交 V3 事实唯一定位可外送正文。
// selectionId = "sel-" + turnId——执行与恢复同一式,不变洗。
struct ReplySelectionPlan {
    bool ok = false;
    std::string error;                 // 无 assistant/正文为空等原因
    std::string selection_id;          // "sel-" + turnId
    std::string turn_id;
    std::string source_message_ref;    // 选定 assistant 的 V3 message id
    std::string completed_event_ref;   // 本轮最后 model.response.completed 事件 id
    std::string text;                  // text 块全文(冻结正文)
};
// 纯函数:给定 V3 流与 turnId,按冻结策略(plan=final_assistant_text)出
// 选择计划。turn 内无 assistant message / 正文空 = 不可选(如实报,不猜)。
ReplySelectionPlan PlanReplySelection(const trajectory::v3::V3Ledger& ledger,
                                      const std::string& turn_id);

// 幂等查重(执行路与恢复路共用):流上是否已有同 selectionId 的
// reply.selection.committed——有则不重复落,直接续 outbox 投影(§11
// "reply selection 已提交,outbox 尚未投影:从已提交选择补出同一 delivery")。
bool SelectionAlreadyCommitted(const trajectory::v3::V3Ledger& ledger,
                               const std::string& selection_id);

// 把选择计划落成事实(执行路与恢复路共用):原件先落稳
// (replies_dir/<selectionId>.txt,原子写,已在不重写、hash 不符拒),
// 再 reply.selection.committed 落 V3(PowerLoss)。selection 已在流上 =
// 幂等 no-op(返回 committed=true,不重复落)。
struct CommitReplySelectionResult {
    bool committed = false;    // 本次或此前已提交
    bool written_now = false;  // 本次真落了行
    std::string error_code;    // selection.artifact_failed | selection.append_failed
    std::string error;
};
CommitReplySelectionResult CommitReplySelection(trajectory::v3::V3Writer* writer,
                                                const std::filesystem::path& replies_dir,
                                                const ReplySelectionPlan& plan,
                                                const std::string& session_id);

class HeadlessExecutor {
public:
    struct Options {
        // 会话落位(与 SessionService 三端同形)。
        std::filesystem::path workspaces_root;  // 空 = 生产默认
        std::filesystem::path workspace_root;   // 身份裁决起点(fallback identity)
        std::string cwd_utf8;
        std::string lubancode_version;
        std::string wire_name;
        std::string model;
        // 回复原件落位(delivery/replies;由泵递进)。
        std::filesystem::path replies_dir;
        // 工具授权(fail closed:allow 名单没列 = 拒,同渠道 §16.1)。
        channel::ToolRoutePolicy tools;
        // hook 核(空 = 没装配,四点一处不调)。
        hooks::HookDispatcher* hook_dispatcher = nullptr;
        // 预算三根硬线(AgentRuntimeProfile;0 = 不设——生产装配应设)。
        int max_steps_per_turn = 0;
        int max_wall_secs = 0;
        std::int64_t max_total_tokens = 0;
        // 故障注入(测试专用;生产恒空):返回非空 = 在该点按"进程死掉"
        // 收场——不落后续任何账,Execute 返回 fault 错误码。放行门册用
        // 它钉三处硬杀窗口。
        enum class FaultPoint {
            AfterGeneration,        // turn 终态已落、reply selection 未提交
            AfterSelectionCommitted,  // selection 已落、outbox 未投影
        };
        std::function<std::string(FaultPoint)> fault_injection;
    };

    struct Result {
        bool ok = false;
        std::string error_code;  // gateway.requires_v3 | gateway.launch_failed |
                                 // gateway.input_rejected | gateway.fault_injected |
                                 // gateway.turn_failed | gateway.reply_unavailable
        std::string error;
        std::string session_id;   // 开出的 V3 场
        std::string turn_id;      // 本轮 id(绑定/恢复反查的锚)
        std::string reply_text;   // 冻结正文(ok 时)
        std::string selection_id; // "sel-" + turnId
    };

    // backend/registry 借用(须活过本次执行)。cancel 外部持有,置位即
    // 掐断模型流与协作取消的工具(取消链核齐点)。
    HeadlessExecutor(api::Backend& backend, tools::ToolRegistry& registry, Options options);

    // 一次执行:开场 → 受理 → 绑定 → 执行 → 收口 → reply selection。
    // on_bound 在领域绑定窗调(泵用它落 occurrence.bound,先于 V3
    // work.bound——恢复器优先走领域行定位原场)。
    Result Execute(const std::string& prompt, const HeadlessWorkBinding& binding,
                   const std::function<void(const std::string& session_id,
                                            const std::string& turn_id)>& on_bound,
                   const std::atomic<bool>* cancel);

private:
    api::Backend& backend_;
    tools::ToolRegistry& registry_;
    Options options_;
};

}  // namespace lubancode::runtime
