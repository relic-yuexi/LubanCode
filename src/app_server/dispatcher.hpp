// app-server 方法路由与握手状态机(纯逻辑,不碰 IO):
//   - initialize/initialized/shutdown 三步握手:握手前调业务法子给稳定
//     错误码(kErrNotInitialized),重复 initialize 给 kErrInvalidRequest;
//   - 方法表:骨架期接线的法子有 handler,留位的法子回
//     kErrMethodNotFound(名字认识也照实回不认识——没接线就是不接线);
//   - handler 异常兜底:任何 handler 抛出都折成 kErrInternalError,一条
//     坏消息不能撞死整台服务。
//
// Handler 协议:吃 params,回出站消息列表(响应 + 该回的事件,次序即
// 发送次序)。不直接写 stdout——发送是 connection/服务循环的事,这里
// 只算"这一条入站消息该产出哪些行"。
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"

namespace lubancode::app_server {

// 连接握手状态。
enum class HandshakeState {
    WaitingInitialize, // 起手:只认 initialize,别的都给 kErrNotInitialized
    WaitingInitialized, // initialize 应答完,等 initialized 通知
    Ready,              // 握手齐了,业务放行
    ShutdownRequested,  // 收到 shutdown:业务一律 kErrShutdownRequested
};

// 一条入站消息的处理产出:零到多条出站 JSON(已序列化的行,不含换行)。
// DispatchOutcome::close_connection 置位时连接层在发完这些行后收线退场。
struct DispatchOutcome {
    std::vector<std::string> outbound;
    bool close_connection = false;
};

// 服务端拿给 handler 的上下文。骨架期就 thread 管理器与回调两样;审批
// 反向请求的执行链(Broker,另一条线)接入时在这里扩挂接点,不动本层。
struct DispatchContext {
    // thread/start 等业务要用的服务句柄(由 server 装配层填)。
    void* service = nullptr;

    // 出事件用(事件经这条路走,handler 不直接碰 outbox——保持 dispatcher
    // 纯逻辑可测)。事件一律 must_keep 分型见 outbox.hpp 的 EventMustKeep。
    std::function<void(std::string_view method, const nlohmann::json& params, bool must_keep)> emit_event;

    // 反向请求响应的落点(阶段 2 接线):前端对 permission/request 或
    // user/ask 的答复走这里,交 server 的悬起件配对。空 = 没接线(骨架
    // 期测试与直驱的形状)。返回值:空串 = 配对成功;否则稳定错误码
    //(kStaleRequestId 一类),由 HandleResponse 折成响应回给前端。
    std::function<std::string(const IncomingResponse& response)> resolve_interaction;
};

// 方法 handler:params 进,响应 JSON(result 或 error 的整条响应信封,
// 含 id)出,外加可选的事件产出(经 emit_event 发)。返回 nullopt 表示
// handler 自己没产出响应(不该发生,防御性兜底交 kErrInternalError)。
using MethodHandler = std::function<std::optional<nlohmann::json>(
    const IncomingRequest& request, DispatchContext& context)>;

// 方法路由器 + 握手状态机。
class Dispatcher {
public:
    Dispatcher();

    // 处理一条折好的入站请求(带 id)。握手规矩在这里守。
    DispatchOutcome HandleRequest(const IncomingRequest& request, DispatchContext& context);

    // 处理一条通知(initialized/exit)。通知没有响应,产出只有事件/关线。
    DispatchOutcome HandleNotification(const IncomingNotification& notification, DispatchContext& context);

    // 服务端反向请求(审批/ask_user)的响应进来时走这里。没接
    // resolve_interaction(骨架期形状)丢弃并打 stderr 诊断;接了就把响
    // 应交悬起件配对,配不上(迟到/失效)回 kErrStaleRequestId——前端
    // 拿它区分"答对了"与"答晚了"。
    DispatchOutcome HandleResponse(const IncomingResponse& response,
                                   DispatchContext& context = kNullContext);

    // 注册方法(装配层用)。同名再注册,后注册的胜。
    void RegisterMethod(std::string_view method, MethodHandler handler);

    HandshakeState state() const { return state_; }

    // 空上下文(HandleResponse 的缺省参数:直驱测试没装配 resolve 回调,
    // 也不该为此造一条假连接)。
    static DispatchContext kNullContext;

    // 装配层给的握手结果拼装(initialize 的 result)。
    void SetInitializeResultFactory(std::function<nlohmann::json()> factory) {
        initialize_result_factory_ = std::move(factory);
    }

private:
    HandshakeState state_ = HandshakeState::WaitingInitialize;
    std::deque<std::string> recent_unknown_methods_; // 诊断:最近不认识的方法名
    std::function<nlohmann::json()> initialize_result_factory_;
    // 方法表:方法名 -> handler。
    std::vector<std::pair<std::string, MethodHandler>> handlers_;

    const MethodHandler* FindHandler(std::string_view method) const;
};

}  // namespace lubancode::app_server
