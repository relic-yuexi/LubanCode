// app-server 服务装配:把 Dispatcher、StdioConnection 与真家伙接起来。
// 骨架期(thread runtime)接线的方法面:
//   - initialize/initialized/shutdown/exit(握手);
//   - thread/start、thread/list、thread/stop(会话账,复用 SessionStore);
//   - turn/start(假 backend 一整回合,事件账走 outbox);
// 留位方法(resume/read/steer/interrupt/model/list/config/read)回
// kErrMethodNotFound——名字认识、执行链没有。
//
// 审批反向请求(permission/request、user/ask)与打断:协议位在
// protocol.hpp/schema.hpp 立住,执行链等 Broker(另一条线),本层不碰。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/session_store.hpp"
#include "api/backend.hpp"
#include "app_server/connection.hpp"
#include "app_server/dispatcher.hpp"
#include "app_server/interaction.hpp"
#include "app_server/outbox.hpp"
#include "app/version.hpp"
#include "tools/registry.hpp"

namespace lubancode::app_server {

// 服务进程的平台标识(initialize 结果里的 platform 字段)。
std::string PlatformId();

// 一场 thread(协议层的一个会话)在服务侧的全部家当。
struct ThreadRecord {
    std::string thread_id;   // = SessionStore 的会话 id
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
    // 场次存档句柄(落盘由它管;失败不拦协议,只打 stderr)。
    std::unique_ptr<agent::SessionStore> store;

    explicit ThreadRecord(std::string id)
        : thread_id(std::move(id)) {}
};

// 装配选项。
struct ServerOptions {
    std::string sessions_dir;     // 会话档目录(空 = 不落盘,纯内存跑)
    std::string cwd;              // 服务进程当前目录(事件里回给前端)
    std::string lubancode_version = std::string(app::kVersion);
    // 出站队列容量(事件账的有界上限)。
    std::size_t outbox_capacity = 4096;
    // 审批悬停时限(毫秒)。0 = 不限(悬到断线/打断)。悬停不偷跑:时限
    // 到了按"没人可答"的悬空收口处理,不冒充用户拒绝。
    int approval_timeout_ms = 0;
    // turn/interrupt 的硬时限(毫秒)。打断旗置位后回合驱动最多再等这么
    // 久:AgentLoop 的 cancel 在流式/工具边界生效,工具跑完了才看旗;真
    // 有卡死不看的(长命令/卡住的外部进程),硬时限一到强制收线,终态照
    // 发 interrupted,不留挂着回合。0 = 不设硬时限(只靠 cancel 旗)。
    int interrupt_hard_deadline_ms = 15000;
};

// 一台 app-server。一个进程一台;装配好后 Run() 进 stdio 主循环。
class Server {
public:
    // backend_factory:回合驱动用的 api::Backend(生产是 cpr 后端,测试
    // 注入假 backend)。registry_factory:工具注册表(空 = 无工具的空表)。
    using BackendFactory = std::function<std::unique_ptr<api::Backend>()>;
    using RegistryFactory = std::function<std::unique_ptr<tools::ToolRegistry>()>;

    Server(ServerOptions options, BackendFactory backend_factory, RegistryFactory registry_factory);
    ~Server();

    // 装配方法表,跑 stdio 主循环到收线。返回退出码。
    int Run();

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
    // thread/list 的处理体。
    nlohmann::json HandleThreadList();
    // thread/stop 的处理体。
    nlohmann::json HandleThreadStop(const std::string& thread_id, std::string& out_error_code);
    // turn/start 的处理体:同步跑完一整回合(假 backend 一趟即终),
    // 事件从 emit 出去。返回 turn/completed 的 params。
    nlohmann::json HandleTurnStart(const std::string& thread_id, const std::string& text,
                                   std::string& out_error_code);
    // turn/start 的受理体(协议路径):立工作线程跑整回合,立即回
    // {threadId, turnId};终态走 turn/completed 事件。
    nlohmann::json AcceptTurnStart(const std::string& thread_id, const std::string& text,
                                   std::string& out_error_code);
    // turn/interrupt 的处理体:置打断旗。turn_id 空 = 该 thread 当前在跑
    // 的回合;回合不在跑(收口了/没这回合)报 stale,不追旧账。
    // 返回空串 = 受理;否则 out_error_code 记原因("stale" = 迟到)。
    nlohmann::json HandleTurnInterrupt(const std::string& thread_id, const std::string& turn_id,
                                       std::string& out_error_code);
    // 反向请求响应的处理体(审批/ask_user 的前端答复):对到 thread 的
    // 悬起件上。result 只在 ok 时有意义。
    InteractionResolution HandleInteractionResponse(const IncomingResponse& response);

    // 当前活着的 thread 数(测试断言用)。
    std::size_t active_thread_count();

private:
    void RegisterMethods();
    // 整回合驱动(工作线程体):审批/ask_user 悬停、打断旗、终态分型。
    void RunTurnToCompletion(const std::shared_ptr<ThreadRecord>& record, const std::string& thread_id,
                             const std::string& turn_id, const std::string& text);

    // 找一场 thread 的家当(锁内拷 shared_ptr)。
    std::shared_ptr<ThreadRecord> FindThread(const std::string& thread_id);

    ServerOptions options_;
    BackendFactory backend_factory_;
    RegistryFactory registry_factory_;
    std::shared_ptr<Dispatcher> dispatcher_;
    std::unique_ptr<StdioConnection> connection_;

    std::mutex threads_mutex_;
    std::map<std::string, std::shared_ptr<ThreadRecord>> threads_;

    // 会话档目录(选项里给了才有)。
    std::string sessions_dir_;
    // 回合计数(turnId 派生用)。
    std::atomic<std::uint64_t> turn_counter_{0};
};

}  // namespace lubancode::app_server
