// model_request_recovery.hpp 的实现:错误分型 -> 稳定码、白名单判定、
// 抖动阶梯与尝试环。退避等待复用 runtime::WaitBackoffCancellable 的可取消
// 睡(10ms 粒度),用户取消一置位立刻返回 false。
#include "api/model_request_recovery.hpp"

#include <mutex>
#include <random>
#include <type_traits>
#include <variant>

#include "hooks/hash.hpp"  // Sha256Hex:边界凭据只留短哈希
#include "runtime/retry_backoff.hpp"  // WaitBackoffCancellable:可取消等待

namespace lubancode::api {

std::string HistoryCommitHashOf(const Request& request) {
    std::string material;
    material.reserve(request.messages.size() * 16);
    for (const auto& message : request.messages) {
        material += std::to_string(static_cast<int>(message.role));
        material.push_back(':');
        std::size_t bytes = 0;
        for (const auto& block : message.content) {
            bytes += std::visit(
                [](const auto& b) -> std::size_t {
                    using T = std::decay_t<decltype(b)>;
                    if constexpr (std::is_same_v<T, TextBlock>) {
                        return b.text.size();
                    } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                        return b.text.size() + b.signature.size();
                    } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                        return b.id.size() + b.name.size() + b.input.dump().size();
                    } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                        return b.tool_use_id.size() + b.content.size();
                    } else if constexpr (std::is_same_v<T, ImageBlock>) {
                        return b.data.size();
                    } else {
                        return 0;
                    }
                },
                block);
        }
        material += std::to_string(bytes);
        material.push_back(';');
    }
    return hooks::Sha256Hex(material).substr(0, 16);
}

std::string ReasonCodeOfError(const Error& error) {
    switch (error.kind) {
        case ErrorKind::Network:
            // 网络类不再细分文案:连接类/空闲类的原文留在 message 里给人看,
            // 稳定码只认一层(network.*)。
            return "network.error";
        case ErrorKind::HttpStatus:
            return "http." + std::to_string(error.http_status);
        case ErrorKind::Parse:
            return "protocol.parse";
        case ErrorKind::Api:
            return "api.error";
        case ErrorKind::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

bool IsRetryableError(const Error& error) {
    switch (error.kind) {
        case ErrorKind::Network:
            // DNS/连接被拒/复位/TLS 瞬断/首字节前超时/流空闲超时都归
            // Network(见 ClassifyNetworkError)。流空闲重发之所以安全:
            // assistant 消息只在 send_stream 归队后才提交,断流时永不落地。
            return true;
        case ErrorKind::HttpStatus:
            return error.http_status == 408 || error.http_status == 429 || error.http_status == 502 ||
                   error.http_status == 503 || error.http_status == 504;
        case ErrorKind::Parse:
        case ErrorKind::Api:
        case ErrorKind::Cancelled:
            return false;
    }
    return false;
}

std::chrono::milliseconds BackoffMsForAttempt(int attempt) {
    // 单子 §8.2 的阶梯:1 -> 250~750ms,2 -> 1~2s,3 -> 收口。均匀抖动把
    // 同时断的多只任务错开,不齐步拥上去。
    static std::mutex rng_mutex;
    static std::mt19937_64 engine{std::random_device{}()};
    int low_ms = 0;
    int high_ms = 0;
    if (attempt <= 1) {
        low_ms = 250;
        high_ms = 750;
    } else {
        low_ms = 1000;
        high_ms = 2000;
    }
    std::lock_guard<std::mutex> lock(rng_mutex);
    std::uniform_int_distribution<int> dist(low_ms, high_ms);
    return std::chrono::milliseconds(dist(engine));
}

namespace {

std::string NextLogicalRequestId() {
    // 逻辑请求号:进程内单调递增,拼一条足够对账的稳定 id。不用时间戳——
    // 多线程同毫秒会撞,单调计数天然唯一。
    static std::mutex id_mutex;
    static std::uint64_t next_id = 0;
    std::lock_guard<std::mutex> lock(id_mutex);
    return "req-" + std::to_string(++next_id);
}

}  // namespace

std::expected<void, Error> RunRequestWithRecovery(const AttemptSender& send_once,
                                                  const RequestRecoveryHooks& hooks,
                                                  const std::atomic<bool>* cancel) {
    ModelRequestAttempt attempt;
    attempt.logical_request_id = NextLogicalRequestId();
    for (attempt.attempt = 1;; ++attempt.attempt) {
        attempt.saw_stream_event = false;
        attempt.saw_headers = false;
        attempt.error_code.clear();
        if (hooks.on_attempt) {
            hooks.on_attempt(attempt, RequestAttemptPhase::Started);
        }
        auto result = send_once(attempt);
        if (result.has_value()) {
            if (hooks.on_attempt) {
                hooks.on_attempt(attempt, RequestAttemptPhase::Succeeded);
            }
            return result;
        }
        attempt.error_code = ReasonCodeOfError(result.error());
        const bool user_cancelled =
            result.error().kind == ErrorKind::Cancelled || (cancel != nullptr && cancel->load());
        if (user_cancelled) {
            // 用户停止压过自动恢复(单子不变量 10):立即停,原样交回。
            if (hooks.on_attempt) {
                hooks.on_attempt(attempt, RequestAttemptPhase::Exhausted);
            }
            return result;
        }
        if (attempt.attempt >= kMaxRequestAttempts || !IsRetryableError(result.error())) {
            if (hooks.on_attempt) {
                hooks.on_attempt(attempt, RequestAttemptPhase::Exhausted);
            }
            return result;
        }
        if (hooks.on_attempt) {
            hooks.on_attempt(attempt, RequestAttemptPhase::Retrying);
        }
        if (!runtime::WaitBackoffCancellable(BackoffMsForAttempt(attempt.attempt), cancel)) {
            // 退避中被取消:按取消收口,不让"停了还在重试"穿透出去。
            return std::unexpected(Error{ErrorKind::Cancelled, "重试等待中被取消", 0});
        }
        if (cancel != nullptr && cancel->load()) {
            return std::unexpected(Error{ErrorKind::Cancelled, "重试等待中被取消", 0});
        }
    }
}

std::expected<void, Error> SendStreamWithRecovery(Backend& backend, const Request& request,
                                                  const std::function<void(const StreamEvent&)>& on_event,
                                                  const RequestRecoveryHooks& hooks,
                                                  const std::atomic<bool>* cancel) {
    return RunRequestWithRecovery(
        [&backend, &request, &on_event, cancel](ModelRequestAttempt& attempt) {
            return backend.send_stream(
                request,
                [&on_event, &attempt](const StreamEvent& event) {
                    // 首枚流事件即"到过字节"的旁证:恢复环每次尝试前重置,
                    // 这里见第一枚就记。
                    if (!attempt.saw_stream_event) {
                        attempt.saw_stream_event = true;
                        attempt.saw_headers = true;
                    }
                    on_event(event);
                },
                cancel);
        },
        hooks, cancel);
}

}  // namespace lubancode::api
