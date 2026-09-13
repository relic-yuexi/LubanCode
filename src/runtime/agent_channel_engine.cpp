// AgentChannelEngine 实现(多渠道消息接入单阶段 3)。装配合同见头文件。
#include "runtime/agent_channel_engine.hpp"

#include "config/config.hpp"
#include "runtime/hook_host_services.hpp"
#include "runtime/interaction.hpp"
#include "runtime/middleware_runtime.hpp"
#include "runtime/middleware_v3_sink.hpp"
#include "tools/path_utils.hpp"
#include "tools/session_utils.hpp"  // NowIdTimestamp(P0-6 自 sessions 迁来)
#include "workspace/identity.hpp"

namespace lubancode::runtime {

namespace {

// (ApplyChannelToolPolicy 的定义在下方具名空间外——V1 起与 Gateway
// headless 执行器共用,公开导出。)

// 受保护路径闸的包装层(QQ 接入单 Q0;security.md §3):拦在"实际工具
// 执行入口"——不管这枚调用从哪条路来(直名调用/延迟挂载/tool_invoke
// 解引用),Registry 里查到的都是这层壳,execute 前先过路径闸。只查
// 认得的路径入参(read_file.path/search.path,见 channel::ChannelToolPathBlocked),
// 其余工具透传——工具能力收窄归五层交集,这里管的是"允许的工具也不许
// 碰凭据与全局密钥配置"。
class GuardedChannelTool : public tools::Tool {
public:
    GuardedChannelTool(tools::Tool& inner, channel::ChannelProtectedPaths protected_paths,
                       std::string default_search_root)
        : inner_(inner),
          protected_paths_(std::move(protected_paths)),
          default_search_root_(std::move(default_search_root)) {}

    std::string name() const override { return inner_.name(); }
    std::string description() const override { return inner_.description(); }
    nlohmann::json input_schema() const override { return inner_.input_schema(); }
    bool needs_confirm() const override { return inner_.needs_confirm(); }
    tools::ApprovalClass approval_class() const override { return inner_.approval_class(); }
    bool deferred() const override { return inner_.deferred(); }

    tools::Tool::Result execute(const nlohmann::json& input) override {
        if (const std::string blocked = channel::ChannelToolPathBlocked(
                name(), input, protected_paths_, default_search_root_);
            !blocked.empty()) {
            return tools::Tool::Result::Error(blocked);
        }
        return inner_.execute(input);
    }

    tools::Tool::Result execute(const nlohmann::json& input,
                                const tools::ToolExecutionContext& context) override {
        if (const std::string blocked = channel::ChannelToolPathBlocked(
                name(), input, protected_paths_, default_search_root_);
            !blocked.empty()) {
            return tools::Tool::Result::Error(blocked);
        }
        return inner_.execute(input, context);
    }

private:
    tools::Tool& inner_;
    channel::ChannelProtectedPaths protected_paths_;
    std::string default_search_root_;
};

}  // namespace

// §16.2 的权限交集在暴露面执法:五层交集后的 allow/deny(渠道/账号/
// 所有命中 binding)叠进 AgentProfile 的 tool_filter(原 profile 已有
// 过滤的先过,再过渠道层——每层只收窄)。被滤掉的工具模型看都看不见;
// 看得见但 needs_confirm 的调用点再由 on_tool_confirm 的 fail closed
// 裁定;逐轮收窄由 RunTurn 的 on_pre_tool_use_hook 闸兜住(三层各管一段)。
agent::AgentProfile ApplyChannelToolPolicy(agent::AgentProfile profile,
                                           const channel::ToolRoutePolicy& policy) {
    if (!policy.allow.has_value() && policy.deny.empty()) {
        return profile;  // 没有任何层设上限:不添乱,交给 Agent 自身工具表
    }
    auto prior = profile.tool_filter;
    const channel::ToolRoutePolicy policy_copy = policy;
    profile.tool_filter = [prior, policy_copy](const tools::Tool& tool) {
        if (prior && !prior(tool)) {
            return false;
        }
        return policy_copy.Allows(tool.name());
    };
    profile.tool_filter_denial =
        "channel.policy_denied|该工具不在渠道工具上限的允许名单内(渠道/账号/"
        "binding 的 tools.allow 交集没列它,或进了任一层 deny)。要放行须在全局 "
        "config 的渠道段/账号段/binding 显式声明。";
    return profile;
}

AgentChannelEngine::AgentChannelEngine(api::Backend& backend, tools::ToolRegistry& registry,
                                       agent::AgentProfile profile, Options options)
    : options_(std::move(options)), session_runtime_([this]() {
        SessionRuntime::Options runtime_options;
        runtime_options.wire_name = options_.wire_name;
        runtime_options.start_ts = tools::NowIdTimestamp();
        runtime_options.lubancode_version = options_.lubancode_version;
        // P0-2(Trajectory 升为唯一 Session):账本恒开;身份按 engine 的
        // cwd 四级裁决(P0-1 规矩:不认进程 current_path)。
        const std::filesystem::path identity_cwd = tools::Utf8ToPath(options_.cwd);
        // 身份裁决的 home = workspaces 树宿主根 = 状态根(应用Worker接入单 §4.2)。
        const auto identity_home = config::StateRootDir();
        auto identity = workspace::ResolveWorkspaceIdentity(
            identity_cwd, identity_home.has_value() ? tools::Utf8ToPath(*identity_home)
                                                    : std::filesystem::path());
        if (identity.has_value()) {
            runtime_options.trajectory_workspace_identity = std::move(*identity);
        }
        if (!options_.workspaces_dir.empty()) {
            runtime_options.trajectory_workspaces_root = tools::Utf8ToPath(options_.workspaces_dir);
        }
        return runtime_options;
    }()),
      protected_paths_([this]() {
          // 受保护路径闸(Q0):默认表 + 装配层追加的根。
          channel::ChannelProtectedPaths paths = channel::DefaultChannelProtectedPaths();
          for (const std::string& root : options_.extra_protected_roots) {
              if (!root.empty()) {
                  paths.roots.push_back(root);
              }
          }
          return paths;
      }()),
      guarded_registry_([this, &registry]() {
          // search 不带 path 时的起点按会话 cwd(options_.cwd)——不认
          // 进程 current_path,与身份裁决同一条规矩。注册元数据
          //(来源/实例/版本/副作用档)原样拷贝,不把 MCP/插件洗成 builtin。
          auto guarded = std::make_unique<tools::ToolRegistry>();
          for (const auto& tool : registry.All()) {
              tools::ToolRegistration registration;
              registration.tool = std::make_unique<GuardedChannelTool>(
                  *tool, protected_paths_, options_.cwd);
              if (const tools::ToolRegistration* source = registry.RegistrationOf(tool->name());
                  source != nullptr) {
                  registration.source_kind = source->source_kind;
                  registration.source_instance = source->source_instance;
                  registration.version_or_digest = source->version_or_digest;
                  registration.effect_class = source->effect_class;
                  registration.idempotency = source->idempotency;
                  registration.recovery = source->recovery;
                  registration.package_origin = source->package_origin;
              }
              guarded->Register(std::move(registration));
          }
          return guarded;
      }()),
      agent_(backend, *guarded_registry_,
             ApplyChannelToolPolicy(std::move(profile), options_.tools)) {}

agent::RunOutcome AgentChannelEngine::RunTurn(const TurnIngress& ingress, std::string* reply_text,
                                              std::string* error) {
    if (reply_text != nullptr) reply_text->clear();
    if (error != nullptr) error->clear();

    // (P0-6:旧存档建档路已删;账本在 SessionRuntime ctor 里恒开。)
    const std::size_t history_before = agent_.history().size();

    // 事件出水:每轮一只适配器(与终端路同款;sink 没挂就只发号)。
    // Start 先走:turn_id 在这里 mint,轨迹桥吃同一枚(与 trace 同口径)。
    TurnEventAdapter turn_events = session_runtime_.MakeTurnAdapter();
    const std::string turn_id = turn_events.Start();

    // LuaHook 单 P0-B:渠道路(app-server/headless)的中间件接线。与 CLI/
    // one-shot(turn_runner)共用同一 runtime 派发点(RunPreUserMiddleware/
    // RunPostUserMiddleware/RunPreRequestMiddleware),不另接一套。没装配
    // (hook_dispatcher 空/零注册)= 恒等结果,一个字节不动。渠道输入的
    // origin 按 provenance 分档:HumanTerminal/PeerSession 都是宿主外的
    // 真来信,按 human 报;purpose=interactive,delivery_mode=direct。
    hooks::HookDispatcher* dispatcher = options_.hook_dispatcher;
    // P0-B 遗留①(P1-C 补):渠道路同样把会话 v3 主写者挂进中间件事件账
    // 与 hook 工具桥的子执行账(幂等;v2 场/未开卷 = 解绑)。
    BindMiddlewareSessionWriter(dispatcher, &DefaultHookServiceCenter(),
                                session_runtime_.trajectory() != nullptr
                                    ? session_runtime_.trajectory()->v3_main_writer()
                                    : nullptr);
    std::string effective_text;
    for (const auto& block : ingress.message.content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            effective_text = text->text;
            break;
        }
    }
    MiddlewareHookContext middleware_context;
    middleware_context.turn_id = turn_id;
    middleware_context.origin = "human";
    middleware_context.purpose = "interactive";
    middleware_context.delivery_mode = "direct";
    const PreUserGate pre_user = RunPreUserMiddleware(dispatcher, effective_text, middleware_context);
    if (pre_user.blocked) {
        // 阻断发生在接纳之前:turn 事实未建,不伪造收口账。
        if (error != nullptr) {
            *error = "PreUser 钩子阻断本轮: " + pre_user.block_reason;
        }
        return agent::RunOutcome{};
    }
    api::Message effective_message = ingress.message;
    if (pre_user.dispatched && pre_user.rewritten) {
        // 改写被采用:替换首枚文本块(工作版本;附件块不动)。
        for (auto& block : effective_message.content) {
            if (auto* text = std::get_if<api::TextBlock>(&block)) {
                text->text = pre_user.prompt;
                break;
            }
        }
        effective_text = pre_user.prompt;
    }
    for (const std::string& append : pre_user.additional_context) {
        effective_message.content.push_back(api::TextBlock{"[PreUser 钩子附加上下文,非用户手敲]\n" + append});
    }

    // P0-2(Trajectory 升为唯一 Session):渠道轮的真账进 Journal——本轮
    // 边界桥管 input/模型请求/输出/收口。工具栅栏经 ToolTraceHub 的路
    // 是 channel 线后续批次的活,这里不伪造。
    std::unique_ptr<TrajectoryTurnBridge> trajectory_bridge;
    if (TrajectorySessionLedger* ledger = session_runtime_.trajectory(); ledger != nullptr) {
        trajectory_bridge = ledger->NewTurnBridge({"", options_.wire_name, "channel"});
        if (trajectory_bridge != nullptr) {
            trajectory_bridge->BeginTurn(turn_id, "peer_agent");
            trajectory_bridge->RecordInput(effective_message);
        }
    }

    // LuaHook 单 P0-B:PostUser(user 消息与接纳关系落稳后、模型请求准备前;
    // §4.47)。追加带来源的隐藏上下文;required 失败阻断本轮(原 user 保留)。
    const PostUserAppend post_user = RunPostUserMiddleware(dispatcher, effective_text, middleware_context);
    if (post_user.blocked) {
        if (error != nullptr) {
            *error = "PostUser 钩子阻断本轮: " + post_user.block_reason;
        }
        if (trajectory_bridge != nullptr) {
            trajectory_bridge->EndTurn(/*ok=*/false, /*cancelled=*/false, "post_user_denied");
        }
        return agent::RunOutcome{};
    }
    for (const std::string& append : post_user.context_appends) {
        effective_message.content.push_back(api::TextBlock{"[PostUser 钩子附加上下文,非用户手敲]\n" + append});
    }

    // 渠道轮的最小接线:无终端、无远端审批——工具确认 fail closed。
    agent::TurnWiring wiring;
    wiring.events = &turn_events;
    wiring.boundary_recorder = trajectory_bridge.get();
    // LuaHook 单 P0-B:PreRequest(§4.36)——与终端路同一函数;拦下即整步
    // 明败。没配(HookDispatcher 空/PreRequest 零注册)不设回调。
    if (HasPreRequestMiddleware(dispatcher)) {
        wiring.on_pre_request_hooks = [dispatcher](const std::string& step_id, const std::string& turn_id_,
                                                   const nlohmann::json& frozen_request_snapshot,
                                                   const PreRequestBudget& budget) {
            MiddlewareHookContext context;
            context.turn_id = turn_id_;
            context.step_id = step_id;
            context.purpose = "interactive";
            const PreRequestStages stages =
                RunPreRequestMiddleware(dispatcher, frozen_request_snapshot, budget, context);
            if (!stages.dispatched || stages.decision == "allow") {
                return std::string();
            }
            return "PreRequest 钩子拦下本次请求[" + stages.decision + "]: " + stages.reason;
        };
    }
    // 逐轮工具策略(QQ 接入单 Q0):TurnIngress.tools 是执行前重验准入后
    // 冻结的五层交集账,随入账带进执行;没递(终端路/旧装配)回落会话级
    // options_.tools。比会话级窄时(权限撤销/收窄发生在建档后),这枚
    // PreToolUse 闸在执行口拦下——暴露面(会话级 tool_filter)不动,不折
    // 前缀缓存;须确认工具由确认口按同一份冻结策略 fail closed。
    const channel::ToolRoutePolicy& effective_tools =
        ingress.tools.has_value() ? *ingress.tools : options_.tools;
    wiring.on_tool_confirm = [&effective_tools](const std::string& /*tool_use_id*/,
                                                const std::string& name,
                                                const nlohmann::json& /*input*/) {
        return ChannelConfirmAllows(effective_tools, name);
    };
    wiring.on_tool_denial_text = [](const std::string& /*tool_use_id*/,
                                    const std::string& name) {
        return ChannelToolDenialText(name);
    };
    wiring.on_pre_tool_use_hook =
        [&effective_tools](const std::string& /*tool_use_id*/, const std::string& name,
                           const nlohmann::json& /*input*/) {
            runtime::ToolHookDecision decision;
            if (!effective_tools.Allows(name)) {
                decision.decision = runtime::ToolHookDecision::Decision::Deny;
                decision.reason =
                    "工具 " + name +
                    " 不在本轮渠道工具上限的允许名单内(渠道/账号/binding 的 tools.allow "
                    "交集没列它,或进了任一层 deny)。本轮策略以执行前重验为准;要放行须在"
                    "全局 config 显式声明。";
            }
            return decision;
        };

    const auto outcome = agent_.Run(effective_message, wiring);
    if (!outcome.has_value()) {
        if (error != nullptr) {
            *error = outcome.error();
        }
    }

    // 回复正文粗账:本轮新增 assistant 消息的 TextBlock 拼接(空正文不
    // 造块——阶段 4 ReplyAssembler 只吃 ServerEvent,这份只是 host 的
    // 回执底账)。
    if (reply_text != nullptr) {
        const auto& history = agent_.history();
        for (std::size_t i = history_before; i < history.size(); ++i) {
            if (history[i].role != api::Role::Assistant) continue;
            for (const auto& block : history[i].content) {
                if (const auto* tb = std::get_if<api::TextBlock>(&block)) {
                    *reply_text += tb->text;
                }
            }
        }
    }

    // 收口:本轮边界桥封 turn 终态(成败如实;provenance 进 Journal 的
    // typed 投影是 channel 线后续批次的活)。
    if (trajectory_bridge != nullptr) {
        const bool ok = outcome.has_value();
        trajectory_bridge->EndTurn(ok, /*cancelled=*/false,
                                    ok ? std::string("done") : outcome.error());
    }
    return outcome.has_value() ? *outcome : agent::RunOutcome{};
}

}  // namespace lubancode::runtime
