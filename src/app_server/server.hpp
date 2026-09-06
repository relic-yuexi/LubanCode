// app-server 服务装配:把 Dispatcher、StdioConnection 与真家伙接起来。
// 阶段 3/4 接线的方法面:
//   - initialize/initialized/shutdown/exit(握手);
//   - thread/start、thread/list、thread/stop(会话账,走 Trajectory Journal);
//   - thread/archive、thread/unarchive、thread/delete(P9 收尾:走
//     runtime::SessionCommandService,server 不另写扫盘路);
//   - turn/start(图片输入 + 文本;工具条目带中立 diff 行表;usage/
//     context 进度事件);
//   - turn/interrupt(阶段 2);
//   - workflow/query(wf 线的 run 快照 + 增量事件出口);
// 留位方法(resume/read/steer/model/list/config/read/workflow/list)回
// kErrMethodNotFound——名字认识、执行链没有。
//
// 审批反向请求(permission/request、user/ask)与打断:阶段 2 已接线。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "app_server/browser_service.hpp"
#include "app_server/connection.hpp"
#include "app_server/dispatcher.hpp"
#include "app_server/interaction.hpp"
#include "app_server/outbox.hpp"
#include "app_server/ws_transport.hpp"
#include "app/version.hpp"
#include "config/config.hpp"
#include "runtime/command_service.hpp"
#include "runtime/goal_coordinator.hpp"
#include "runtime/interaction_broker.hpp"
#include "runtime/loop_scheduler.hpp"
#include "runtime/session_command_service.hpp"
#include "runtime/session_runtime.hpp"
#include "runtime/turn_runtime.hpp"
#include "tools/registry.hpp"

namespace lubancode::app_server {

// 服务进程的平台标识(initialize 结果里的 platform 字段)。
std::string PlatformId();

// 配置缺省(哪一级都没写 max_steps_per_turn)时 app-server 回合的步数默认
// 闸。协议前端没有 ESC 可打断,不设闸会真跑飞,故缺省给正数——终端主会话
// 的对应默认是 0(不限,防跑飞靠用户打断),协议宿主没有这层保险。用户在
// 任何一级写了就吃什么,含显式 0(不限,那是用户自己的选择)。
inline constexpr int kAppServerDefaultMaxStepsPerTurn = 32;

// 步数闸的配置解析(装配层与单测共用同一份):sources 落 Default(四级合并
// 谁都没给)时用 kAppServerDefaultMaxStepsPerTurn,否则原样吃
// config.max_steps_per_turn——与终端主会话(app/runtime_profile 的
// BuildMainRuntimeProfile)同一条解析轴,不再自造默认硬盖。
int ResolveMaxStepsPerTurn(const config::ConfigResult& config_result);

// 一场 thread(协议层的一个会话)在服务侧的全部家当。
struct ThreadRecord {
    // P0-2:thread_id = workspace session id(Trajectory 唯一真账发号;
    // 旧 SessionStore 会话 id 退役)。
    std::string thread_id;
    std::string cwd;         // 本场工作目录
    std::atomic<bool> turn_running{false};
    std::string turn_id;     // 在跑/最近一轮的 id
    // 打断旗:turn/interrupt 置位,回合驱动线程在流式/工具边界收口。
    // 跨线程置位/读取,回合驱动读它传给 AgentLoop::Run 的 cancel 指针。
    std::atomic<bool> interrupt_requested{false};
    // 这一场打断过的回合(interrupt 只对"当前在跑的那一轮"生效,迟到的
    // interrupt 不追旧账:收口了的回合没有旗可拔)。
    std::string interrupted_turn;
    // 交互悬起件:审批/ask_user 的 pending 表 + 会话级放行账。
    // (构造时 thread_id 未定,先空着,thread_id 生成后 Reset。)
    std::unique_ptr<InteractionLedger> interactions = std::make_unique<InteractionLedger>(std::string());
    // 回合工作线程(一场同拍最多一轮,joinable 即在跑)。
    std::thread turn_worker;
    // 回合收尾信号(RunTurnToCompletion 末尾置位):interrupt 的硬时限
    // 等这条,等不到(join 前)按"卡死不退"收线处理。
    std::atomic<bool> turn_finished{false};
    // 最近一轮的 turn/completed params(HandleTurnStart 同步口径取回)。
    nlohmann::json last_completed;
    // 本场 main.jsonl 的绝对路径(trace/query 断线补账/冷回放用;账本
    // 开张成功后由 thread/start 填)。
    std::string session_main_path;

    // goal 单合流批:typed 命令面(goal 六 + loop 七 + plan 三)的会话级
    // 状态。goal/loop 的状态机真值按 thread 各一本(一场 thread 一只
    // active goal/一组 loop task,与终端会话同规矩);Plan 走
    // SessionRuntime(mode 真值 + 计划成品账)。coordinator 的 goals_enabled
    // 与 scheduler 的 enabled 由装配层从 options.features_goal/loop 折。
    std::unique_ptr<runtime::goal::GoalCoordinator> goal_coordinator;
    std::unique_ptr<runtime::loop::LoopScheduler> loop_scheduler;
    std::unique_ptr<runtime::SessionRuntime> session_runtime;

    explicit ThreadRecord(std::string id)
        : thread_id(std::move(id)) {}
};

// 装配选项。
struct ServerOptions {
    std::string workspaces_dir;   // P0-2:唯一持久化根(<home>/.lubancode/workspaces)
    std::string cwd;              // 服务进程当前目录(事件里回给前端)
    std::string lubancode_version = std::string(app::kVersion);
    // 会话档 meta 真值(阶段 3 冻结项:thread/start 的 meta 不再写占位)。
    // wire 是 "anthropic"/"responses"/"chat",model 是配置里的模型名;
    // 空串照写(纯内存/测试没有配置可读)。
    std::string session_wire;
    std::string session_model;
    // workflow run 账根目录(workflow/query 用;空 = 该法子报没有)。
    std::string workflow_runs_dir;
    // 出站队列容量(事件账的有界上限)。
    std::size_t outbox_capacity = 4096;
    // 审批悬停时限(毫秒)。0 = 不限(悬到断线/打断)。悬停不偷跑:时限
    // 到了按"没人可答"的悬空收口处理,不冒充用户拒绝。
    int approval_timeout_ms = 0;
    // 非终端宿主同样由 Runtime 权限核裁定；缺省仍为可询问。值域 = 公共
    // ApprovalMode(收口审计单 P1:runtime 侧镜像枚举已删)。
    lubancode::ApprovalMode permission_mode = lubancode::ApprovalMode::Default;
    // turn/interrupt 的硬时限(毫秒)。打断旗置位后回合驱动最多再等这么
    // 久:AgentLoop 的 cancel 在流式/工具边界生效,工具跑完了才看旗;真
    // 有卡死不看的(长命令/卡住的外部进程),硬时限一到强制收线,终态照
    // 发 interrupted,不留挂着回合。0 = 不设硬时限(只靠 cancel 旗)。
    int interrupt_hard_deadline_ms = 15000;
    // goal/loop 的 feature 门(goal 单合流批):false 时 typed 命令面回
    // goal.disabled/loop.disabled 稳定码(不冒充成功);缺省关(与终端
    // 的 features.goals/features.loop 缺省一致,装配层显式开)。
    bool features_goal = false;
    bool features_loop = false;
    // 回合步数闸:装配层经 ResolveMaxStepsPerTurn 把配置轴
    // max_steps_per_turn 折进来(用户写了就吃什么);缺省
    // kAppServerDefaultMaxStepsPerTurn。单测直设此值注小闸验回路。
    int max_steps_per_turn = kAppServerDefaultMaxStepsPerTurn;
    // 浏览器面(阶段 3):sidecar 命令与参数、截图 artifact 目录。
    // sidecar_command 空 = browser/* 方法回 browser.not_configured(不冒充)。
    std::string browser_sidecar_command;
    std::vector<std::string> browser_sidecar_args;
    std::string browser_artifact_dir;
    // 镜像流(阶段 C):在飞待落盘帧的有界队列容量,0 = 用 BrowserService
    // 的缺省(8)。测试用小容量逼出"慢消费者丢帧"确定性地发生。
    int browser_screencast_queue_capacity = 0;
    // WS 承载(多前端外壳单阶段 A):有值 = Run() 走 WS 监听而不是 stdio
    // (两承载按需起一种,不并跑——stdio 的"EOF 即进程收线"与 WS 的
    // "断线只收连接、进程等重连"语义不同,混跑两头都拧巴)。
    std::optional<WsOptions> ws;
};

// 一台 app-server。一个进程一台;装配好后 Run() 进主循环(stdio 或 WS
// 承载,按 ServerOptions::ws 择一)。
class Server {
public:
    // backend_factory:回合驱动用的 api::Backend(生产是 cpr 后端,测试
    // 注入假 backend)。registry_factory:工具注册表(空 = 无工具的空表)。
    using BackendFactory = std::function<std::unique_ptr<api::Backend>()>;
    using RegistryFactory = std::function<std::unique_ptr<tools::ToolRegistry>()>;

    Server(ServerOptions options, BackendFactory backend_factory, RegistryFactory registry_factory);
    ~Server();

    // 装配方法表,跑主循环到收线:配了 ws 走 WS 监听,否则 stdio。返回
    // 退出码。
    int Run();

    // ---- WS 承载(阶段 A) ----
    // 一条连接服务完的分型:纯断线(进程活着等重连)、对端 exit/shutdown
    // (整场收线,stdio 同义)、监听收摊(叫停或监听层错,没有后续)。
    enum class WsServeOutcome { Disconnected, ExitRequested, ListenerStopped };

    // 接一条 WS 连接并服务到收线:每条连接自己的 Dispatcher(握手状态机
    // 不许跨连接复用)与 StdioConnection(WS 帧收发折成 writer/reader),
    // 收线后打断在跑的回合(浏览器会话与 thread 账不动——重连续用)。
    // 单测直驱也走它(一条一条喂连接,不进死循环)。
    WsServeOutcome ServeWsConnection(WsTransport& transport);

    // 服务一条已接好的 WS 会话(ServeWsConnection 的内核)。RunWsLoop 的
    // 专职 accept 线程把升级好的 Session 递进来——会话仍一条一条服务
    //(阶段 A 语义),但 accept 不再被在服务的会话堵死:参考前端(阶段 D)
    // 开着 WS 的同时经 HTTP 取 artifact 字节,两头并发。
    WsServeOutcome ServeWsSession(std::unique_ptr<WsTransport::Session> session);

    // 喂给 ServeWsConnection 的 dispatcher 工厂:每条 WS 连接新铸一只,
    // 方法表与 stdio 那只同源(RegisterMethods + browser 面 + 能力表)。
    std::shared_ptr<Dispatcher> MakeWsDispatcher();

    // 收线:在跑的回合按打断收口(置旗 + 清悬起 + 等收尾),等不到就分离。
    // Run() 返回前与析构都会走;断管/exit 的"打断在跑回合,停后台任务"
    // 落在这里(单子协议底线第一节)。
    void Shutdown();

    // 单测直驱:注入假连接(假 writer/reader),不起进程、不碰 stdio。
    void AttachForTest(std::unique_ptr<StdioConnection> connection);

    // ---- 以下给单测直驱(不起进程、不碰 stdio) ----

    Dispatcher& dispatcher() { return *dispatcher_; }
    // 整只 dispatcher(测试自建连接时要 shared_ptr 形态)。
    const std::shared_ptr<Dispatcher>& dispatcher_handle() const { return dispatcher_; }
    StdioConnection& connection() { return *connection_; }

    // thread/start 的处理体(单测直调)。
    nlohmann::json HandleThreadStart(const nlohmann::json& params, std::string& out_error_code);
    // thread/list 的处理体(查询参数透传 SessionCommandService)。
    nlohmann::json HandleThreadList(const nlohmann::json& params = nlohmann::json::object());
    // thread/stop 的处理体。
    nlohmann::json HandleThreadStop(const std::string& thread_id, std::string& out_error_code);
    // thread/archive|unarchive|delete 的处理体(SessionCommandService 执行)。
    // accepted=false 时 out_error_code/out_error_message 有值(稳定码,
    // SessionCommandService 的错误码表)。out_state:archive 给
    // "archived",unarchive 给 "active",delete 给空。
    nlohmann::json HandleThreadLifecycle(const std::string& method, const std::string& thread_id,
                                         const nlohmann::json& params, std::string& out_error_code,
                                         std::string& out_error_message, std::string& out_state);
    // workflow/query 的处理体:快照 + lastSeq+1 起的增量事件(事件走
    // 返回值,不经 connection——协议 handler 拿去经 emit_event 出)。
    // 错误:out_error_code 稳定码("not_found"/"no_workflow_dir")。
    struct WorkflowQueryResult {
        nlohmann::json snapshot;
        std::vector<nlohmann::json> events; // 每条已是协议事件形状(method/params 内层)
    };
    WorkflowQueryResult HandleWorkflowQuery(const std::string& run_id, std::uint64_t last_seq,
                                             std::string& out_error_code,
                                             std::string& out_error_message);
    // turn/start 的处理体:同步跑完一整回合(假 backend 一趟即终),
    // 事件从 emit 出去。返回 turn/completed 的 params。images 是
    // CheckTurnStartParams 折出来的图片输入(空 = 纯文本)。
    nlohmann::json HandleTurnStart(const std::string& thread_id, const std::string& text,
                                   const std::vector<nlohmann::json>& images, std::string& out_error_code);
    // turn/start 的受理体(协议路径):立工作线程跑整回合,立即回
    // {threadId, turnId};终态走 turn/completed 事件。
    nlohmann::json AcceptTurnStart(const std::string& thread_id, const std::string& text,
                                   const std::vector<nlohmann::json>& images, std::string& out_error_code);
    // turn/interrupt 的处理体:置打断旗。turn_id 空 = 该 thread 当前在跑
    // 的回合;回合不在跑(收口了/没这回合)报 stale,不追旧账。
    // 返回空串 = 受理;否则 out_error_code 记原因("stale" = 迟到)。
    nlohmann::json HandleTurnInterrupt(const std::string& thread_id, const std::string& turn_id,
                                       std::string& out_error_code);
    // 反向请求响应的处理体(审批/ask_user 的前端答复):对到 thread 的
    // 悬起件上。result 只在 ok 时有意义。
    InteractionResolution HandleInteractionResponse(const IncomingResponse& response);

    // goal/loop/plan 的 typed 命令执行体(goal 单合流批):折成
    // ClientCommand 交 CommandService,回执折协议响应。线程模型:goal/
    // loop 的内存状态机自带锁(scheduler)/只归读线程碰(coordinator 与
    // 会话泵同线程——app-server 的会话泵就是读线程,turn 工作线程不碰
    // 它们)。error 时错误码走 data.reason 带稳定串(协议 v1 的错误码段
    // 没有 goal.*/loop.*/plan.* 专属号)。
    nlohmann::json HandleTypedDomainCommand(const IncomingRequest& request, bool& out_error,
                                            std::string& out_error_code, std::string& out_error_message);

    // 当前活着的 thread 数(测试断言用)。
    std::size_t active_thread_count();

    // ---- 测试直驱:browser 面的持有体(注入假 sidecar / 直查状态用) ----
    BrowserService& browser_service() { return *browser_; }

private:
    void RegisterMethods(Dispatcher& dispatcher);
    // WS 主循环(Run 的 ws 分支):起监听、逐条服务连接到整场收线。
    int RunWsLoop();
    // 反向请求响应的装配口(stdio 与 WS 同一份):HandleInteractionResponse
    // 的薄封,错误码口径见 connection.hpp。
    std::function<std::string(const IncomingResponse&)> MakeInteractionResolver();
    // 事件出水的安全口:快照当下活连接再发(WS 换连接的窗口里,回合工作
    // 线程/分离出去的僵尸线程不扑空)。没有活连接 = 丢弃(有界队列语义
    // 的极端版:没人听的事件不留)。
    void EmitEventSafe(std::string_view method, const nlohmann::json& params);
    // 在跑的回合一律按打断收口(WS 连接收线后调用:浏览器会话不动,只
    // 把回合从旧连接上摘下来)。Shutdown 的回合段就是它。
    void InterruptRunningTurns();
    // 整回合驱动(工作线程体):审批/ask_user 悬停、打断旗、终态分型。
    void RunTurnToCompletion(const std::shared_ptr<ThreadRecord>& record, const std::string& thread_id,
                             const std::string& turn_id, const std::string& text,
                             const std::vector<nlohmann::json>& images);

    // browser 动作的审批询问(browser_service 的 ApprovalAsk 落点):
    // 挂到 thread 的悬起件上,取消旗贯通(动作取消即悬空收口 + 擦账)。
    std::optional<BrowserService::ApprovalTicket> HandleBrowserApproval(const std::string& thread_id,
                                                                        const runtime::ApprovalRequest& request,
                                                                        const std::atomic<bool>* cancel);

    // 找一场 thread 的家当(锁内拷 shared_ptr)。
    std::shared_ptr<ThreadRecord> FindThread(const std::string& thread_id);

    ServerOptions options_;
    BackendFactory backend_factory_;
    RegistryFactory registry_factory_;
    std::shared_ptr<Dispatcher> dispatcher_;
    // 活连接(stdio 一条用到退场;WS 每条连接换一只)。回合工作线程经
    // EmitEventSafe 快照访问,换装由 Run/ServeWsConnection 独占做。
    std::shared_ptr<StdioConnection> connection_;
    std::mutex connection_mutex_;
    // P9 收尾:thread/list|archive|unarchive|delete 的执行体。server 不
    // 另写扫盘路,全从这里走(workspaces 根空 = 没建,搬删一律拒)。
    std::unique_ptr<runtime::SessionCommandService> session_commands_;
    // 服务进程默认 workspace 的 key(scope=cwd 的 thread/list 用;按
    // options_.cwd 四级裁决,thread 各自的 cwd 在各自账里)。
    std::string DefaultWorkspaceKey() const;
    // 浏览器面(阶段 3):sidecar 进程管理 + browser/* 方法与事件。
    std::unique_ptr<BrowserService> browser_;

    std::mutex threads_mutex_;
    std::map<std::string, std::shared_ptr<ThreadRecord>> threads_;

    // 旧会话档目录(P0-2 起不消费,P0-6 删)。
    // P0-2:唯一持久化根(~/.lubancode/workspaces)。
    std::string workspaces_dir_;
    // turnId/itemId 派生:P9 起统一走 runtime::ProcessIdAuthority(进程级
    // 单份,见 runtime/id_authority.hpp),这里的回合计数账已拆。
};

}  // namespace lubancode::app_server
