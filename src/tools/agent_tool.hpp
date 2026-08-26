// 内置 "agent" 工具:把一个独立子任务委托给子代理执行。子代理是一个全新
// 的、空历史的 agent::Agent,只有它自己知道任务细节——主对话历史里
// 只留一次工具调用的入参(prompt)和一段 Result.content(子代理的最终
// 结论),中间的搜索/试错/来回工具调用过程都不会挤占主对话的上下文,
// 这是这个工具存在的全部意义。
//
// 子代理是独立任务 agent:默认与 main 同能力(同一份 runtime profile、
// 同一 provider 能力、同工具面——含再派 agent 的资格),接一项任务,
// 通常完成后自动退出。递归失控不靠"子表拿掉 agent 工具"防:子表挂的是
// AgentDispatchTool 转发壳,真闸是全局并发槽与显式深度上限
// (SetDispatchGovernance,subagent.max_active / subagent.max_depth)。
//
// 骨架拆解批三(病十四·六职拆分)后这只类只剩工具门面:execute 的入参
// 校验与前后台分岔、Hooks 转发合同、每任务装配(RunTask)。五职外迁:
//   TaskLedger(task_ledger.hpp)——统一台账:快照/事件账/inbox/取消/通知;
//   SubagentScheduler(subagent_scheduler.hpp)——并发槽与深度 RAII;
//   IsolationRooms(subagent_isolation.hpp)——worktree 房务与包装表;
//   TurnHarness(agent/turn_harness.hpp)——续投外环/取消链/收场分型/Stop
//   续跑环(主回合同一份,见 turn_runner);
//   TraceBackend(本 cpp)——子代理请求的活度账与诊断日志包装。
//
// 回调贯通:子代理执行期间的确认请求(needs_confirm 的工具)、usage 记账、
// "子代理调了个工具"这件事本身,都要能让上层看到、按同一套确认模式处理
// ——做法是 Hooks 结构体:宿主在每一轮 RunTurn 开始时把这一轮的
// on_tool_confirm/on_usage 直接转发过来、外加一个"子代理调了工具"的打印
// 回调,通过 SetHooks 灌进来;execute() 每次跑子代理时都用当前这份 Hooks
// 转发,不设(默认构造的空 Hooks)也不会崩,只是不转发/不打印。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "agent/loop.hpp"  // ToolHookDecision/ToolPhase/Callbacks:hooks 与事件流转发的类型
#include "agent/runtime_profile.hpp"
#include "api/types.hpp"
#include "cli/worktree.hpp"
#include "hooks/detached.hpp"
#include "hooks/dispatcher.hpp"
#include "tools/isolation.hpp"
#include "tools/registry.hpp"
#include "tools/subagent_scheduler.hpp"
#include "tools/task_ledger.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

// 后台子代理不能借主回合那条 Backend：主回合会重画 spinner，切 provider
// 时还会替换内部 client。工厂在 launch 当口造一份独立快照，线程随后只
// 握自己的 client、请求档案和提示词叠加层。它跑的仍是 agent::Agent；
// 此结构只管跨线程所需的不可变材料，不是另一种 Agent。
struct DetachedAgentBackend {
    std::unique_ptr<api::Backend> backend;
    std::string provider;
    api::RequestProfile request_profile;
    std::string model_instructions;
    std::string soul;
};

// 同级派工的转发壳(规格"递归派工不能再靠拿掉工具解决"):子代理工具表
// 里的 "agent" 工具,目标就是主 AgentTool 实例——子代理默认与 main 同
// 能力,能再拆任务。递归失控不靠"子表没有 agent"防,改由全局并发槽 +
// 显式深度上限治理。后台(detached)注册表不挂这枚壳:后台线程不能同步
// 跑前台任务(UI 回调线程模型不允许)。
class AgentTool;

class AgentDispatchTool : public Tool {
public:
    explicit AgentDispatchTool(AgentTool& target) : target_(target) {}

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }
    Result execute(const nlohmann::json& input) override;

private:
    AgentTool& target_;
};

class AgentTool : public Tool {
public:
    struct Hooks {
        // 子代理内部工具 needs_confirm() 为真时,原样转发给父级
        // on_tool_confirm——三档确认模式(yolo/auto/confirm)在父级那份
        // 回调里已经处理好了,这里不用重复实现。
        std::function<bool(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input)> on_tool_confirm;

        // 子代理发起了一次工具调用(还没执行),给上层打一行提示用。跟
        // on_tool_confirm 分开是因为这个纯粹用于展示,没有返回值、不影响
        // 子代理是否真的执行。
        std::function<void(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input)> on_sub_tool_start;

        // 子代理每次独立请求结束的 usage(连同步号/请求 id 身份),原样
        // 转发给父级 on_usage——累计进本轮 token 统计与逐步流水账,请求
        // 次数也算进去。
        std::function<void(const api::UsageReport& report)> on_usage;

        // M9:子代理内部的工具调用也要受 pre_tool/post_tool 钩子管——原样
        // 转发给父级的同名回调,子代理这边不重复实现匹配/执行逻辑。
        std::function<std::optional<std::string>(const std::string& tool_use_id, const std::string& name,
                                              const nlohmann::json& input)>
            on_pre_tool_hook;
        std::function<void(const std::string& tool_use_id, const std::string& name, const nlohmann::json& input,
                          const Tool::Result& result)>
            on_post_tool_hook;

        // hooks 框架第三步:新回调同样原样转发(完整 PreToolUse 表态、
        // PermissionRequest、UI 相位、PostToolUse 反馈)。注意 on_pre_tool_
        // use_hook 捕获的"预决策槽"在父级闭包里,转发的是同一批 std::function,
        // 槽随行——子代理的确认回调读到的就是子代理当前那次工具调用的决策。
        std::function<agent::ToolHookDecision(const std::string& tool_use_id, const std::string& name,
                                              const nlohmann::json& input)>
            on_pre_tool_use_hook;
        std::function<agent::ToolHookDecision(const std::string& tool_use_id, const std::string& name,
                                              const nlohmann::json& input)>
            on_permission_request;
        std::function<void(const std::string& tool_use_id, const std::string& name, agent::ToolPhase phase)>
            on_tool_phase;
        std::function<std::vector<std::string>(const std::string& tool_use_id, const std::string& name,
                                               const nlohmann::json& input, const Tool::Result& result)>
            on_post_tool_use_hook;

        // 逐枚追踪单:子代理内部工具的 canonical 事件出口。转发给子代理
        // AgentLoop 的 on_tool_trace;宿主侧的实现经 ToolTraceHub 的只读
        // 并轨口投递。事件的 parent_execution_id 由调用方在 AgentLoop 那侧
        // 填,这里只透传。不设 = 子代理不追踪(旧行为)。
        std::function<void(const agent::ToolTraceEvent&)> on_tool_trace;

        // 发起这只子代理的那枚 agent 工具调用的 execution_id(parent
        // 关系边的另一端)。装配在这里、值在 agent 工具真正开跑时才钉得
        // 下,所以是取值函数而非快照;空函数 = 没有这条边(旧调用方),
        // 子代理事件照发,parent 留空。
        std::function<std::string()> parent_execution_id_getter;

        // ESC/Ctrl+C 打断信号(宿主那份 cancel_flag 的地址)——子代理
        // 内部的 AgentLoop::Run() 原样收这根指针,工具循环里就能跟顶层同一套
        // "cancel != nullptr && cancel->load()"判断打断,不用重新实现打断
        // 语义。不设(默认 nullptr)= 子代理收不到外部打断,行为跟从前一样。
        const std::atomic<bool>* cancel = nullptr;

        // Plan 模式(只读研究硬闸单):子代理内部工具的 ModePolicy 闸。
        // 转发父级同名回调——子代理不因独立 context 逃闸。不设 = 子代理
        // 不过 Plan 闸(旧路)。
        std::function<std::string(const std::string& tool_name, const nlohmann::json& input)> on_mode_policy;

        // 批二尾巴(骨架拆解批三):宿主侧装配好的事件流显示回调。子代理
        // 自己的 sub_callbacks(正文/思考/工具起止/usage)先过这份事件流
        // (canonical 账),再落台账/终端——与主回合"一份事件、两路消费"
        // 同一格局。编译边界:tools/ 住 engine 库不许 include runtime,
        // TurnEventAdapter 的翻译在宿主(app)侧做完,这里只收
        // agent::Callbacks 这份中立形状。不设 = 这只子代理不上事件流
        // (后台任务没有宿主轮、旧调用方),行为与从前一致。
        const agent::Callbacks* event_callbacks = nullptr;

        // hooks 框架第四五步:进程级 dispatcher。前台子代理用它发
        // SubagentStart/SubagentStop,并把 dispatcher 的上下文换成这只子代理
        // 的 agent_id/agent_type(跑完还原)。后台子代理不接 hooks(线程模型
        // 见 dispatcher 注释),这里是空指针。
        lubancode::hooks::HookDispatcher* hook_dispatcher = nullptr;
    };

    // backend:子代理发请求用的后端。宿主传进来的通常是跟主循环共用
    // 的那条包装链(模型覆盖/推理强度覆盖/转轮),子代理天然继承同一套。
    // sub_registry:子代理能用的工具注册表,调用方保证其中不含 "agent" 自己
    // (防递归,深度硬限 1)。
    // cwd:子代理系统提示词里的工作目录段,跟主代理一致。
    // model:子代理发请求用的 model 字段;如果 backend 链里有会覆盖 model
    // 的包装层,这里填什么都会被覆盖掉,留空也没关系。
    // default_max_steps_per_turn:入参没给 max_steps_per_turn 时用的默认值。
    // 调用方必须从配置传入(首选 subagent.max_steps_per_turn,未设继承
    // config.max_steps_per_turn;默认 0 = 不限步)——这里不再暗藏魔数。
    // skills_segment:M9,系统提示词里"可用技能"那一段,子代理跟主代理
    // 共用同一份扫描结果,空串表示没有技能(不注入)。
    AgentTool(api::Backend& backend, ToolRegistry& sub_registry, std::string cwd, std::string model = std::string(),
              int default_max_steps_per_turn = 0, std::string skills_segment = std::string());

    ~AgentTool() override;

    void SetHooks(Hooks hooks) { hooks_ = std::move(hooks); }

    // Explore 是内置只读代理。调用方另建一张只读工具表塞进来；不设时
    // Explore 仍能启动，只是沿用普通子表再由过滤器挡掉写入工具。
    void SetExploreRegistry(ToolRegistry* registry) { explore_registry_ = registry; }

    // 交互会话开、单发/单测关。入参显式给 run_in_background 时压过它。
    void SetBackgroundByDefault(bool enabled) { background_by_default_ = enabled; }
    void SetDetachedBackendFactory(std::function<DetachedAgentBackend()> factory) {
        detached_backend_factory_ = std::move(factory);
    }
    void SetDetachedRegistryFactory(std::function<std::unique_ptr<ToolRegistry>()> factory) {
        detached_registry_factory_ = std::move(factory);
    }

    // ---- 台账口(门面转发,本体在 TaskLedger)----
    std::vector<AgentTaskSnapshot> TaskSnapshots(std::size_t max_entries = 0) const {
        return ledger_.Snapshots(max_entries);
    }
    std::uint64_t TaskRevision() const { return ledger_.revision(); }
    std::string DrainCompletionNotices() { return ledger_.DrainCompletionNotices(); }
    bool HasRunningTasks() const { return ledger_.HasRunningTasks(); }
    std::vector<AgentTaskSummary> TaskSummaries() const { return ledger_.Summaries(); }
    std::optional<AgentTaskSnapshot> TaskDetail(int task_id) const { return ledger_.Detail(task_id); }
    std::vector<AgentTaskEvent> TaskEvents(int task_id) const { return ledger_.Events(task_id); }
    std::vector<std::string> PendingTaskMessages(int task_id) const { return ledger_.PendingMessages(task_id); }
    TaskMessageStatus SendTaskMessage(int task_id, const std::string& text,
                                      TaskMessageSource source = TaskMessageSource::User) {
        return ledger_.SendMessage(task_id, text, source);
    }
    std::string RunningTasksRoster() const { return ledger_.RunningTasksRoster(); }
    bool CancelTask(int task_id) { return ledger_.CancelTask(task_id); }
    int CancelAllTasks() { return ledger_.CancelAllTasks(); }
    bool ClearFinishedTask(int task_id) { return ledger_.ClearFinishedTask(task_id); }
    std::vector<std::string> TakeUndeliveredInboxReport() { return ledger_.TakeUndeliveredInboxReport(); }
    bool HasUndeliveredCompletions() const { return ledger_.HasUndeliveredCompletions(); }
    std::vector<std::string> CompletionNoticeLines() const { return ledger_.CompletionNoticeLines(); }
    std::vector<int> UndeliveredCompletionTaskIds() const { return ledger_.UndeliveredCompletionTaskIds(); }
    std::vector<std::string> TakePermissionDenialNotices() { return ledger_.TakePermissionDenialNotices(); }

    // 主会话切进 /worktree 后，子代理也得看见同一处工作目录。
    void SetWorkingDirectory(std::string cwd) { cwd_ = std::move(cwd); }

    // 子代理的上下文窗口(token):mid-turn 压力评估与自动 compact 用。
    // 0 = 未知,不评估——行为与从前一致。
    void SetContextWindowTokens(std::size_t tokens) { context_window_tokens_ = tokens; }

    // 运行策略(规格根因一):会话重建时把 main 的有效 profile 派生份
    // 灌进来——输出上限、上下文安全网、续跑次数与 main 同一份。
    void SetRuntimeProfile(agent::AgentRuntimeProfile profile) {
        runtime_profile_ = profile;
        agent_profile_.runtime = std::move(profile);
    }
    // 皮(骨架拆解批四起的正门):provider/request/runtime/system_prompt 与
    // 四段开关(病十)全在 AgentProfile 上,子代理默认与主代理同段。
    void SetAgentProfile(agent::AgentProfile profile) {
        agent_profile_ = std::move(profile);
        runtime_profile_ = agent_profile_.runtime;
        if (!agent_profile_.request.model.empty()) {
            model_ = agent_profile_.request.model;
        }
    }

    // 派工治理(转发 SubagentScheduler;max_active/max_depth 语义见那头)。
    void SetDispatchGovernance(int max_active, int max_depth) {
        scheduler_.SetGovernance(max_active, max_depth);
    }

    // 墙钟兜底(规格"detached 超时链路核查与兜底"):一只任务整轮的墙钟
    // 上限,秒。到点先走正常取消链(合并 cancel);任务线程 grace_secs 内
    // 还没报终态,由看门狗直接把台账翻成 Failed/WallClockTimeout。0 = 不限。
    void SetWallClockTimeout(int secs, int grace_secs = kDefaultSubagentWallClockGraceSecs) {
        wall_clock_timeout_secs_ = secs > 0 ? secs : 0;
        wall_clock_grace_secs_ = grace_secs > 0 ? grace_secs : 1;
    }

    // 子代理的项目记忆召回:按子任务 prompt 独立检索,同预算同安全声明。
    // 由会话层灌(闭包着 ProjectMemory),不设 = 子代理不召回(旧行为)。
    void SetTurnContextProvider(std::function<std::string(const std::string&)> provider) {
        turn_context_provider_ = std::move(provider);
    }

    // tool_search(延迟挂载):子代理注册表同机制。filter 原样灌给每次
    // execute() 新建的 sub_loop;index_provider 每次 execute() 现算"延迟
    // 未加载"索引段,拼进子代理系统提示末尾。两个都不设(默认)= 子代理
    // 不启用延迟,行为跟从前完全一样。
    void SetToolFilter(std::function<bool(const Tool&)> filter) { tool_filter_ = std::move(filter); }
    void SetDeferredIndexProvider(std::function<std::string()> provider) {
        deferred_index_provider_ = std::move(provider);
    }

    // 提示词运行时化(0.21.x):用户模块目录(~/.lubancode/prompts)。设了
    // 之后每次 execute() 新建子代理时,系统提示的 features 模块同走"用户
    // 文件优先、嵌入回退"。不设(默认空)= 只用嵌入版。
    void SetPromptsDir(std::string prompts_dir) { prompts_dir_ = std::move(prompts_dir); }

    // isolation=worktree 的房务 Git 调用可替身(测试注入假 runner);
    // 不设走真 git。
    void SetGitRunner(lubancode::cli::GitRunner runner) { git_runner_ = std::move(runner); }

    void SetSkillsSegment(std::string skills_segment) { skills_segment_ = std::move(skills_segment); }
    void SetProjectInstructions(std::string instructions) { project_instructions_ = std::move(instructions); }
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }  // 子代理内部的危险工具各自有确认关
    Result execute(const nlohmann::json& input) override;

private:
    // background_hooks:后台任务的只读 hooks 会话(LaunchBackground 在主线程
    // 造好带进来;前台路径为空)。RunTask 拿它在后台线程发 SubagentStart/Stop
    // 与工具事件——记录只投递,主会话安全点归并。
    Result RunTask(api::Backend& backend, ToolRegistry& task_registry, const std::string& prompt,
                   const std::string& agent_type, int max_steps_per_turn, const Hooks* foreground_hooks,
                   const std::shared_ptr<TaskRecord>& task,
                   const DetachedAgentBackend* detached = nullptr,
                   const std::string* prepared_system_prompt = nullptr,
                   const IsolationScope* isolation_scope = nullptr,
                   const std::shared_ptr<lubancode::hooks::DetachedHookSession>& background_hooks = nullptr);

    Result ExecuteForeground(const nlohmann::json& input, const std::string& title, const std::string& agent_type,
                             ToolRegistry& task_registry, int max_steps_per_turn, bool isolate);
    Result LaunchBackground(const nlohmann::json& input, const std::string& title, const std::string& agent_type,
                            ToolRegistry& task_registry, int max_steps_per_turn, bool isolate);

    // 子代理请求的包装后端(agent_tool.cpp 内实现):一次不落地把"请求发出/
    // 首事件/逐事件/收场错误"记进活度账与诊断日志(LUBANCODE_DEBUG_SUBAGENT)。
    class TraceBackend;

    api::Backend& backend_;
    ToolRegistry& sub_registry_;
    ToolRegistry* explore_registry_ = nullptr;
    std::string cwd_;
    std::string model_;
    int default_max_steps_per_turn_;
    std::string skills_segment_;
    std::string prompts_dir_;  // 提示词运行时化:空 = 只用嵌入版
    std::string project_instructions_;  // 当前工作目录的 AGENTS.md 分层内容
    Hooks hooks_;
    bool background_by_default_ = false;
    std::function<DetachedAgentBackend()> detached_backend_factory_;
    std::function<std::unique_ptr<ToolRegistry>()> detached_registry_factory_;
    // 六职拆分(批三):台账与调度各归各家,门面只转不发。
    TaskLedger ledger_;
    SubagentScheduler scheduler_;
    // 后台任务线程表:已收尾的 join 回收(LaunchBackground 顺带),析构
    // 再兜底(有界 join,detach 绝不冻退出——见析构注释)。
    std::vector<std::thread> task_threads_;
    std::function<bool(const Tool&)> tool_filter_;            // tool_search:空 = 不过滤
    std::function<std::string()> deferred_index_provider_;    // tool_search:空 = 不注索引段
    lubancode::cli::GitRunner git_runner_;                    // isolation 房务;空 = 真 git
    std::size_t context_window_tokens_ = 0;                   // 子代理 mid-turn 压缩评估;0 = 未知
    agent::AgentRuntimeProfile runtime_profile_;              // 运行策略:与 main 同一份(默认 unset)
    agent::AgentProfile agent_profile_;                       // main/sub/workflow 共用的代理属性形状
    // 墙钟兜底:整轮上限与收杀宽限(秒;0 = 不限)。
    int wall_clock_timeout_secs_ = 0;
    int wall_clock_grace_secs_ = kDefaultSubagentWallClockGraceSecs;
    std::function<std::string(const std::string&)> turn_context_provider_;  // 子代理记忆召回;空 = 不召回
};

}  // namespace lubancode::tools
