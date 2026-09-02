// model_request_recovery.hpp 的实现:错误分型 -> 稳定码、白名单判定、
// 抖动阶梯与尝试环。退避等待复用 runtime::WaitBackoffCancellable 的可取消
// 睡(10ms 粒度),用户取消一置位立刻返回 false。
#include "api/model_request_recovery.hpp"

#include <algorithm>
#include <array>
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
    // 五档长退避:低高界内均匀抖动,同时断的多只任务不会齐步拥上去。
    // 表外也钳在最后一档,保证任何单次等待都不越过 2 分钟。
    static constexpr std::array<std::pair<int, int>, 5> kRangesMs{{
        {1000, 2000},
        {4000, 8000},
        {15000, 30000},
        {30000, 60000},
        {60000, 120000},
    }};
    static std::mutex rng_mutex;
    static std::mt19937_64 engine{std::random_device{}()};
    const std::size_t index = static_cast<std::size_t>(std::clamp(attempt, 1, 5) - 1);
    const auto [low_ms, high_ms] = kRangesMs[index];
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
    const auto request_started_at = std::chrono::steady_clock::now();
    const auto emit = [&](RequestAttemptPhase phase) {
        attempt.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - request_started_at);
        if (hooks.on_attempt) {
            hooks.on_attempt(attempt, phase);
        }
    };
    for (attempt.attempt = 1;; ++attempt.attempt) {
        attempt.saw_stream_event = false;
        attempt.saw_headers = false;
        attempt.error_code.clear();
        emit(RequestAttemptPhase::Started);
        auto result = send_once(attempt);
        if (result.has_value()) {
            emit(RequestAttemptPhase::Succeeded);
            return result;
        }
        attempt.error_code = ReasonCodeOfError(result.error());
        const bool user_cancelled =
            result.error().kind == ErrorKind::Cancelled || (cancel != nullptr && cancel->load());
        if (user_cancelled) {
            // 用户停止压过自动恢复(单子不变量 10):立即停,原样交回。
            emit(RequestAttemptPhase::Exhausted);
            return result;
        }
        if (attempt.attempt >= kMaxRequestAttempts || !IsRetryableError(result.error())) {
            emit(RequestAttemptPhase::Exhausted);
            return result;
        }
        emit(RequestAttemptPhase::Retrying);
        const auto wait = BackoffMsForAttempt(attempt.attempt);
        const bool waited = hooks.wait_backoff ? hooks.wait_backoff(wait, cancel)
                                                : runtime::WaitBackoffCancellable(wait, cancel);
        if (!waited) {
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
