// 浏览器协议服务(内嵌浏览器调试工作台 阶段 3):App Server 的 browser/*
// 方法面与事件族的执行体。C++ 这层是协议转发层——真 Runtime 在 Node
// sidecar(browser/sidecar.js,复用 browser/lib/session.js 的
// BrowserSession,Playwright 生命周期只有那一本账),这里管:
//
//   1. sidecar 进程管理:懒起(首个 browser/* 方法才 spawn)、复用(整台
//      服务一只)、崩了明报(browser/crashed + 在飞动作收口)、收线收尸
//      (Shutdown 杀进程树);
//   2. RPC 配对:stdio JSON-RPC 的 id -> future,超时/进程死/取消都有
//      稳定错误码;取消令贯通到 sidecar 的动作队列(notifications/
//      cancelled,与工具路 P1.6 的取消先例同款);
//   3. 事件转发:sidecar 的 event 通知翻成冻结的 browser/* 协议事件
//      (连接层统一盖 seq);console/network 的批量通知原样转(journal
//      的批量、有帽、丢老明报在 sidecar 源头做,出站队列再兜一层
//      有界 + 合并);
//   4. 审批:owner=agent 的写动作先过 permission/request(宿主可拦可
//      问),decline/cancel/超时按拒绝收口,acceptForSession 按方法名
//      记会话级放行账;
//   5. 截图 artifact:sidecar 回的字节只在内部通道走,这边落
//      artifact(内容寻址)后只发引用,协议上绝不出现 base64。
//
// 线程模型:
//   - 读线程:同步查询(status/list/console|network|downloads query)
//      直答;异步方法受理即回 actionId;
//   - 动作工作线程(一条):排队跑异步动作(审批 -> sidecar 调用 ->
//      终态事件)。一份浏览器状态一位主人,动作串行是仲裁规矩;
//   - sidecar 读线程(StdioTransport 内部):回响应、发事件。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "app_server/dispatcher.hpp"
#include "app_server/protocol.hpp"
#include "mcp/client.hpp"  // mcp::Transport 接口(注入假 sidecar 用)
#include "mcp/transport.hpp"
#include "runtime/interaction_broker.hpp"

namespace lubancode::app_server {

struct ThreadRecord;

// sidecar 调用的结果:成功装 result 对象;失败装稳定码 browser.* 与人话。
struct SidecarCallResult {
    bool ok = false;
    nlohmann::json result = nlohmann::json::object();
    std::string error_code;
    std::string error_message;
    bool cancelled = false;
};

// 装配选项。
struct BrowserServiceOptions {
    // sidecar 启动命令与参数(例:"node" + {"<repo>/browser/sidecar.js"})。
    // command 空 = 没配 browser 面,browser/* 方法回 browser.not_configured。
    std::string sidecar_command;
    std::vector<std::string> sidecar_args;
    // 截图 artifact 落盘目录(内容寻址)。空 = browser/screenshot 回
    // browser.artifact_unavailable(不冒充可取)。
    std::string artifact_dir;
    // 同步查询等 sidecar 回执的硬时限(毫秒)。
    int query_timeout_ms = 20000;
    // 异步动作等 sidecar 终态的硬上限(毫秒;含审批之外的一段)。
    int action_deadline_ms = 90000;
    // 取消后的宽限:sidecar 收到 cancelled 通知后多久没终态就按取消收口。
    int cancel_grace_ms = 3000;
};

// 一只在飞的异步浏览器动作。
struct BrowserAction {
    std::string action_id;      // "br-<n>"(ProcessIdAuthority 发号)
    std::string method;         // 协议方法名(browser/page/open 等)
    std::string sidecar_method; // 折算后的 sidecar 方法名(page/open 等)
    std::string owner;          // "agent" | "user"
    std::string thread_id;      // owner=agent 必有(审批挂这场上)
    nlohmann::json input;       // 协议侧参数摘要(action/started 与审批展示用)
    nlohmann::json sidecar_params; // 折算后的 sidecar 请求参数
    bool needs_approval = false;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};
    std::atomic<std::int64_t> sidecar_request_id{0}; // >0 = sidecar 调用已发出
    std::chrono::steady_clock::time_point started_at;
};

class BrowserService {
public:
    // 事件出水口(装配层给:connection_->EmitEvent 的薄封)。
    using EventSink = std::function<void(std::string_view method, const nlohmann::json& params)>;

    // 审批票:future 悬着等;cancel 把这枚悬起件收口(迟到答案按 stale
    // 报,不吞)。nullopt = 没有这条 thread(动作按 browser.unknown_thread
    // 收口)。实现方(Server)把取消旗挂进 future 的打断路(WatchInterrupt),
    // browser/action/cancel、thread/stop 都能从这儿把悬着的审批叫醒。
    struct ApprovalTicket {
        std::shared_ptr<runtime::InteractionFuture> future;
        std::function<void()> cancel;
    };
    using ApprovalAsk =
        std::function<std::optional<ApprovalTicket>(const std::string& thread_id,
                                                    const runtime::ApprovalRequest& request,
                                                    const std::atomic<bool>* cancel)>;

    BrowserService(BrowserServiceOptions options, EventSink sink);
    ~BrowserService();

    BrowserService(const BrowserService&) = delete;
    BrowserService& operator=(const BrowserService&) = delete;

    // 审批口(装配层给;不给 = owner=agent 的动作一律按无审批路拒绝,
    // 缺省 owner=user 不受影响)。
    void SetApprovalAsk(ApprovalAsk ask) { approval_ask_ = std::move(ask); }

    // 把 browser/* 方法挂进 dispatcher。find_thread:threadId -> ThreadRecord
    // 的查找口(审批要用;装配层给 Server::FindThread 的薄封)。
    void RegisterMethods(Dispatcher& dispatcher,
                         const std::function<std::shared_ptr<ThreadRecord>(const std::string&)>& find_thread);

    // 停场/打断:取消某 thread 名下在飞的浏览器动作(审批悬着也叫醒)。
    std::size_t CancelActionsForThread(const std::string& thread_id, const std::string& reason);

    // 收线收尸:取消全部动作、冲事件、杀 sidecar 进程树。
    void Shutdown();

    // ---- 测试口 ----
    // 注入假 sidecar(不真起进程;调用方保 transport 生命周期)。注入后
    // EnsureSidecar 不再 spawn,直接用这只。
    void AttachTransportForTest(mcp::Transport* transport);
    // 喂一行"来自 sidecar"的协议行(假 transport 的驱动口;真路径由
    // StdioTransport 的读线程调)。
    void OnSidecarLine(const std::string& line);
    // 在飞动作数(断言用)。
    std::size_t active_action_count();
    // sidecar 起过几只(进程复用/重起的断言用)。
    int sidecar_spawn_count() const { return spawn_count_.load(); }
    // 测试:模拟 sidecar 崩溃(直接杀进程,不走 shutdown 钩子)。下一笔
    // 调用会自动重起。
    void KillSidecarForTest();
    // 测试直驱:同步调一笔 sidecar 请求(真进程用例的直驱口)。
    SidecarCallResult CallForTest(const std::string& method, const nlohmann::json& params, int timeout_ms) {
        return Call(method, params, timeout_ms);
    }

private:
    // ---- sidecar 进程与 RPC ----

    // 确保 sidecar 活着(懒起;测试注入的 transport 除外)。失败给
    // browser.not_configured / browser.sidecar_spawn_failed。
    SidecarCallResult EnsureSidecar();

    // 发一次 sidecar 请求并等回执。cancel 非空时:旗置位即发 cancelled
    // 通知并进宽限等待,宽限内等不到终态按取消收口。timeout_ms 到点按
    // browser.sidecar_timeout 收口。进程死(browser.sidecar_dead)与回执
    // 错误(data.browserCode)都折进 SidecarCallResult。
    SidecarCallResult Call(const std::string& method, const nlohmann::json& params, int timeout_ms,
                           const std::atomic<bool>* cancel = nullptr,
                           std::atomic<std::int64_t>* request_id_out = nullptr);

    // sidecar 事件通知的处理(见文件头 3)。
    void HandleSidecarEvent(const nlohmann::json& params);

    // sidecar 死了:失败全部在飞 RPC、发 browser/crashed、标记进程待重起。
    void HandleSidecarGone(const std::string& reason);

    // ---- 动作管线 ----

    // 受理一只动作:登记、排队、回受理响应(result 对象)。
    nlohmann::json AcceptAction(const std::string& method, const std::string& sidecar_method, nlohmann::json input,
                                nlohmann::json sidecar_params, bool needs_approval);
    void ActionWorkerLoop();
    void RunAction(const std::shared_ptr<BrowserAction>& action);

    // 截图:字节落 artifact、发 browser/screenshot/ready,返回带引用的
    // result(不含 base64)。
    nlohmann::json FinishScreenshot(const nlohmann::json& sidecar_result, std::string& out_error_code,
                                    std::string& out_error_message);

    void Emit(std::string_view method, const nlohmann::json& params);

    BrowserServiceOptions options_;
    EventSink sink_;
    ApprovalAsk approval_ask_;

    // ---- sidecar 传输 ----
    mcp::Transport* attached_transport_ = nullptr; // 测试注入(不持有)
    std::unique_ptr<mcp::StdioTransportAdapter> owned_transport_;
    std::atomic<bool> sidecar_alive_{false};
    std::atomic<bool> shutting_down_{false};

    // ---- RPC 配对账 ----
    struct PendingCall {
        std::promise<nlohmann::json> promise;
    };
    std::mutex rpc_mutex_;
    std::condition_variable rpc_cv_;
    std::map<std::int64_t, std::shared_ptr<PendingCall>> pending_calls_;
    std::atomic<std::int64_t> next_request_id_{1};

    // ---- 动作账 ----
    std::mutex actions_mutex_;
    std::map<std::string, std::shared_ptr<BrowserAction>> actions_;
    std::deque<std::shared_ptr<BrowserAction>> action_queue_;
    std::thread action_worker_;
    std::condition_variable action_cv_;
    std::atomic<std::uint64_t> next_action_seq_{0};
    std::atomic<int> spawn_count_{0};
};

}  // namespace lubancode::app_server
