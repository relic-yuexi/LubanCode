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

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/session_store.hpp"
#include "api/backend.hpp"
#include "app_server/connection.hpp"
#include "app_server/dispatcher.hpp"
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
    // 场次存档句柄(落盘由它管;失败不拦协议,只打 stderr)。
    std::unique_ptr<agent::SessionStore> store;
};

// 装配选项。
struct ServerOptions {
    std::string sessions_dir;     // 会话档目录(空 = 不落盘,纯内存跑)
    std::string cwd;              // 服务进程当前目录(事件里回给前端)
    std::string lubancode_version = std::string(app::kVersion);
    // 出站队列容量(事件账的有界上限)。
    std::size_t outbox_capacity = 4096;
};

// 一台 app-server。一个进程一台;装配好后 Run() 进 stdio 主循环。
class Server {
public:
    // backend_factory:回合驱动用的 api::Backend(生产是 cpr 后端,测试
    // 注入假 backend)。registry_factory:工具注册表。
    using BackendFactory = std::function<std::unique_ptr<api::Backend>()>;
    using RegistryFactory = std::function<std::unique_ptr<tools::ToolRegistry>()>;

    Server(ServerOptions options, BackendFactory backend_factory, RegistryFactory registry_factory);

    // 装配方法表,跑 stdio 主循环到收线。返回退出码。
    int Run();

    // 单测直驱:注入假连接(假 writer/reader),不起进程、不碰 stdio。
    void AttachForTest(std::unique_ptr<StdioConnection> connection);

    // ---- 以下给单测直驱(不起进程、不碰 stdio) ----

    Dispatcher& dispatcher() { return *dispatcher_; }
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

    // 当前活着的 thread 数(测试断言用)。
    std::size_t active_thread_count();

private:
    void RegisterMethods();

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
