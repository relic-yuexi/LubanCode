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
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "agent/agent.hpp"  // Agent/AgentProfile/Callbacks:子代理的引擎与皮
#include "agent/agent_definition.hpp"  // AgentDefinition:自定义 Agent 的解析结果(P2-2)
#include "agent/agent_profile_resolver.hpp"  // ResolvedAgentProfile:阶段 3 统一解析
#include "agent/loop.hpp"      // RunOutcome/RunOneTool:轮次收口与工具执行链
#include "agent/runtime_profile.hpp"
#include "api/types.hpp"
#include "agent/prompt_assembler.hpp"  // PackageProfileRoot:包层 Profile 根(阶段 3)
#include "agent/task_spec.hpp"  // AgentTaskSpec:扁平 title + instructions 合同
#include "runtime/trajectory_session.hpp"  // TrajectorySubagentBridge:P0-2 子代理独立 JSONL
#include "runtime/worktree.hpp"
#include "config/project_instructions.hpp"  // ProjectInstructionResolver:AGENTS.md 作用域(作用域单 P0)
#include "hooks/detached.hpp"
#include "hooks/dispatcher.hpp"
#include "tools/agent_task_coordinator.hpp"  // P0-3:协调器/scoped handle/冻结环境
#include "tools/isolation.hpp"
#include "tools/registry.hpp"
#include "tools/subagent_scheduler.hpp"
#include "tools/task_ledger.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

// 自定义 Agent 的派发材料(真机实测 P2-1/P2-2):宿主(会话装配层)从
// AgentCatalog 按名解析一份定义交进来。preloaded_skills 与 definition.
// skills_preload 按位对齐——预装技能的正文(SKILL.md frontmatter 之后的
// body)在宿主侧读好带进来,tools 层不认得技能目录的扫描规矩,也不该认。
//
// builtin(阶段 4):这条材料是 Catalog 里码内注册的两枚内置定义时置真。
// agent_type 的派发校验已换净成"查得到即可派"(下面 SetCustomAgentResolver
// 的注释),内置名也照查 Catalog——查到的若正是码内那份,builtin 让它走
// 阶段 4 之前的内置快路(explore_registry、Explore 白名单、persona 查表),
// 行为一字不动;user/project 层显式覆盖内置名的(builtin=false)则按定义
// 走自定义路,契约 §6.2"跨层同名属于显式覆盖"由此真正生效。
struct CustomAgentMaterial {
    agent::AgentDefinition definition;
    std::vector<std::string> preloaded_skills;
    bool builtin = false;  // 码内内置定义(general-purpose/Explore)的记号
};

// 动态 schema 的一行(阶段 4·单子 §6.3):名字 + 一句描述,从 AgentCatalog
// 拉。agent 工具 input_schema 的 agent_type 说明据此列"当前可派的类型",
// 模型照单挑人。首版只放 name + description,不做 deferred catalog。
struct AgentTypeInfo {
    std::string name;
    std::string description;
};

// 一次派工的成本预算(真机实测 P2-6):三根硬线 + 软线百分比,全部 0(软
// 线除外) = 不设。解析次序:入参显式 > 自定义 Agent YAML 的 runtime 字段
// > 配置默认(subagent.max_steps_per_turn 一脉)。
struct SubagentBudget {
    int max_steps_per_turn = 0;
    // 任务总 turn(turn 预算单 P0-1/P0-3):整项任务从注册到终态最多准入
    // 几次逻辑模型请求——初始 Run、mailbox 续投、孩子回流、Stop 钩子续跑
    // 共这本账。不走模型可见的 JSON 入参(解析链在 Resolver:宿主
    // override > Agent Definition runtime.max_turns > subagent.default_max_
    // turns > 0)。0 = 不设任务总帽。与 max_steps_per_turn(兼容窗里仍是
    // "每个 input round"的旧义)分家,不混写。
    int max_turns = 0;
    int max_wall_secs = 0;
    std::int64_t max_total_tokens = 0;
    int soft_percent = agent::kDefaultBudgetSoftPercent;
};

// 同级派工的薄壳(P0-3 重写):子代理工具表里的 "agent" 工具不再是直指
// 主 AgentTool 的转发壳——那枚壳把活 hooks、线程表、台账与工厂全数暴露给
// 后台线程(单子 §一的病灶)。新壳只握一枚绑定 caller identity 的
// AgentDispatchHandle:协调器 weak_ptr + 身份 + 冻结环境,执行时锁出短命
// 强引用。后台(detached)注册表也挂它:前后台同一条派工路,资格由
// 工具面与 lineage 深度决定,不由"前台/后台"决定(单子不变量 1)。
class AgentTool;

class AgentDispatchTool : public Tool {
public:
    // 主路:各自身份的 handle(main/每只任务各一枚,RunTask 派生)。
    explicit AgentDispatchTool(AgentDispatchHandle handle) : handle_(std::move(handle)) {}
    // 兼容门面(旧装配/测试):从 AgentTool 拿它的 main handle——行为与
    // 旧转发壳等价(execute 直通,name/schema 同源),但真规则在协调器。
    explicit AgentDispatchTool(AgentTool& target);

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }
    Result execute(const nlohmann::json& input) override;
    Result execute(const nlohmann::json& input, const ToolExecutionContext& context) override;

private:
    AgentDispatchHandle handle_;
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

        // 权限收窄执法(阶段 4):宿主的"带下限的确认"口。自定义 Agent 的
        // permissions.mode 比父会话档严时(父 yolo 子 confirm),子代理
        // needs_confirm 的工具走这一口——宿主按 min(会话档, 下限) 裁定,
        // 该问就真把确认拉回来,yolo/auto 的免问不再免。空 = 宿主没接
        //(旧调用方/单测),退回 on_tool_confirm 原样转发,行为与从前一致。
        std::function<bool(const std::string& tool_use_id, const std::string& name,
                           const nlohmann::json& input, agent::AgentPermissionMode floor)>
            on_tool_confirm_floored;

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
        std::function<runtime::ToolHookDecision(const std::string& tool_use_id, const std::string& name,
                                              const nlohmann::json& input)>
            on_pre_tool_use_hook;
        std::function<runtime::ToolHookDecision(const std::string& tool_use_id, const std::string& name,
                                              const nlohmann::json& input)>
            on_permission_request;
        std::function<void(const std::string& tool_use_id, const std::string& name, runtime::ToolPhase phase)>
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

        // ---- P0-2/P1-2 轨迹接线(flag 开的会话;空 = 旧路,行为零变) ------
        // 子代理派工的轨迹账申请口:向会话账本要独立 JSONL(scoped
        // recorder + run.started,relations 带 parent 边)。返回空 = 子账
        // 没开张(开张失败/没接线),子代理照跑,父账如实缺子边。
        // parent_run_id(P1-2 嵌套轨迹边):派工者自己的 agent_run_id;main
        // 直派传空串(SpawnSubagent 内部按空串落回 main_run_id,旧行为不
        // 变)。嵌套 headless 路必须传父任务自己的 run id——它的父亲是
        // 派出它的那只子代理,不是 main(单子 §12.3 第一条)。
        std::function<std::unique_ptr<runtime::TrajectorySubagentBridge>(const std::string& task_label,
                                                                        const std::string& parent_run_id)>
            trajectory_spawn;
        // 子账收口(run terminal + 关柄)后的回填口:父桥记下子账终态
        // hash,父侧 agent 调用的执行终态事件引用它(§3.5 边界对账)。
        std::function<void(const std::string& run_id, const std::string& terminal_hash)>
            trajectory_child_finished;

        // ESC/Ctrl+C 打断信号(宿主那份 cancel_flag 的地址)——子代理
        // 内部的 AgentLoop::Run() 原样收这根指针,工具循环里就能跟顶层同一套
        // "cancel != nullptr && cancel->load()"判断打断,不用重新实现打断
        // 语义。不设(默认 nullptr)= 子代理收不到外部打断,行为跟从前一样。
        const std::atomic<bool>* cancel = nullptr;

        // MCP 富结果单 P0.5:工具二进制 artifact 目录随派工下发——子代理
        // 调 MCP 工具返回的图片/音频/blob 与主回合落同一份会话 artifact
        // 目录。空 = 父级没开(单测/没建档),富二进制块在子代理里同样按
        // 稳定错误收口。
        std::string tool_artifact_dir;

        // Plan 模式(只读研究硬闸单):子代理内部工具的 ModePolicy 闸。
        // 转发父级同名回调——子代理不因独立 context 逃闸。不设 = 子代理
        // 不过 Plan 闸(旧路)。
        std::function<std::string(const std::string& tool_name, const nlohmann::json& input)> on_mode_policy;

        // 批二余款(骨架拆解):宿主的显示出水口。给了就在 RunTask 里现起
        // 一只从路适配器(同一本发号局、同一枚 turn_id):子代理的正文/思考
        // 增量、工具起止、usage 先落台账 sink,再原样转发进宿主流(payload
        // 带 subordinate 标,画屏侧跳过——子代理只画外层卡)。编译边界:
        // tools/ 不直接 include runtime 头,指针形状经 agent/loop.hpp 过路。
        // 不设 = 这只子代理不上宿主流(后台任务没有宿主轮、旧调用方),
        // 台账照走(自己的本地适配器),行为与从前一致。
        runtime::TurnEventAdapter* events = nullptr;

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

    // 宿主每轮 RunTurn 开头都会重灌这份 Hooks(turn_runner 的 BuildTurnWiring)
    //——SetHooks 因此就是回合边界:参数连败账(见成员区注释)随灌随清,
    // 计数归回合,不跨回合记仇;agent 类型清单的 schema 缓存也在这里翻新
    //(用户改了 YAML,下一回合的 schema 就列得出新名字——派发口本来就
    // 现扫现查,两头不脱节太久;/agents 仍是权威清单)。
    void SetHooks(Hooks hooks) {
        hooks_ = std::move(hooks);
        // main handle 的连败账随回合清(任务 handle 的账随任务寿命,任务
        // 内本就只有一个回合)。
        main_handle_.set_param_fail_cause(std::string());
        main_handle_.set_param_fail_streak(0);
        {
            std::lock_guard<std::mutex> lock(agent_types_cache_.mutex);
            agent_types_cache_.loaded = false;
        }
    }

    // Explore 是内置只读代理。调用方另建一张只读工具表塞进来；不设时
    // Explore 仍能启动，只是沿用普通子表再由过滤器挡掉写入工具。
    void SetExploreRegistry(ToolRegistry* registry) { explore_registry_ = registry; }

    // 自定义 Agent 解析口(真机实测 P2-1/P2-2;阶段 4 起是 agent_type 的
    // 唯一派发校验):execute() 拿名字问这枚回调——宿主侧接的是 AgentCatalog
    // 按名查账。给到材料就派:码内内置两枚(builtin=true)走内置快路,行为
    // 与阶段 4 前一字不差;其余按自定义 Agent 派发——身份按 resolved name
    // 记(Dock/台账/日志不冒名)、tools.allow/deny 收窄工具面、skills.preload
    // 预装、runtime.max_steps_per_turn 落预算。查不到 = "没有这名,看
    // /agents",不再写死两枚内置名当白名单。空函数(默认,旧调用方/单测)
    // = 退回旧口径,只认内置两枚,行为与从前一致。
    void SetCustomAgentResolver(std::function<std::optional<CustomAgentMaterial>(const std::string&)> resolver) {
        custom_agent_resolver_ = std::move(resolver);
    }
    // 只读口(真机实测 P2-3):Plan 闸按 agent_type 判工具面时要用同一份
    // 解析器,不许装配层再养一份平行账。
    const std::function<std::optional<CustomAgentMaterial>(const std::string&)>& custom_agent_resolver() const {
        return custom_agent_resolver_;
    }

    // agent 类型清单源(阶段 4·动态 schema):宿主从 AgentCatalog 拉"当前
    // 可派的类型"(内置+自定义,各带一句 description)递进来。input_schema
    // 的 agent_type 说明据此列单;清单在回合边界(SetHooks)翻新、平时读
    // 缓存——input_schema 每次构造请求都要跑一遍,不许它拖着一回磁盘扫
    // 描(性能口径:单子 §6.3 照 skill 工具 enum 的同一先例——动态内容进
    // 缓存快照,不进每请求重算)。不设(默认空)= 说明不追加清单,schema
    // 与从前逐字节一致。
    void SetAgentTypesProvider(std::function<std::vector<AgentTypeInfo>()> provider) {
        agent_types_provider_ = std::move(provider);
        std::lock_guard<std::mutex> lock(agent_types_cache_.mutex);
        agent_types_cache_.loaded = false;
    }

    // 自定义 Agent 的解析环境(阶段 3:AgentProfileResolver 的父会话活材料
    // 账——权限档、技能/MCP 名单、角色路由、思考档表)。每笔派发现调,
    // 会话内会变的账(权限档)读到的总是当下值。空 = 宿主没接(旧调用方/
    // 单测),权限与依赖校验按"没账可查"跳过:不报错,也不放宽。
    void SetResolveEnvironment(std::function<agent::AgentProfileResolveEnvironment()> environment) {
        resolve_environment_ = std::move(environment);
    }
    // 只读口(阶段 5):Workflow 的 agent 节点接自定义 Agent 时复用同一份
    // 环境账——两路解析喂同一只供应商,权限/依赖查账才不各养一本。
    const std::function<agent::AgentProfileResolveEnvironment()>& resolve_environment_provider() const {
        return resolve_environment_;
    }

    // 交互会话开、单发/单测关。入参显式给 run_in_background 时压过它。
    void SetBackgroundByDefault(bool enabled) { background_by_default_ = enabled; }
    void SetDetachedBackendFactory(std::function<DetachedAgentBackend()> factory) {
        detached_backend_factory_ = std::move(factory);
    }
    void SetDetachedRegistryFactory(std::function<std::unique_ptr<ToolRegistry>()> factory) {
        detached_registry_factory_ = std::move(factory);
    }

    // 嵌套后台孩子的后端工厂源(P0-3"派出时冻结 execution snapshot"):
    // 每只后台任务起跑当口调一次,返回一份闭包拷值的冻结工厂——它造的
    // client 用的是父任务派出时刻的 model/think/指令/魂,任务树中途
    // /model 换档不影响在跑的树。没设(旧调用方/单测)= 嵌套后台派工回
    // 稳定错(那只树没有后台再派后台的资格),不影响 main 直派。
    void SetFrozenBackendSpawnerSource(
        std::function<std::function<DetachedAgentBackend()>()> source) {
        frozen_backend_spawner_source_ = std::move(source);
    }

    // 后台子代理的放行账源(修"后台审批不查放行账"):LaunchBackground 在
    // 主线程调它取一份快照(见 BackgroundPermissionLedger 的定格理由)。
    // 没设 = 空账,后台需确认工具照旧全拒(旧行为)。闭包按 Hooks 同一
    // 套寿命规矩捕获会话侧引用——控制器死后主循环不在,源不会再被调。
    void SetBackgroundPermissionSource(std::function<BackgroundPermissionLedger()> source) {
        background_permission_source_ = std::move(source);
    }

    // ---- 台账口(门面转发,本体在 TaskLedger;P0-3 起台账归协调器所有)----
    std::shared_ptr<AgentTaskCoordinator> coordinator() const { return coordinator_; }
    // main 身份的派工句柄:main 工具表挂的壳用它(参数连败账也随它走)。
    AgentDispatchHandle main_dispatch_handle() const { return main_handle_; }
    TaskLedger& ledger() { return coordinator_->ledger(); }
    const TaskLedger& ledger() const { return coordinator_->ledger(); }
    std::vector<AgentTaskSnapshot> TaskSnapshots(std::size_t max_entries = 0) const {
        return coordinator_->ledger().Snapshots(max_entries);
    }
    std::uint64_t TaskRevision() const { return coordinator_->ledger().revision(); }
    std::string DrainCompletionNotices() { return coordinator_->ledger().DrainCompletionNotices(); }
    bool HasRunningTasks() const { return coordinator_->ledger().HasRunningTasks(); }
    std::vector<AgentTaskSummary> TaskSummaries() const { return coordinator_->ledger().Summaries(); }
    std::optional<AgentTaskSnapshot> TaskDetail(int task_id) const {
        return coordinator_->ledger().Detail(task_id);
    }
    std::vector<AgentTaskEvent> TaskEvents(int task_id) const { return coordinator_->ledger().Events(task_id); }
    std::vector<std::string> PendingTaskMessages(int task_id) const {
        return coordinator_->ledger().PendingMessages(task_id);
    }
    TaskMessageStatus SendTaskMessage(int task_id, const std::string& text,
                                      TaskMessageSource source = TaskMessageSource::User) {
        return coordinator_->ledger().SendMessage(task_id, text, source);
    }
    std::string RunningTasksRoster() const { return coordinator_->ledger().RunningTasksRoster(); }
    bool CancelTask(int task_id) { return coordinator_->ledger().CancelTask(task_id); }
    int CancelAllTasks() { return coordinator_->ledger().CancelAllTasks(); }
    bool ClearFinishedTask(int task_id) { return coordinator_->ledger().ClearFinishedTask(task_id); }
    std::vector<std::string> TakeUndeliveredInboxReport() {
        return coordinator_->ledger().TakeUndeliveredInboxReport();
    }
    bool HasUndeliveredCompletions() const { return coordinator_->ledger().HasUndeliveredCompletions(); }
    std::vector<std::string> CompletionNoticeLines() const {
        return coordinator_->ledger().CompletionNoticeLines();
    }
    std::vector<int> UndeliveredCompletionTaskIds() const {
        return coordinator_->ledger().UndeliveredCompletionTaskIds();
    }
    std::vector<std::string> TakePermissionDenialNotices() {
        return coordinator_->ledger().TakePermissionDenialNotices();
    }

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
    // main 专属的活字段(叠层与工具可见性)在拷贝时剥掉:它们引用的是
    // main 的会话状态与注册表——模型指令/魂由派生处按需另烤进系统提示
    //(后台任务)或按现状不注(前台子代理,病十的既有不对称),延迟索引
    // 与过滤走 AgentTool 自己的口(SetDeferredIndexProvider/SetToolFilter,
    // 按子代理自己的注册表算)。批四行为不变;子代理要不要同享指令与魂,
    // 由后续单子显式裁决,不做拷贝里的暗继承。
    void SetAgentProfile(agent::AgentProfile profile) {
        agent_profile_ = std::move(profile);
        agent_profile_.model_instructions.clear();
        agent_profile_.soul.clear();
        agent_profile_.deferred_index_provider = nullptr;
        agent_profile_.tool_filter = nullptr;
        agent_profile_.tool_filter_denial.clear();
        // 动态工具 P1:main 的代理引用账也一并剥掉——子代理用自己的 resolver
        //(SetToolRefResolver 灌的子侧账),不拷贝 main 的活接线。
        agent_profile_.tool_ref_resolver = nullptr;
        agent_profile_.tool_execution_policy = nullptr;
        agent_profile_.tool_execution_denial.clear();
        runtime_profile_ = agent_profile_.runtime;
        if (!agent_profile_.request.model.empty()) {
            model_ = agent_profile_.request.model;
        }
    }

    // 派工治理(转发协调器;各字段语义见 SubagentGovernance)。P1-2 起补
    // max_children_per_task/max_tree_nodes——判决门(EvaluateAdmission)
    // P0 已备,这里只是把配置接进来;0 = 不设(与 SubagentGovernance 默认
    // 一致),旧调用方两参数版仍可用,树级两门维持不设。
    void SetDispatchGovernance(int max_active, int max_depth, int max_children_per_task = 0,
                               int max_tree_nodes = 0) {
        SubagentGovernance governance = coordinator_->governance();
        governance.max_active = max_active;
        governance.max_depth = max_depth;
        governance.max_children_per_task = max_children_per_task;
        governance.max_tree_nodes = max_tree_nodes;
        coordinator_->SetGovernance(governance);
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

    // 任务总 turn 的宿主默认(turn 预算单 §4.2):配置 subagent.default_max_
    // turns(0/未设 = 不限)。与 default_max_steps_per_turn(兼容窗旧义)
    // 分开管——自定义 Agent 的 runtime.max_turns 在 Resolver 里压过它,
    // 内置两枚与旧调用路径吃这份默认。
    void SetDefaultMaxTurns(int turns) { default_max_turns_ = turns > 0 ? turns : 0; }
    // 只读口:Workflow 的自定义 Agent 解析路复用同一份默认(turn 预算单
    // P0-3 验收线——同一 Agent 定义两路解析,预算链逐级一致,不各养一本)。
    int default_max_turns() const { return default_max_turns_; }

    // tool_search(延迟挂载):子代理注册表同机制。filter 原样灌给每次
    // execute() 新建的 sub_loop;index_provider 每次 execute() 现算"延迟
    // 未加载"索引段,拼进子代理系统提示末尾。两个都不设(默认)= 子代理
    // 不启用延迟,行为跟从前完全一样。
    void SetToolFilter(std::function<bool(const Tool&)> filter) { tool_filter_ = std::move(filter); }
    void SetDeferredIndexProvider(std::function<std::string()> provider) {
        deferred_index_provider_ = std::move(provider);
    }

    // 动态工具 P1(通用 ProxyReference):子代理侧的引用解析器与执行资格。
    // resolver 是子侧那只(独立 ledger,与 main 不串);policy 只作用于经
    // tool_invoke 解引用来的调用,直接按名调用仍走 SetToolFilter 那道
    // exposure 过滤。都不设(默认)= 子代理不开 proxy 路,行为与从前一致。
    void SetToolRefResolver(std::shared_ptr<DeferredToolResolver> resolver) { tool_ref_resolver_ = std::move(resolver); }
    void SetToolExecutionPolicy(std::function<bool(const Tool&)> policy, std::string denial) {
        sub_execution_policy_ = std::move(policy);
        sub_execution_denial_ = std::move(denial);
    }

    // 提示词运行时化(0.21.x):用户模块目录(~/.lubancode/prompts)。设了
    // 之后每次 execute() 新建子代理时,系统提示的 features 模块同走"用户
    // 文件优先、嵌入回退"。不设(默认空)= 只用嵌入版。
    void SetPromptsDir(std::string prompts_dir) { prompts_dir_ = std::move(prompts_dir); }

    // Prompt Profile(阶段 2):项目模块目录(<项目根>/.lubancode/prompts)。
    // 只有自定义 Agent 点名 Prompt Profile 时才用——Profile 的"项目选中
    // 覆盖"这一层从这儿找(default 上下文没有项目层,主 Agent 拼装不受
    // 影响,契约 §6.2)。不设(默认空)= 没有项目层。
    void SetProjectPromptsRoot(std::string dir) { project_prompts_dir_ = std::move(dir); }

    // 统一 Package 封装单阶段 3:包层 Profile 根。packaged Agent 点名的
    // canonical Profile("<包id>:<名>")只在包根里解析(命名空间与裸名不相
    // 交,见 prompt_assembler 的包层注释)。不设(默认空)= 没有包层。
    void SetPackageProfileRoots(std::vector<lubancode::agent::PackageProfileRoot> roots) {
        package_profile_roots_ = std::move(roots);
    }

    // isolation=worktree 的房务 Git 调用可替身(测试注入假 runner);
    // 不设走真 git。
    void SetGitRunner(lubancode::cli::GitRunner runner) { git_runner_ = std::move(runner); }

    void SetSkillsSegment(std::string skills_segment) { skills_segment_ = std::move(skills_segment); }
    void SetProjectInstructions(std::string instructions) { project_instructions_ = std::move(instructions); }

    // ---- AGENTS.md 作用域(作用域单 P0)----
    // 与主代理共享同一只 Resolver(全会话一份);每只子代理的已见指纹账
    // 在 RunTask 里现起、任务结束即弃(§7.6:不复制父 Agent 的最终字符串,
    // 也不共享父的确认账)。不设(旧调用方/单测)= 子代理不过闸,行为
    // 与从前一致。只读口给 Workflow 的 agent 节点取同一份,两路不各养一只。
    void SetInstructionResolver(std::shared_ptr<const lubancode::config::ProjectInstructionResolver> resolver) {
        instruction_resolver_ = std::move(resolver);
    }
    const std::shared_ptr<const lubancode::config::ProjectInstructionResolver>& instruction_resolver() const {
        return instruction_resolver_;
    }
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }  // 子代理内部的危险工具各自有确认关
    Result execute(const nlohmann::json& input) override;
    // 子代理自带 CancelChain(面板 x/父轮 ESC/墙钟在 RunTask 里并根),外层
    // 递进来的取消旗不另开旁路——using 把基类的 context 口带进重载集,
    // AgentDispatchTool 等转发壳递 (input, context) 时走基类默认适配。
    using Tool::execute;

private:
    // P0-3 的 typed 派工入口:execute()/AgentDispatchHandle 都汇到这。caller
    // 是校准过的派工者身份;env 为空 = main 直派(引擎读自家活账,调用在
    // main 线程),非空 = 嵌套(只吃冻结材料)。input 的校验、连败账、
    // title/prompt 校验都在这层做,后续函数全部吃 typed 值。
    Result ExecuteDispatch(const AgentDispatchRequest& request, AgentDispatchHandle& fail_account);

    // 一次派工的 typed 合同：入参解析一次成型，后续不再 input.at()。
    struct DispatchRequest {
        std::string title;                     // canonical 标题(spec->title)
        std::shared_ptr<const agent::AgentTaskSpec> spec;
        std::string task_input_text;           // 原始派工说明；首轮 user message 原样投递
        std::string agent_type;
        bool background = false;
        bool isolate = false;
        SubagentBudget budget;
        std::optional<CustomAgentMaterial> custom;
        std::optional<agent::ResolvedAgentProfile> resolved;
        std::optional<agent::AgentPermissionMode> permission_floor;
    };

    // background_hooks:后台任务的只读 hooks 会话(LaunchBackground 在主线程
    // 造好带进来;前台路径为空)。RunTask 拿它在后台线程发 SubagentStart/Stop
    // 与工具事件——记录只投递,主会话安全点归并。
    Result RunTask(api::Backend& backend, ToolRegistry& task_registry, const std::string& prompt,
                   const std::string& agent_type, const SubagentBudget& budget, const Hooks* foreground_hooks,
                   const std::shared_ptr<TaskRecord>& task,
                   const DetachedAgentBackend* detached = nullptr,
                   const std::string* prepared_system_prompt = nullptr,
                   const IsolationScope* isolation_scope = nullptr,
                   const std::shared_ptr<lubancode::hooks::DetachedHookSession>& background_hooks = nullptr,
                   const std::shared_ptr<const BackgroundPermissionLedger>& background_permissions = nullptr,
                   const CustomAgentMaterial* custom = nullptr,
                   const agent::ResolvedAgentProfile* resolved = nullptr,
                   std::optional<agent::AgentPermissionMode> permission_floor = std::nullopt,
                   // P0-2 轨迹:这只子代理的独立 JSONL 桥(前台/后台都在派工
                   // 线程申请好带进来;空 = flag 关的旧路)。
                   std::unique_ptr<runtime::TrajectorySubagentBridge> trajectory = nullptr,
                   // P0-3:派工者环境(main 直派为空;嵌套带冻结材料,孩子的
                   // scoped agent 工具从这派生)。
                   const std::shared_ptr<const SubagentDispatchEnv>& env = nullptr);

    Result ExecuteForeground(const DispatchRequest& request, ToolRegistry& task_registry,
                             const AgentRunIdentity& caller, const std::shared_ptr<const SubagentDispatchEnv>& env);
    Result LaunchBackground(const DispatchRequest& request, ToolRegistry& task_registry,
                            const AgentRunIdentity& caller, const std::shared_ptr<const SubagentDispatchEnv>& env);

    // 类型清单缓存(阶段 4·动态 schema):provider 的快照 + 回合边界翻新。
    // input_schema() 是 const 且会被后台任务的派工壳跨线程调——整只缓存
    // mutable,锁短持有;provider 在锁内跑,生产装配里是一次 Catalog 扫描,
    // 一回合至多一遍。
    struct AgentTypesCache {
        std::mutex mutex;
        bool loaded = false;
        std::vector<AgentTypeInfo> types;
    };
    mutable AgentTypesCache agent_types_cache_;
    std::vector<AgentTypeInfo> CachedAgentTypes() const;

    // 子代理请求的包装后端(agent_tool.cpp 内实现):一次不落地把"请求发出/
    // 首事件/逐事件/收场错误"记进活度账与诊断日志(LUBANCODE_DEBUG_SUBAGENT)。
    class TraceBackend;

    api::Backend& backend_;
    ToolRegistry& sub_registry_;
    ToolRegistry* explore_registry_ = nullptr;
    std::string cwd_;
    std::string model_;
    int default_max_steps_per_turn_;
    int default_max_turns_ = 0;  // 任务总 turn 的配置默认(turn 预算单 §4.2;0 = 不限)
    std::string skills_segment_;
    std::string prompts_dir_;  // 提示词运行时化:空 = 只用嵌入版
    std::string project_prompts_dir_;  // Prompt Profile 项目层根(阶段 2):空 = 没有项目层
    std::vector<lubancode::agent::PackageProfileRoot> package_profile_roots_;  // 包层(阶段 3)
    std::string project_instructions_;  // 当前工作目录的 AGENTS.md 分层内容
    // AGENTS.md 作用域(作用域单 P0):与主会话共享的 Resolver;空 = 旧
    // 调用方没接,子代理不过闸。并发只读,const 全程安全。
    std::shared_ptr<const lubancode::config::ProjectInstructionResolver> instruction_resolver_;
    Hooks hooks_;
    bool background_by_default_ = false;
    std::function<DetachedAgentBackend()> detached_backend_factory_;
    std::function<std::unique_ptr<ToolRegistry>()> detached_registry_factory_;
    std::function<std::function<DetachedAgentBackend()>()> frozen_backend_spawner_source_;  // P0-3 冻结工厂源
    std::function<BackgroundPermissionLedger()> background_permission_source_;  // 后台放行账源;空 = 全拒(旧行为)
    // 六职拆分批三立账,P0-3 迁完:台账/治理/线程表归协调器(唯一 owner),
    // 本类只剩装配与执行。main_handle_ 是 main 工具表那枚壳的身份+连败账。
    std::shared_ptr<AgentTaskCoordinator> coordinator_;
    AgentDispatchHandle main_handle_;
    std::function<bool(const Tool&)> tool_filter_;            // tool_search:空 = 不过滤
    std::function<std::string()> deferred_index_provider_;    // tool_search:空 = 不注索引段
    // 动态工具 P1:子侧 proxy 接线(resolver 空则整路不开)。
    std::shared_ptr<DeferredToolResolver> tool_ref_resolver_;
    std::function<bool(const Tool&)> sub_execution_policy_;
    std::string sub_execution_denial_;
    // 自定义 Agent 解析口(P2-2):空 = 只认内置两枚(旧行为)。宿主在会话
    // 装配时灌入;回调在 execute() 的宿主线程被调,内部自管线程安全。
    std::function<std::optional<CustomAgentMaterial>(const std::string&)> custom_agent_resolver_;
    // agent 类型清单源(阶段 4):空 = schema 不列清单。与 resolver 同一条
    // 寿命规矩;provider 在宿主线程或后台线程经 CachedAgentTypes 调,内部
    // 自管线程安全(生产装配里就是一次 Catalog 扫描)。
    std::function<std::vector<AgentTypeInfo>()> agent_types_provider_;
    // 解析环境供应商(阶段 3):空 = 没接(旧调用方),Resolver 的权限/依赖
    // 校验按"没账可查"跳过。与 custom_agent_resolver_ 同一条寿命规矩。
    std::function<agent::AgentProfileResolveEnvironment()> resolve_environment_;
    lubancode::cli::GitRunner git_runner_;                    // isolation 房务;空 = 真 git
    std::size_t context_window_tokens_ = 0;                   // 子代理 mid-turn 压缩评估;0 = 未知
    agent::AgentRuntimeProfile runtime_profile_;              // 运行策略:与 main 同一份(默认 unset)
    agent::AgentProfile agent_profile_;                       // main/sub/workflow 共用的代理属性形状
    // 墙钟兜底:整轮上限与收杀宽限(秒;0 = 不限)。
    int wall_clock_timeout_secs_ = 0;
    int wall_clock_grace_secs_ = kDefaultSubagentWallClockGraceSecs;
    std::function<std::string(const std::string&)> turn_context_provider_;  // 子代理记忆召回;空 = 不召回
    // ---- 连败保险(缺 title 无限重试拖死主循环单)----
    // 账随 handle 走(P0-3:main 那枚常驻 main_handle_,每只任务那枚随
    // 私有表):同一回合内同一入参错误连拒到 kParamFailLimit 次就明拒收场,
    // 不再无限喂重试。计数归回合:宿主每轮 RunTurn 重灌 Hooks(main handle
    // 的账随 SetHooks 清)、入参一旦过检都清零;换一个错误原因各自重新
    // 起算。一只 handle 只被一条线程使用,无锁访问。
    static constexpr int kParamFailLimit = 3;
};

// 子代理系统提示的统一装配口(自定义 Agent 单·阶段 5 从匿名 namespace
// 提出):agent 工具派发路(AgentTool 的 ExecuteForeground/LaunchBackground/
// RunTask)与 Workflow 的 agent 节点(AgentExecutor)两路共用同一只——
// "同一 Agent 从两路唤起,Prompt 完全同源"的验收线钉的就是这两枚函数。
// 参数全是纯数据,不摸盘、不发请求;拼装次序见 agent/prompt_assembler
// (阶段 2 黄金测试)。
agent::PromptOptions BuildSubagentPromptOptions(const std::string& cwd, const std::string& agent_type,
                                                const std::string& prompts_dir,
                                                const std::string& project_prompts_dir,
                                                const std::string& project_instructions,
                                                const std::string& skills_segment,
                                                const agent::AgentProfile& agent_profile,
                                                const CustomAgentMaterial* custom,
                                                const agent::ResolvedAgentProfile* resolved,
                                                const std::vector<agent::PackageProfileRoot>& package_roots);

// 预装技能段(skills.preload):名字与正文按位对齐,缺正文只登记名字。
// 与 BuildSubagentPromptOptions 同一批提出的同源件。
std::string AppendPreloadedSkills(const std::vector<std::string>& names,
                                  const std::vector<std::string>& bodies);

// agent_type 的工具面是否只读(真机实测 P2-3;阶段 4 起 resolver 优先):
// 接了解析口的先问它——查到码内内置两枚按旧答案(Explore 只读、
// general-purpose 不只读,explore_registry 整表只读是运行时事实,不靠
// registry 查档);查到自定义/覆盖定义走 tools.allow 查账——非空且每一枚
// 都在 registry 里注册为只读档(ReadOnlyLocal/ReadOnlyRemote)才算。空
// allow(继承全工具面)、解析失败、查无注册、含非只读档,一律 false
//(保守为纲;契约 4.9:只读不设权限档,由 tools.allow 表达)。没接解析口
//(旧调用方)退回内置两枚的旧答案。纯查表,不发请求、不碰盘之外的任何
// 状态。
bool AgentFaceIsReadOnly(
    const std::function<std::optional<CustomAgentMaterial>(const std::string&)>& resolver,
    const ToolRegistry& registry, const std::string& agent_type);

}  // namespace lubancode::tools
