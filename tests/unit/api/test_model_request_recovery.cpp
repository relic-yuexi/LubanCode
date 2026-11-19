// 请求级恢复(监督器单 P0-1)的单元测试:错误分型 -> 稳定码、重试白名单、
// 抖动阶梯边界、尝试环的各路收口(可重试->重发->成功;不可重试->立即收口;
// 重试用尽;用户取消压过重试;退避等待中可打断)。恢复环用假的 AttemptSender
// 驱动,不碰真网络;经 AgentLoop 的合同测试(半截 text/半截 tool JSON/工具
// 只跑一次)在 test_agent_recovery.cpp。

#include <doctest/doctest.h>

#include <array>
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
    std::vector<std::chrono::milliseconds> waits;
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
    hooks.wait_backoff = [&sender](std::chrono::milliseconds wait, const std::atomic<bool>* cancel) {
        sender.waits.push_back(wait);
        return cancel == nullptr || !cancel->load();
    };
    return hooks;
}

}  // namespace

TEST_CASE("稳定错误码:网络/HTTP/取消各归各,HTTP 带码") {
    CHECK(api::ReasonCodeOfError(api::Error{api::ErrorKind::Network, "x", 0}) == "network.error");
    CHECK(api::ReasonCodeOfError(api::Error{api::ErrorKind::HttpStatus, "x", 429}) == "http.429");
    CHECK(api::ReasonCodeOfError(api::Error{api::ErrorKind::Parse, "x", 0}) == "protocol.parse");
    CHECK(api::ReasonCodeOfError(api::Error{api::ErrorKind::Api, "x", 0}) == "api.error");
    CHECK(api::ReasonCodeOfError(api::Error{api::ErrorKind::Api, "x", 0, "upstream_error"}) ==
          "api.upstream_error");
    CHECK(api::ReasonCodeOfError(api::Error{api::ErrorKind::Cancelled, "x", 0}) == "cancelled");
}

TEST_CASE("重试白名单:瞬时网络错、指定 HTTP 状态与瞬时 Api code 可重发") {
    CHECK(api::IsRetryableError(api::Error{api::ErrorKind::Network, "connect reset", 0}));
    for (const int status : {408, 429, 500, 502, 503, 504}) {
        CHECK(api::IsRetryableError(api::Error{api::ErrorKind::HttpStatus, "x", status}));
    }
    for (const int status : {400, 401, 403, 404, 501}) {
        CHECK_FALSE(api::IsRetryableError(api::Error{api::ErrorKind::HttpStatus, "x", status}));
    }
    for (const std::string code : {"upstream_error", "server_error", "overloaded_error", "overloaded",
                                    "model_overloaded", "internal_error", "internal_server_error"}) {
        CHECK(api::IsRetryableError(api::Error{api::ErrorKind::Api, "x", 0, code}));
    }
    for (const std::string code : {"", "authentication_error", "invalid_request", "not_found", "permission"}) {
        CHECK_FALSE(api::IsRetryableError(api::Error{api::ErrorKind::Api, "x", 0, code}));
    }
    CHECK_FALSE(api::IsRetryableError(api::Error{api::ErrorKind::Parse, "x", 0}));
    CHECK_FALSE(api::IsRetryableError(api::Error{api::ErrorKind::Cancelled, "x", 0}));
}

TEST_CASE("恢复环:500 与 upstream_error 都会用满预算,任一后续成功即恢复") {
    for (const api::Error& error : {
             api::Error{api::ErrorKind::HttpStatus, "server failed", 500},
             api::Error{api::ErrorKind::Api, "Upstream request failed", 0, "upstream_error"},
         }) {
        SUBCASE(error.kind == api::ErrorKind::HttpStatus ? "HTTP 500" : "Api upstream_error") {
            ScriptedSender exhausted;
            exhausted.script.assign(api::kMaxRequestAttempts, std::unexpected(error));
            const auto failed = api::RunRequestWithRecovery(exhausted.Sender(), RecordingHooks(exhausted));
            REQUIRE_FALSE(failed.has_value());
            CHECK(exhausted.calls == api::kMaxRequestAttempts);
            CHECK(exhausted.waits.size() == api::kMaxRequestAttempts - 1);

            ScriptedSender recovered;
            recovered.script = {std::unexpected(error), {}};
            const auto succeeded = api::RunRequestWithRecovery(recovered.Sender(), RecordingHooks(recovered));
            REQUIRE(succeeded.has_value());
            CHECK(recovered.calls == 2);
            CHECK(recovered.waits.size() == 1);
        }
    }
}

TEST_CASE("恢复环:400/401 与确定性 Api code 零重试") {
    for (const api::Error& error : {
             api::Error{api::ErrorKind::HttpStatus, "bad request", 400},
             api::Error{api::ErrorKind::HttpStatus, "unauthorized", 401},
             api::Error{api::ErrorKind::Api, "invalid", 0, "invalid_request"},
             api::Error{api::ErrorKind::Api, "denied", 0, "authentication_error"},
         }) {
        ScriptedSender sender;
        sender.script = {std::unexpected(error), {}};
        const auto result = api::RunRequestWithRecovery(sender.Sender(), RecordingHooks(sender));
        REQUIRE_FALSE(result.has_value());
        CHECK(sender.calls == 1);
        CHECK(sender.waits.empty());
    }
}

TEST_CASE("抖动阶梯:五档指数范围且单次以 2 分钟封顶") {
    constexpr std::array<std::pair<long long, long long>, 5> ranges{{
        {1000, 2000}, {4000, 8000}, {15000, 30000}, {30000, 60000}, {60000, 120000}}};
    for (int attempt = 1; attempt <= 5; ++attempt) {
        for (int sample = 0; sample < 20; ++sample) {
            const auto wait = api::BackoffMsForAttempt(attempt).count();
            CHECK(wait >= ranges[attempt - 1].first);
            CHECK(wait <= ranges[attempt - 1].second);
        }
    }
    CHECK(api::BackoffMsForAttempt(99).count() <= 120000);
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

TEST_CASE("恢复环:401 确定性错误零重试零等待") {
    ScriptedSender sender;
    sender.script = {
        std::unexpected(api::Error{api::ErrorKind::HttpStatus, "unauthorized", 401}),
    };
    const auto result = api::RunRequestWithRecovery(sender.Sender(), RecordingHooks(sender), nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().http_status == 401);
    CHECK(sender.calls == 1);
    CHECK(sender.waits.empty());
}

TEST_CASE("恢复环:fake clock 钉住五段等待,第 6 次尝试后收口") {
    ScriptedSender sender;
    sender.script.assign(api::kMaxRequestAttempts,
                         std::unexpected(api::Error{api::ErrorKind::Network, "reset", 0}));
    const auto result = api::RunRequestWithRecovery(sender.Sender(), RecordingHooks(sender), nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(sender.calls == 6);
    CHECK(sender.attempts_started == std::vector<int>{1, 2, 3, 4, 5, 6});
    REQUIRE(sender.waits.size() == 5);
    constexpr std::array<std::pair<long long, long long>, 5> ranges{{
        {1000, 2000}, {4000, 8000}, {15000, 30000}, {30000, 60000}, {60000, 120000}}};
    for (std::size_t i = 0; i < sender.waits.size(); ++i) {
        CHECK(sender.waits[i].count() >= ranges[i].first);
        CHECK(sender.waits[i].count() <= ranges[i].second);
    }
}

// ---------------------------------------------------------------------------
// 流中途断/无进展墙落锤的重试封顶(Terminal-Bench 超时单)
// ---------------------------------------------------------------------------

namespace {

api::Error StreamBroken() {
    api::Error error{api::ErrorKind::Network, "stream idle timeout", 0};
    error.stream_started = true;
    return error;
}

api::Error WallHit() {
    api::Error error{api::ErrorKind::Network, "hard timeout", 0};
    error.hard_timeout = true;
    return error;
}

}  // namespace

TEST_CASE("代价高失败判据:只认 Network 且流已开始或墙落锤") {
    CHECK(api::IsCostlyNetworkFailure(StreamBroken(), false));
    CHECK(api::IsCostlyNetworkFailure(WallHit(), false));
    // 恢复环旁证:本次尝试见过流事件,传输层没留旁证也算。
    CHECK(api::IsCostlyNetworkFailure(api::Error{api::ErrorKind::Network, "reset", 0}, true));
    // 连接前失败:照旧不算。
    CHECK_FALSE(api::IsCostlyNetworkFailure(api::Error{api::ErrorKind::Network, "connect refused", 0}, false));
    // 429/5xx/Api 码哪怕见过流事件也不算——不动它们的既有阶梯。
    CHECK_FALSE(api::IsCostlyNetworkFailure(api::Error{api::ErrorKind::HttpStatus, "x", 429}, true));
    CHECK_FALSE(api::IsCostlyNetworkFailure(api::Error{api::ErrorKind::HttpStatus, "x", 503}, true));
    CHECK_FALSE(api::IsCostlyNetworkFailure(api::Error{api::ErrorKind::Api, "x", 0, "upstream_error"}, true));
    // 封顶不改白名单:两类代价高失败仍是可重试错。
    CHECK(api::IsRetryableError(StreamBroken()));
    CHECK(api::IsRetryableError(WallHit()));
}

TEST_CASE("重发判据:代价高失败累计超过封顶即收口,其余照旧") {
    for (int costly = 0; costly <= api::kMaxCostlyFailureRetries; ++costly) {
        CHECK(api::ShouldRetryAfterFailure(StreamBroken(), 1, costly));
    }
    CHECK_FALSE(api::ShouldRetryAfterFailure(StreamBroken(), 1, api::kMaxCostlyFailureRetries + 1));
    // 普通网络错不计代价高次数,仍走满阶梯。
    const api::Error reset{api::ErrorKind::Network, "reset", 0};
    CHECK(api::ShouldRetryAfterFailure(reset, api::kMaxRequestAttempts - 1, 0));
    CHECK_FALSE(api::ShouldRetryAfterFailure(reset, api::kMaxRequestAttempts, 0));
    // 不可重试错不因封顶松口。
    CHECK_FALSE(api::ShouldRetryAfterFailure(api::Error{api::ErrorKind::HttpStatus, "x", 401}, 1, 0));
}

TEST_CASE("恢复环:流中途断一路失败,至多再试 kMaxCostlyFailureRetries 次就收口") {
    for (const api::Error& error : {StreamBroken(), WallHit()}) {
        SUBCASE(error.hard_timeout ? "无进展墙落锤" : "流已开始后断") {
            ScriptedSender sender;
            sender.script.assign(api::kMaxRequestAttempts, std::unexpected(error));
            const auto result = api::RunRequestWithRecovery(sender.Sender(), RecordingHooks(sender));
            REQUIRE_FALSE(result.has_value());
            CHECK(sender.calls == 1 + api::kMaxCostlyFailureRetries);
            CHECK(sender.waits.size() == static_cast<std::size_t>(api::kMaxCostlyFailureRetries));
            REQUIRE_FALSE(sender.phases.empty());
            CHECK(sender.phases.back() == api::RequestAttemptPhase::Exhausted);
            CHECK(sender.calls < api::kMaxRequestAttempts);  // 不走满六次长阶梯
        }
    }
}

TEST_CASE("恢复环:见过流事件的普通网络错也按代价高封顶(经恢复环旁证)") {
    ScriptedSender sender;
    int calls = 0;
    const api::AttemptSender send = [&calls](api::ModelRequestAttempt& attempt) -> std::expected<void, api::Error> {
        ++calls;
        attempt.saw_stream_event = true;  // 吐过事件再断:传输层旁证缺席也认
        return std::unexpected(api::Error{api::ErrorKind::Network, "reset", 0});
    };
    const auto result = api::RunRequestWithRecovery(send, RecordingHooks(sender));
    REQUIRE_FALSE(result.has_value());
    CHECK(calls == 1 + api::kMaxCostlyFailureRetries);
}

TEST_CASE("恢复环:连接前失败与代价高失败混排,封顶只数代价高的那几次") {
    static_assert(api::kMaxCostlyFailureRetries == 2, "脚本按封顶 2 次排的,改封顶要同步改脚本");
    ScriptedSender sender;
    const api::Error reset{api::ErrorKind::Network, "connect reset", 0};
    // 连接前失败两次不占代价高额度;随后代价高失败第 3 次(累计)才收口。
    sender.script = {std::unexpected(reset), std::unexpected(StreamBroken()), std::unexpected(reset),
                     std::unexpected(StreamBroken()), std::unexpected(StreamBroken()), {}};
    const auto result = api::RunRequestWithRecovery(sender.Sender(), RecordingHooks(sender));
    REQUIRE_FALSE(result.has_value());
    CHECK(sender.calls == 5);
    CHECK(result.error().stream_started);
}

TEST_CASE("恢复环:流中途断一次后重发成功,不受封顶影响") {
    ScriptedSender sender;
    sender.script = {std::unexpected(StreamBroken()), {}};
    const auto result = api::RunRequestWithRecovery(sender.Sender(), RecordingHooks(sender));
    REQUIRE(result.has_value());
    CHECK(sender.calls == 2);
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

TEST_CASE("恢复环:退避等待中取消立即返回,不发第 2 次") {
    ScriptedSender sender;
    sender.script = {
        std::unexpected(api::Error{api::ErrorKind::Network, "reset", 0}),
    };
    std::atomic<bool> cancel{false};
    auto hooks = RecordingHooks(sender);
    hooks.wait_backoff = [&cancel, &sender](std::chrono::milliseconds wait, const std::atomic<bool>*) {
        sender.waits.push_back(wait);
        cancel.store(true);
        return false;
    };
    const auto result = api::RunRequestWithRecovery(sender.Sender(), hooks, &cancel);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == api::ErrorKind::Cancelled);
    CHECK(sender.calls == 1);
    CHECK(sender.waits.size() == 1);
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
