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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
// 幂等 no-op(返回 committed=true,不重复落)。delivery_target 记进选择
// 事实(QQ 接入单 Q2:渠道路递渠道目标串;缺省沿 V1 的 local:file)。
struct CommitReplySelectionResult {
    bool committed = false;    // 本次或此前已提交
    bool written_now = false;  // 本次真落了行
    std::string error_code;    // selection.artifact_failed | selection.append_failed
    std::string error;
};
CommitReplySelectionResult CommitReplySelection(trajectory::v3::V3Writer* writer,
                                                const std::filesystem::path& replies_dir,
                                                const ReplySelectionPlan& plan,
                                                const std::string& session_id,
                                                const std::string& delivery_target = "local:file");

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
        // ---- 渠道会话(Q2) ----
        // 开场(含 resume-as-new)后回调:装配层把映射账落稳。幂等由回调
        // 自理(同键同场不重复落)。空 = 不记账(纯测试装配)。
        std::function<void(const std::string& session_key, const std::string& session_id)>
            on_session_mapped;
        // 进程内渠道活场上限(超帽按最旧淘汰:Close 后由映射账续
        // resume-as-new,上下文不丢)。0 = 不限(不推荐)。
        std::size_t max_live_channel_sessions = 8;
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

    // ---- 渠道会话的一轮(QQ 接入单 Q2,§六第三/四项) ----------------------
    // 与 Execute 共用同一份执行装配(受理→绑定→hook→执行→收口→reply
    // selection),差别只在开场与生命周期:
    //   - 进程内同 session_key 复用同一场(连续来信共享上下文,不另开空场);
    //   - 缓存未命中而有映射场(stored_session_id)→ resume-as-new 开新场,
    //     经 on_session_mapped 落映射账(§六第四项:重启恢复映射场并更新);
    //   - 无映射 → fresh 开场,同样记映射;
    //   - 不 Close(渠道会话跨轮持有;超帽淘汰/进程退出才封口)。
    struct ChannelTurnRequest {
        std::string session_key;         // 渠道会话键(路由定,含账号/会话)
        std::string stored_session_id;   // 映射账里的场(空 = 无映射)
        std::string prompt;
        HeadlessWorkBinding binding;     // work_id = 渠道域幂等键(client_operation_id)
        // 本轮冻结的工具策略(执行前重验准入后的五层交集账);空 = 回落
        // 会话级 options_.tools(与渠道路旧引擎同款语义)。
        const channel::ToolRoutePolicy* per_turn_tools = nullptr;
        // 选择事实的投递目标串(进 reply.selection.committed 的
        // deliveryTarget;空 = local:file)。渠道路递
        // "channel:<ch>:<acct>:<conv>"——与 outbox 的 MakeDeliveryId 同源。
        std::string delivery_target;
        // 绑定事实的渠道审计载荷(进 gateway.work.bound 的 "channel" 键;
        // 空 = 不带)。键值由泵冻结(ingressSid/sessionKey/conversationId/
        // senderId),恢复反查用。
        nlohmann::json binding_extra;
    };
    struct ChannelTurnResult {
        bool ok = false;
        std::string error_code;  // 同 Result 的 gateway.* 族
        std::string error;
        std::string session_id;   // 本轮实际用的场(fresh 或 resume 后)
        std::string turn_id;
        std::string reply_text;
        std::string selection_id;
        bool resumed = false;     // 走了 resume-as-new(新场 id ≠映射旧值)
    };
    ChannelTurnResult ExecuteChannelTurn(const ChannelTurnRequest& request,
                                         const std::function<void(const std::string& session_id,
                                                                  const std::string& turn_id)>& on_bound,
                                         const std::atomic<bool>* cancel);

    // 渠道活场观测(诊断/测试)。
    std::size_t live_channel_session_count() const;
    // 收口全部渠道活场(泵 Close 时调;失败只留账——场文件在,重启后经
    // 映射账 resume-as-new 续上下文)。
    void CloseChannelSessions(const std::string& reason);

private:
    // 共用核心:受理→绑定→hook→执行→收口→reply selection(不 Close——
    // 生命周期归调用方:automation 跑完 Close,渠道跨轮持有)。
    // agent_override 非空 = 用调用方缓存的引擎(渠道路:同场多轮共享
    // history,上下文不断);空 = 本轮现建(automation:fresh session)。
    Result RunTurnOnService(SessionService& service, agent::Agent* agent_override,
                            const std::string& prompt, const HeadlessWorkBinding& binding,
                            const char* purpose, const char* turn_actor,
                            const channel::ToolRoutePolicy* per_turn_tools,
                            const nlohmann::json& binding_extra,
                            const std::string& selection_delivery_target,
                            const std::function<void(const std::string&,
                                                     const std::string&)>& on_bound,
                            const std::atomic<bool>* cancel);
    // 渠道活场:session_key -> (常驻 SessionService + 常驻 Agent)。同场
    // 多轮共用同一只 Agent——上下文(history/前缀缓存)随场存活;超帽按
    // 最旧淘汰(Close 后由映射账续 resume-as-new,LaunchResumeHistory
    // 把源链折叠投影灌回新 Agent)。锁保护多泵线程下的开场合账。
    struct LiveChannelSession {
        std::unique_ptr<SessionService> service;
        std::unique_ptr<agent::Agent> agent;
    };
    LiveChannelSession* GetOrOpenChannelSession(const std::string& session_key,
                                                const std::string& stored_session_id,
                                                bool* resumed, std::string* error_code,
                                                std::string* error);

    api::Backend& backend_;
    tools::ToolRegistry& registry_;
    Options options_;
    mutable std::mutex channel_sessions_mutex_;
    // 插入序即使用序(取用时挪尾做 LRU)。
    std::vector<std::pair<std::string, std::unique_ptr<LiveChannelSession>>> channel_sessions_;
};

}  // namespace lubancode::runtime
