// AgentTaskCoordinator(子代理递归派工与结构化任务交接单·P0-3):会话级、
// 线程安全的派工协调器。从前主工具表与各转发表共享同一只 AgentTool 的
// 可变内部(hooks/线程表/台账/工厂)——后台线程直调它,寿命、锁、UI 回调
// 与结果归属搅在一处(单子 §一的病灶)。现在共享状态只在这里:
//
//   AgentDispatchTool(薄壳) -> AgentDispatchHandle(带 caller identity)
//     -> AgentTaskCoordinator(唯一派工规则:admission 由 TaskLedger 的
//        注册事务执法;lineage/closing/线程表归这里)
//     -> 引擎回调(装回 AgentTool:子代理装配/RunTask 仍是它的活)
//
// 单一 writer/owner:P0-3 起 AgentTool 不再自带 ledger/scheduler/线程表,
// 它构造这里、挂引擎、门面转发。每步迁移都保持只有一个所有者。
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent_profile_resolver.hpp"  // AgentProfileResolveEnvironment:冻结解析账
#include "api/backend.hpp"                   // Backend:detached 材料的 client 形状
#include "api/types.hpp"                     // RequestProfile
#include "hooks/dispatcher.hpp"              // HookDispatcher:进程级稳定指针
#include "tools/registry.hpp"
#include "tools/subagent_scheduler.hpp"
#include "tools/task_ledger.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

class AgentTool;
class AgentTaskCoordinator;
class AgentDispatchHandle;

// 后台子代理不能借主回合那条 Backend:主回合会重画 spinner,切 provider
// 时还会替换内部 client。工厂在 launch 当口造一份独立快照,线程随后只
// 握自己的 client、请求档案和提示词叠加层。它跑的仍是 agent::Agent;
// 此结构只管跨线程所需的不可变材料,不是另一种 Agent。(自 agent_tool.hpp
// 迁来:协调器的冻结环境要用它的形状,放两处会漂。)
struct DetachedAgentBackend {
    std::unique_ptr<api::Backend> backend;
    std::string provider;
    api::RequestProfile request_profile;
    std::string model_instructions;
    std::string soul;
};

// 后台子代理的放行账快照(修"后台审批不查放行账",2026-08):主会话的
// "总是允许"账(settings.local.json 的 allow_tools + 会话内按 a 落的集合)
// 与 allow/deny 命令前缀,由装配层经 SetBackgroundPermissionSource 递进来。
// 定格成"派出时刻用户已知的账",任务线程只读;嵌套孩子原样继承——后台
// 的免问只有预放行一条路,且只能吃派出时已有的账(单子不变量 7)。
struct BackgroundPermissionLedger {
    std::set<std::string> always_allowed;      // 工具名账(allow_tools + 按 a 落的)
    std::vector<std::string> allow_commands;   // run_command 前缀白名单
    std::vector<std::string> deny_commands;    // run_command 前缀黑名单(压过 allow)
};

// 运行身份(单子 §4.3):谁在派、在哪一层。task_id=0 即 main。depth 只由
// 宿主算(注册事务里对账父记录),模型不得自报。
struct AgentRunIdentity {
    int task_id = 0;       // 0 = main
    int root_task_id = 0;  // main 根为 0
    int depth = 0;         // main=0,子=1,孙=2
    std::string agent_run_id;  // 轨迹开启时填(子账 run id;空 = 没开)

    bool IsMain() const { return task_id == 0; }
};

// 派工环境快照(单子 §4.3 的落地):后台父任务派孩子时,孩子只吃这份冻结
// 材料,不回头读主会话正在变的 model/think/hooks/放行账。三个来源:
//   1. AgentTool 在 main 直派时按自家活账现填(执行线程 = main,读活账安全);
//   2. 每只任务开跑时(RunTask)为自己的孩子派生一份:继承冻结材料,挂上
//      自己的 base_registry/detached_shared;
//   3. 嵌套孩子原样继承(沿树只能相等或收窄,单子不变量 6/7)。
// env 里的工厂闭包拷的是值,不捕会话活引用;在父任务线程上调用。
struct SubagentDispatchEnv {
    // 嵌套后台孩子的后端工厂:每只孩子一份独立 client,值在父任务派出当口
    // 定格(会话侧 BuildFrozenBackendSpawner)。
    std::function<DetachedAgentBackend()> backend_factory;
    // 前台孩子与父共用的 detached 材料(父阻塞等它,无并发;寿命由
    // "父收场前等 attached child"不变量保住)。
    std::shared_ptr<DetachedAgentBackend> detached_shared;
    // 后台孩子的新表工厂(BuildDetachedRegistry 一类;函数对象拷贝)。
    std::function<std::unique_ptr<ToolRegistry>()> registry_factory;
    // 父任务自己的生效表:前台孩子的基表(建私有 todo + scoped agent)。
    ToolRegistry* base_registry = nullptr;
    // 放行账:根任务派出时定格,嵌套孩子原样继承(后台的免问只有预放行
    // 一条路,且只能吃派出时已有的账)。
    std::shared_ptr<const BackgroundPermissionLedger> background_permissions;
    // 进程级 hooks dispatcher(稳定指针);后台孩子自起只读快照会话。
    lubancode::hooks::HookDispatcher* hook_dispatcher = nullptr;
    // 冻结的解析环境:嵌套孩子的自定义 Agent 解析用父任务派出时刻的账
    //(权限档/技能/MCP 名单),不读会话当下的活档。
    std::optional<agent::AgentProfileResolveEnvironment> resolve_environment;
    // 父任务此刻是否在隔离房里(嵌套 worktree 首版稳定拒绝,单子 §11.3)。
    bool parent_in_isolation = false;
    // 真 = 无 UI 的派工(后台父任务的孩子们):确认一律走放行账,不弹终端。
    bool headless = false;
};

// 协调器 -> 引擎的 typed 派工请求:引擎(AgentTool)不再从一团 JSON 里一路
// input.at(),身份与环境一次递清。
struct AgentDispatchRequest {
    nlohmann::json input;      // 原始工具入参(校验/连败账在引擎侧)
    AgentRunIdentity caller;   // 派工者身份(经 TLS 校准,见下)
    std::shared_ptr<const SubagentDispatchEnv> env;  // null = main 直派,引擎读自家活账
    AgentDispatchHandle* fail_account = nullptr;     // 连败账随调用方的 handle 走
};

// ---- 当前派工身份的线程局部账 -------------------------------------------
// 前台任务在调用者线程上同步跑,后台任务在自己的线程上跑——"现在这条线程
// 正在执行哪只任务"是线程属性。handle 递进来的身份可能与线程不符(旧转发
// 壳、直捕的表),DispatchIdentity 以 TLS 为准:嵌套深度沿真实执行链走,
// 不被"谁的工具表"骗。RunTask 进出配对保存/恢复,天然支持前台嵌套。
const AgentRunIdentity* CurrentDispatchIdentity();

// TLS 身份的 RAII 守卫:RunTask 开跑置入、收场还原(还原成外层任务的
// 身份,前台嵌套链因此不串层)。
class ScopedDispatchIdentity {
public:
    ScopedDispatchIdentity(const AgentRunIdentity& identity);
    ~ScopedDispatchIdentity();
    ScopedDispatchIdentity(const ScopedDispatchIdentity&) = delete;
    ScopedDispatchIdentity& operator=(const ScopedDispatchIdentity&) = delete;

private:
    const AgentRunIdentity* saved_ = nullptr;
    AgentRunIdentity storage_;
};

class AgentTaskCoordinator {
public:
    using Engine = std::function<Tool::Result(const AgentDispatchRequest&)>;

    TaskLedger& ledger() { return ledger_; }
    const TaskLedger& ledger() const { return ledger_; }

    // 派工治理(原 SetDispatchGovernance 的账,这里唯一一份)。
    void SetGovernance(SubagentGovernance governance) { governance_ = governance; }
    const SubagentGovernance& governance() const { return governance_; }

    // 引擎回调:AgentTool 装配时挂自己(裸指针捕获安全——协调器是它的
    // 成员,先亡;handle 只持 weak_ptr,引擎亡后派工口稳定报 session_closed)。
    void SetEngine(Engine engine) { engine_ = std::move(engine); }

    // 门面工具指针(AgentTool 自己):薄壳的 name/description/input_schema
    // 只读转发用——schema 的动态内容(agent 类型清单)与主路同源。execute
    // 不走它,派工规则全在协调器。寿命:AgentTool 与各表同注册表共存亡,
    // 嵌套表的壳活不到协调器之后。
    void SetFacadeTool(Tool* tool) { facade_tool_ = tool; }
    Tool* facade_tool() const { return facade_tool_; }

    // 会话收场(单子 §6.2):进 Closing 后任何 handle 再派工都回稳定
    // session_closing,不新起线程。取消/收柄由 JoinAllBounded 办。
    void RequestClose() { closing_.store(true, std::memory_order_release); }
    bool closing() const { return closing_.load(std::memory_order_acquire); }

    // handle 的派工口(closing 在这判,admission 在台账注册事务里判)。
    Tool::Result Dispatch(const AgentDispatchRequest& request);

    // ---- 后台线程表(单一 writer:协调器)--------------------------------
    // 已收尾的线程在此收柄(join);析构兜底有界 join,detach 绝不冻退出。
    void TrackThread(int task_id, std::thread thread);
    void ReapSettledThreads();

    // 退出兜底:广播取消 -> 逐线程有界 join(旧 ~AgentTool 的规矩,原样
    // 迁来)——挂死绝境 detach 放行,台账已是终态,不丢账。
    void JoinAllBounded();

private:
    TaskLedger ledger_;
    SubagentGovernance governance_{};
    Engine engine_;
    Tool* facade_tool_ = nullptr;
    std::atomic<bool> closing_{false};
    std::mutex threads_mutex_;
    struct TaskThreadEntry {
        int task_id = 0;
        std::thread thread;
    };
    std::vector<TaskThreadEntry> threads_;
};

// 绑定 caller identity 的窄句柄(单子 §6.1/§6.4):工具表里挂的是它,不是
// AgentTool&。内部只有协调器 weak_ptr + 身份 + 冻结环境;执行时锁出短命
// 强引用,协调器已退场便回 session_closed。私有 registry 不反握强引用,
// 不与 coordinator/task record 绕引用环。
class AgentDispatchHandle {
public:
    AgentDispatchHandle() = default;
    AgentDispatchHandle(std::weak_ptr<AgentTaskCoordinator> coordinator, AgentRunIdentity identity,
                        std::shared_ptr<const SubagentDispatchEnv> env);

    const AgentRunIdentity& identity() const { return identity_; }
    // 派工入口:身份先经 TLS 校准(见 CurrentDispatchIdentity),再进协调器。
    Tool::Result Dispatch(const nlohmann::json& input);
    // 薄壳的 schema/description 只读转发口(协调器亡 = null,壳退静态文案)。
    Tool* facade_tool() const;

    // 参数连败账(单子缺 title 无限重试单):随 handle 走——一只 handle 只
    // 被一只 agent loop 在一条线程上用,不设锁;main 那只常驻 AgentTool。
    const std::string& param_fail_cause() const { return param_fail_cause_; }
    void set_param_fail_cause(std::string cause) { param_fail_cause_ = std::move(cause); }
    int param_fail_streak() const { return param_fail_streak_; }
    void set_param_fail_streak(int streak) { param_fail_streak_ = streak; }

private:
    std::weak_ptr<AgentTaskCoordinator> coordinator_;
    AgentRunIdentity identity_;
    std::shared_ptr<const SubagentDispatchEnv> env_;
    std::string param_fail_cause_;
    int param_fail_streak_ = 0;
};

// 身份小件:任务快照 -> 运行身份(台账是唯一真账,handle 的身份从这里投影)。
AgentRunIdentity IdentityOfSnapshot(const AgentTaskSnapshot& snapshot);

}  // namespace lubancode::tools
