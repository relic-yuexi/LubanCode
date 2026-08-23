// dispatcher.hpp 的实现:握手状态机 + 方法路由 + 异常兜底。
#include "app_server/dispatcher.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace lubancode::app_server {

namespace {

// stderr 诊断(app-server 模块的 stdout 纪律:除协议行外一个 std::cout
// 都不许有,诊断一律 stderr)。头文件不许带 <iostream>,实现里带。
void Diagnose(const std::string& text) {
    std::fprintf(stderr, "[app-server] %s\n", text.c_str());
}

// resolve_interaction 回报的错误码记号:参数错走 kErrInvalidParams,
// 其余(迟到/失效)一律 kErrStaleRequestId。
constexpr const char* kInvalidParamsToken = "invalid_params";

}  // namespace

Dispatcher::Dispatcher() = default;

const MethodHandler* Dispatcher::FindHandler(std::string_view method) const {
    for (const auto& [name, handler] : handlers_) {
        if (name == method) {
            return &handler;
        }
    }
    return nullptr;
}

void Dispatcher::RegisterMethod(std::string_view method, MethodHandler handler) {
    const std::string key(method);
    for (auto& [name, existing] : handlers_) {
        if (name == key) {
            existing = std::move(handler);
            return;
        }
    }
    handlers_.emplace_back(key, std::move(handler));
}

DispatchOutcome Dispatcher::HandleRequest(const IncomingRequest& request, DispatchContext& context) {
    DispatchOutcome outcome;

    // 握手规矩:initialize 只许在 WaitingInitialize 收一次;其余业务在
    // Ready 之前一律稳定错误码。
    if (request.method == kMethodInitialize) {
        if (state_ != HandshakeState::WaitingInitialize) {
            outcome.outbound.push_back(
                SerializeMessage(MakeError(request.id, kErrInvalidRequest, "重复 initialize")));
            return outcome;
        }
        const ParamsCheck check = CheckInitializeParams(request.params);
        if (!check.ok) {
            outcome.outbound.push_back(SerializeMessage(MakeError(request.id, check.code, check.message)));
            return outcome;
        }
        nlohmann::json result = initialize_result_factory_ ? initialize_result_factory_()
                                                           : nlohmann::json::object();
        outcome.outbound.push_back(SerializeMessage(MakeResult(request.id, std::move(result))));
        state_ = HandshakeState::WaitingInitialized;
        return outcome;
    }

    if (state_ == HandshakeState::WaitingInitialize || state_ == HandshakeState::WaitingInitialized) {
        // 未握手就调业务:稳定错误码。shutdown 也算业务——握手没齐,退场
        // 也不给走协议路(连接层自己会因 EOF/exit 收线)。
        outcome.outbound.push_back(SerializeMessage(
            MakeError(request.id, kErrNotInitialized, "未 initialize,先握手(" + request.method + ")")));
        return outcome;
    }

    if (state_ == HandshakeState::ShutdownRequested) {
        outcome.outbound.push_back(
            SerializeMessage(MakeError(request.id, kErrShutdownRequested, "已 shutdown,不再受理请求")));
        return outcome;
    }

    // shutdown 是协议自带法子(不占方法表):响应发完置 ShutdownRequested
    // 并收线。exit 走通知那条路(立即收线、不回话)。
    if (request.method == kMethodShutdown) {
        outcome.outbound.push_back(SerializeMessage(MakeResult(request.id, nlohmann::json::object())));
        state_ = HandshakeState::ShutdownRequested;
        outcome.close_connection = true;
        return outcome;
    }

    const MethodHandler* handler = FindHandler(request.method);
    if (handler == nullptr) {
        recent_unknown_methods_.push_back(request.method);
        if (recent_unknown_methods_.size() > 16) {
            recent_unknown_methods_.pop_front();
        }
        outcome.outbound.push_back(
            SerializeMessage(MakeError(request.id, kErrMethodNotFound, "未知方法: " + request.method)));
        return outcome;
    }

    // handler 兜底:任何异常都折成稳定错误码,一条坏消息不能撞死整台服务。
    try {
        const std::optional<nlohmann::json> response = (*handler)(request, context);
        if (!response.has_value()) {
            outcome.outbound.push_back(SerializeMessage(
                MakeError(request.id, kErrInternalError, "handler 未产出响应: " + request.method)));
            return outcome;
        }
        outcome.outbound.push_back(SerializeMessage(*response));
    } catch (const std::exception& error) {
        Diagnose(std::string("handler 异常: ") + request.method + ": " + error.what());
        outcome.outbound.push_back(
            SerializeMessage(MakeError(request.id, kErrInternalError, "内部错误: " + request.method)));
    } catch (...) {
        Diagnose("handler 抛出未知异常: " + request.method);
        outcome.outbound.push_back(
            SerializeMessage(MakeError(request.id, kErrInternalError, "内部错误: " + request.method)));
    }
    return outcome;
}

DispatchOutcome Dispatcher::HandleNotification(const IncomingNotification& notification,
                                               DispatchContext&) {
    DispatchOutcome outcome;
    if (notification.method == kMethodInitialized) {
        if (state_ == HandshakeState::WaitingInitialized) {
            state_ = HandshakeState::Ready;
        }
        // 重复 initialized:无害,吃下不报(通知没有响应可回)。
        return outcome;
    }
    if (notification.method == kMethodExit) {
        // 明确退场:立即收线。在跑回合的打断/冲刷由连接层在收线前做
        // (骨架期没有在跑回合的并行场景,直接退)。
        outcome.close_connection = true;
        return outcome;
    }
    // 其它通知:认识不认识都不回话(通知没有响应),不认识的打一笔
    // stderr 诊断了事。
    Diagnose("忽略通知: " + notification.method);
    return outcome;
}

DispatchContext Dispatcher::kNullContext{};

DispatchOutcome Dispatcher::HandleResponse(const IncomingResponse& response, DispatchContext& context) {
    // 反向请求的响应(审批/ask_user 的前端答复):交悬起件配对。
    if (!context.resolve_interaction) {
        // 没接线(骨架期直驱的形状):丢弃 + 诊断。
        Diagnose("收到反向请求响应,但 resolve 回调未装配,id=" + std::to_string(response.id));
        return DispatchOutcome{};
    }
    const std::string error = context.resolve_interaction(response);
    if (error.empty()) {
        // 配对成功:回一条空 result,前端好收账。
        DispatchOutcome outcome;
        outcome.outbound.push_back(SerializeMessage(MakeResult(response.id, nlohmann::json::object())));
        return outcome;
    }
    // 配不上:迟到/失效,稳定错误码;参数错(kErrInvalidParams)是报文
    // 形状不对,不是迟到。
    const int code = error == std::string(kInvalidParamsToken)
                         ? kErrInvalidParams
                         : kErrStaleRequestId;
    DispatchOutcome outcome;
    outcome.outbound.push_back(SerializeMessage(MakeError(response.id, code, "反向请求响应: " + error)));
    return outcome;
}

}  // namespace lubancode::app_server
