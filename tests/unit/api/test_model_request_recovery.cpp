// 请求级恢复(监督器单 P0-1)的单元测试:错误分型 -> 稳定码、重试白名单、
// 抖动阶梯边界、尝试环的各路收口(可重试->重发->成功;不可重试->立即收口;
// 重试用尽;用户取消压过重试;退避等待中可打断)。恢复环用假的 AttemptSender
// 驱动,不碰真网络;经 AgentLoop 的合同测试(半截 text/半截 tool JSON/工具
// 只跑一次)在 test_agent_recovery.cpp。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "api/model_request_recovery.hpp"
#include "api/types.hpp"

using namespace lubancode;

namespace {

// 脚本化的假发送:第 N 次调用返回脚本里的第 N 个结果,顺手记 attempt 账。
class ScriptedSender {
public:
    std::vector<std::expected<void, api::Error>> script;
    std::vector<int> attempts_started;
    std::vector<api::RequestAttemptPhase> phases;
    int calls = 0;

    api::AttemptSender Sender() {
        return [this](api::ModelRequestAttempt& attempt) -> std::expected<void, api::Error> {
            const std::size_t idx = static_cast<std::size_t>(calls++);
            attempts_started.push_back(attempt.attempt);
            if (idx >= script.size()) {
                return std::unexpected(api::Error{api::ErrorKind::Api, "脚本用完了", 0});
            }
            return script[idx];
        };
    }
};

api::RequestRecoveryHooks RecordingHooks(ScriptedSender& sender) {
    api::RequestRecoveryHooks hooks;
    hooks.on_attempt = [&sender](const api::ModelRequestAttempt& attempt, api::RequestAttemptPhase phase) {
        (void)attempt;
        sender.phases.push_back(phase);
    };
    return hooks;
}

}  // namespace

TEST_CASE("稳定错误码:网络/HTTP/取消各归各,HTTP 带码") {
    CHECK(api::ReasonCodeOfError(api::Error{api::ErrorKind::Network, "x", 0}) == "network.error");
    CHECK(api::ReasonCodeOfError(api::Error{api::ErrorKind::HttpStatus, "x", 429}) == "http.429");
    CHECK(api::ReasonCodeOfError(api::Error{api::ErrorKind::Parse, "x", 0}) == "protocol.parse");
    CHECK(api::ReasonCodeOfError(api::Error{api::ErrorKind::Api, "x", 0}) == "api.error");
    CHECK(api::ReasonCodeOfError(api::Error{api::ErrorKind::Cancelled, "x", 0}) == "cancelled");
}

TEST_CASE("重试白名单:瞬时网络错与 408/429/502/503/504 可重发,其余立即收口") {
    CHECK(api::IsRetryableError(api::Error{api::ErrorKind::Network, "connect reset", 0}));
    for (const int status : {408, 429, 502, 503, 504}) {
        CHECK(api::IsRetryableError(api::Error{api::ErrorKind::HttpStatus, "x", status}));
    }
    for (const int status : {400, 401, 403, 404, 500}) {
        CHECK_FALSE(api::IsRetryableError(api::Error{api::ErrorKind::HttpStatus, "x", status}));
    }
    CHECK_FALSE(api::IsRetryableError(api::Error{api::ErrorKind::Parse, "x", 0}));
    CHECK_FALSE(api::IsRetryableError(api::Error{api::ErrorKind::Api, "x", 0}));
    CHECK_FALSE(api::IsRetryableError(api::Error{api::ErrorKind::Cancelled, "x", 0}));
}

TEST_CASE("抖动阶梯:第 1 次失败等 250~750ms,第 2 次等 1~2s") {
    for (int i = 0; i < 20; ++i) {
        const auto first = api::BackoffMsForAttempt(1).count();
        CHECK(first >= 250);
        CHECK(first <= 750);
        const auto second = api::BackoffMsForAttempt(2).count();
        CHECK(second >= 1000);
        CHECK(second <= 2000);
    }
}

TEST_CASE("恢复环:网络错一次后重发成功,attempt 连号且 Retrying 相位在案") {
    ScriptedSender sender;
    sender.script = {
        std::unexpected(api::Error{api::ErrorKind::Network, "connect reset", 0}),
        std::expected<void, api::Error>{},
    };
    const auto result = api::RunRequestWithRecovery(sender.Sender(), RecordingHooks(sender), nullptr);
    REQUIRE(result.has_value());
    REQUIRE(sender.attempts_started.size() == 2);
    CHECK(sender.attempts_started[0] == 1);
    CHECK(sender.attempts_started[1] == 2);
    bool saw_retrying = false;
    for (const auto phase : sender.phases) {
        if (phase == api::RequestAttemptPhase::Retrying) {
            saw_retrying = true;
        }
    }
    CHECK(saw_retrying);
}

TEST_CASE("恢复环:不可重试错误立即收口,不退避不重发") {
    ScriptedSender sender;
    sender.script = {
        std::unexpected(api::Error{api::ErrorKind::HttpStatus, "bad request", 400}),
    };
    const auto result = api::RunRequestWithRecovery(sender.Sender(), RecordingHooks(sender), nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().http_status == 400);
    CHECK(sender.calls == 1);
}

TEST_CASE("恢复环:重试用尽(3 次)后按最后一错收口") {
    ScriptedSender sender;
    sender.script = {
        std::unexpected(api::Error{api::ErrorKind::Network, "reset", 0}),
        std::unexpected(api::Error{api::ErrorKind::Network, "reset", 0}),
        std::unexpected(api::Error{api::ErrorKind::HttpStatus, "overloaded", 503}),
    };
    const auto result = api::RunRequestWithRecovery(sender.Sender(), RecordingHooks(sender), nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().http_status == 503);
    CHECK(sender.calls == 3);
    CHECK(sender.attempts_started.size() == 3);
}

TEST_CASE("恢复环:用户取消压过重试,立即原样交回") {
    ScriptedSender sender;
    sender.script = {
        std::unexpected(api::Error{api::ErrorKind::Network, "reset", 0}),
        std::unexpected(api::Error{api::ErrorKind::Cancelled, "用户按 ESC 打断了这次请求", 0}),
    };
    const auto result = api::RunRequestWithRecovery(sender.Sender(), RecordingHooks(sender), nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == api::ErrorKind::Cancelled);
    CHECK(sender.calls == 2);  // 取消那次不再重试
}

TEST_CASE("恢复环:退避等待中被取消,合成 Cancelled 收口") {
    ScriptedSender sender;
    sender.script = {
        std::unexpected(api::Error{api::ErrorKind::Network, "reset", 0}),
    };
    std::atomic<bool> cancel{false};
    // 200ms 后置取消:第 1 次失败的退避(250~750ms)还没睡完就被打断。
    std::thread canceller([&cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel.store(true);
    });
    const auto result = api::RunRequestWithRecovery(sender.Sender(), RecordingHooks(sender), &cancel);
    canceller.join();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == api::ErrorKind::Cancelled);
    CHECK(sender.calls == 1);  // 没有发出第二次
}

TEST_CASE("历史提交边界凭据:同请求同哈希,消息变了就变") {
    api::Request request;
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"问题"});
    request.messages.push_back(user);
    const std::string hash_a = api::HistoryCommitHashOf(request);
    CHECK(api::HistoryCommitHashOf(request) == hash_a);  // 稳定
    api::Message reply;
    reply.role = api::Role::Assistant;
    reply.content.push_back(api::TextBlock{"回答"});
    request.messages.push_back(reply);
    CHECK(api::HistoryCommitHashOf(request) != hash_a);
    CHECK(hash_a.size() == 16);
}
