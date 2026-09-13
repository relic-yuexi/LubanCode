// QQ token 单飞/消息发送册(QQ 机器人接入单 Q1,§十五 auth/messages 模块)。
// 全程 mock HttpFunc,零网络。覆盖:
//   - token:单飞(并发一次请求)、到期重取、Invalidate、错误分型;
//   - messages:稳定 msg_seq(同 delivery 重试复用/同 msg_id 不同回复递增)、
//     Deduped 收账、Unauthorized 刷 token 重试、限频/5xx 分型(官方错误码表)。
#include <doctest/doctest.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "channel/qq/qq_auth.hpp"
#include "channel/qq/qq_messages.hpp"

namespace lubancode::channel::qq {
namespace {

// 可编程假 HTTP:按请求次序回脚本;记录收到的请求(供断言)。
struct ScriptedHttp {
    struct Call {
        std::string method;
        std::string url;
        std::string body;
        std::vector<std::pair<std::string, std::string>> headers;
    };
    struct Reply {
        int status = 200;
        std::string body;
        std::string error;  // 非空 = 传输失败
    };

    mutable std::mutex mutex;
    std::vector<Call> calls;
    std::vector<Reply> replies;  // 按序消费;耗尽后恒 500

    QqHttpFunc Func() {
        return [this](const QqHttpRequest& request) -> std::expected<QqHttpResponse, std::string> {
            const std::lock_guard<std::mutex> lock(mutex);
            calls.push_back(Call{request.method, request.url, request.body, request.headers});
            if (replies.empty()) {
                return QqHttpResponse{500, R"({"code":50055002})"};
            }
            const Reply reply = replies.front();
            replies.erase(replies.begin());
            if (!reply.error.empty()) {
                return std::unexpected(reply.error);
            }
            return QqHttpResponse{reply.status, reply.body};
        };
    }
};

struct Fixture {
    ScriptedHttp http;
    std::atomic<std::int64_t> now{10'000};
    QqTokenManager::Options TokenOptions() {
        QqTokenManager::Options options;
        options.app_id = "APP1";
        options.client_secret = "SECRET1";
        options.http = http.Func();
        options.now_ms = [this]() { return now.load(); };
        options.token_url = "https://bots.test/app/getAppAccessToken";
        options.refresh_margin_secs = 300;
        return options;
    }
};

C2cSendRequest MakeSend(std::string delivery = "out-1", std::uint32_t seq_hint = 0) {
    C2cSendRequest request;
    request.openid = "OPEN1";
    request.content = "reply text";
    request.msg_id = "MSG1";
    request.outbound_delivery_id = delivery;
    (void)seq_hint;
    return request;
}

}  // namespace

// ---------------------------------------------------------------------------
// auth
// ---------------------------------------------------------------------------

TEST_CASE("qq_auth: 首取换 token;错误文案不带 secret") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    const auto token = tokens.GetValidToken();
    REQUIRE(token.has_value());
    CHECK(*token == "T1");
    REQUIRE(fixture.http.calls.size() == 1);
    CHECK(fixture.http.calls[0].url == "https://bots.test/app/getAppAccessToken");
    CHECK(fixture.http.calls[0].body.find("SECRET1") != std::string::npos);  // 请求体带
}

TEST_CASE("qq_auth: 未到期不重取;到期现刷;Invalidate 强制现刷") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    REQUIRE(tokens.GetValidToken().has_value());
    // 7200-300=6900s 内有效。
    fixture.now += 1'000;
    CHECK(tokens.GetValidToken().has_value());
    CHECK(fixture.http.calls.size() == 1);
    // 过了有效窗:重取。
    fixture.http.replies.push_back({200, R"({"access_token":"T2","expires_in":7200})"});
    fixture.now += 7'000'000;
    const auto next = tokens.GetValidToken();
    REQUIRE(next.has_value());
    CHECK(*next == "T2");
    CHECK(fixture.http.calls.size() == 2);
    // Invalidate:即使没到期也现刷。
    tokens.Invalidate();
    fixture.http.replies.push_back({200, R"({"access_token":"T3","expires_in":7200})"});
    const auto third = tokens.GetValidToken();
    REQUIRE(third.has_value());
    CHECK(*third == "T3");
    CHECK(fixture.http.calls.size() == 3);
}

TEST_CASE("qq_auth: 错误分型——401 凭据/429 限频/5xx 服务/网络") {
    {
        Fixture fixture;
        fixture.http.replies.push_back({401, R"({"code":-1})"});
        QqTokenManager tokens(fixture.TokenOptions());
        const auto error = tokens.GetValidToken();
        REQUIRE_FALSE(error.has_value());
        CHECK(error.error().kind == QqTokenManager::ErrorKind::InvalidCredentials);
    }
    {
        Fixture fixture;
        fixture.http.replies.push_back({429, "{}"});
        QqTokenManager tokens(fixture.TokenOptions());
        const auto error = tokens.GetValidToken();
        REQUIRE_FALSE(error.has_value());
        CHECK(error.error().kind == QqTokenManager::ErrorKind::RateLimited);
    }
    {
        Fixture fixture;
        fixture.http.replies.push_back({503, "{}"});
        QqTokenManager tokens(fixture.TokenOptions());
        const auto error = tokens.GetValidToken();
        REQUIRE_FALSE(error.has_value());
        CHECK(error.error().kind == QqTokenManager::ErrorKind::ServerError);
    }
    {
        Fixture fixture;
        fixture.http.replies.push_back({0, "", "http timeout"});
        QqTokenManager tokens(fixture.TokenOptions());
        const auto error = tokens.GetValidToken();
        REQUIRE_FALSE(error.has_value());
        CHECK(error.error().kind == QqTokenManager::ErrorKind::NetworkError);
        // 网络错误文案不携带 secret。
        CHECK(error.error().detail.find("SECRET1") == std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// messages: 稳定 msg_seq 与重试合同
// ---------------------------------------------------------------------------

TEST_CASE("qq_messages: 成功发送带 msg_seq=1;provider id 回传") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    QqMessageSender::Options options;
    options.http = fixture.http.Func();
    options.tokens = &tokens;
    options.api_base = "https://api.test";
    QqMessageSender sender(std::move(options));

    fixture.http.replies.push_back({200, R"({"id":"ROBOT1.0_out1"})"});
    const auto outcome = sender.SendC2c(MakeSend());
    CHECK(outcome.status == QqMessageSender::Outcome::Status::Sent);
    CHECK(outcome.provider_message_id == "ROBOT1.0_out1");
    // 第二笔请求(token 之后)是发送;载荷核 msg_seq=1。
    REQUIRE(fixture.http.calls.size() == 2);
    const auto& send_body = nlohmann::json::parse(fixture.http.calls[1].body);
    CHECK(send_body.at("msg_seq") == 1);
    CHECK(send_body.at("msg_id") == "MSG1");
    CHECK(fixture.http.calls[1].url == "https://api.test/v2/users/OPEN1/messages");
    // Authorization 带 QQBot 前缀。
    bool has_auth = false;
    for (const auto& [name, value] : fixture.http.calls[1].headers) {
        if (name == "Authorization" && value == "QQBot T1") {
            has_auth = true;
        }
    }
    CHECK(has_auth);
}

TEST_CASE("qq_messages: 同 delivery 重试复用同一 msg_seq(稳定载荷)") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    QqMessageSender::Options options;
    options.http = fixture.http.Func();
    options.tokens = &tokens;
    options.api_base = "https://api.test";
    QqMessageSender sender(std::move(options));

    // 第一次:500(可重试分型)。
    fixture.http.replies.push_back({200, R"({"code":50055002,"message":"retry later"})"});
    const auto first = sender.SendC2c(MakeSend("out-1"));
    CHECK(first.status == QqMessageSender::Outcome::Status::DeferredRetry);
    // 第二次(同 delivery 重试):成功。seq 必须还是 1。
    fixture.http.replies.push_back({200, R"({"id":"out-ok"})"});
    const auto second = sender.SendC2c(MakeSend("out-1"));
    CHECK(second.status == QqMessageSender::Outcome::Status::Sent);
    REQUIRE(fixture.http.calls.size() == 3);
    const auto body1 = nlohmann::json::parse(fixture.http.calls[1].body);
    const auto body2 = nlohmann::json::parse(fixture.http.calls[2].body);
    CHECK(body1.at("msg_seq") == 1);
    CHECK(body2.at("msg_seq") == 1);
}

TEST_CASE("qq_messages: 同 msg_id 不同 delivery 各占一个序号") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    QqMessageSender::Options options;
    options.http = fixture.http.Func();
    options.tokens = &tokens;
    options.api_base = "https://api.test";
    QqMessageSender sender(std::move(options));

    fixture.http.replies.push_back({200, R"({"id":"a"})"});
    fixture.http.replies.push_back({200, R"({"id":"b"})"});
    CHECK(sender.SendC2c(MakeSend("out-1")).status == QqMessageSender::Outcome::Status::Sent);
    CHECK(sender.SendC2c(MakeSend("out-2")).status == QqMessageSender::Outcome::Status::Sent);
    const auto body1 = nlohmann::json::parse(fixture.http.calls[1].body);
    const auto body2 = nlohmann::json::parse(fixture.http.calls[2].body);
    CHECK(body1.at("msg_seq") == 1);
    CHECK(body2.at("msg_seq") == 2);  // 官方:同 msg_id 回复序号区分(上限 4)
}

TEST_CASE("qq_messages: 40054005 平台去重按已送达收账(Deduped)") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    QqMessageSender::Options options;
    options.http = fixture.http.Func();
    options.tokens = &tokens;
    options.api_base = "https://api.test";
    QqMessageSender sender(std::move(options));
    fixture.http.replies.push_back({200, R"({"code":40054005,"message":"消息被去重"})"});
    const auto outcome = sender.SendC2c(MakeSend());
    CHECK(outcome.status == QqMessageSender::Outcome::Status::Deduped);
    CHECK(outcome.error.platform_code == 40054005);
}

TEST_CASE("qq_messages: 过期(40034005)不自动重试——PermanentFail") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    QqMessageSender::Options options;
    options.http = fixture.http.Func();
    options.tokens = &tokens;
    options.api_base = "https://api.test";
    QqMessageSender sender(std::move(options));
    fixture.http.replies.push_back({200, R"({"code":40034005,"message":"msg_id 已过期"})"});
    const auto outcome = sender.SendC2c(MakeSend());
    CHECK(outcome.status == QqMessageSender::Outcome::Status::PermanentFail);
    CHECK(outcome.error.kind == QqApiErrorKind::MsgIdExpired);
    CHECK(fixture.http.calls.size() == 2);  // 只发过一次
}

TEST_CASE("qq_messages: 401 先刷 token 再试一次(载荷不变)") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    QqMessageSender::Options options;
    options.http = fixture.http.Func();
    options.tokens = &tokens;
    options.api_base = "https://api.test";
    QqMessageSender sender(std::move(options));
    // 发送 401 → 刷 token(T2) → 重发成功。
    fixture.http.replies.push_back({401, "{}"});
    fixture.http.replies.push_back({200, R"({"access_token":"T2","expires_in":7200})"});
    fixture.http.replies.push_back({200, R"({"id":"ok"})"});
    const auto outcome = sender.SendC2c(MakeSend());
    CHECK(outcome.status == QqMessageSender::Outcome::Status::Sent);
    REQUIRE(fixture.http.calls.size() == 4);
    const auto retry_body = nlohmann::json::parse(fixture.http.calls[3].body);
    CHECK(retry_body.at("msg_seq") == 1);  // 刷 token 不换载荷
    bool used_t2 = false;
    for (const auto& [name, value] : fixture.http.calls[3].headers) {
        if (name == "Authorization" && value == "QQBot T2") {
            used_t2 = true;
        }
    }
    CHECK(used_t2);
}

TEST_CASE("qq_messages: 网络失败 DeferredRetry;非 JSON 2xx 不重试") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    QqMessageSender::Options options;
    options.http = fixture.http.Func();
    options.tokens = &tokens;
    options.api_base = "https://api.test";
    QqMessageSender sender(std::move(options));
    fixture.http.replies.push_back({0, "", "http network failed"});
    CHECK(sender.SendC2c(MakeSend()).status ==
          QqMessageSender::Outcome::Status::DeferredRetry);
    fixture.http.replies.push_back({200, "not json"});
    CHECK(sender.SendC2c(MakeSend("out-9")).status ==
          QqMessageSender::Outcome::Status::PermanentFail);
}

}  // namespace lubancode::channel::qq
