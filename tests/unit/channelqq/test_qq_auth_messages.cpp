// QQ token 单飞/消息发送册(QQ 机器人接入单 Q1,§十五 auth/messages 模块)。
// 全程 mock HttpFunc,零网络。覆盖:
//   - token:单飞(并发一次请求)、到期重取、Invalidate、错误分型;
//   - messages:稳定 msg_seq(同 delivery 重试复用/同 msg_id 不同回复递增)、
//     Deduped 收账、Unauthorized 刷 token 重试、限频/5xx 分型(官方错误码表)。
#include <doctest/doctest.h>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "channel/qq/qq_auth.hpp"
#include "channel/qq/qq_messages.hpp"

namespace lubancode::channel::qq {
namespace {

// 可编程假 HTTP:按请求次序回脚本;记录收到的请求(供断言)。
// 闸门回包(SV-06 并发夹具):先报 entered、等 release 再回包——把一笔
// 回应扣在在途,主线程好安排第二只调用进场读到代次(定序确定性)。
struct ReplyGate {
    std::promise<void> entered;
    std::promise<void> release;
};

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
        std::shared_ptr<ReplyGate> gate;  // 非空 = 扣住等放行
    };

    mutable std::mutex mutex;
    std::vector<Call> calls;
    std::vector<Reply> replies;  // 按序消费;耗尽后恒 500

    QqHttpFunc Func() {
        return [this](const QqHttpRequest& request) -> std::expected<QqHttpResponse, std::string> {
            Reply reply;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                calls.push_back(Call{request.method, request.url, request.body, request.headers});
                if (replies.empty()) {
                    reply = Reply{500, R"({"code":50055002})", "", nullptr};
                } else {
                    reply = replies.front();
                    replies.erase(replies.begin());
                }
            }
            if (reply.gate) {
                reply.gate->entered.set_value();
                reply.gate->release.get_future().wait();
            }
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
    std::atomic<int> now_probes{0};  // token 缓存进门查到期即探测(并发夹具定序用)
    QqTokenManager::Options TokenOptions() {
        QqTokenManager::Options options;
        options.app_id = "APP1";
        options.client_secret = "SECRET1";
        options.http = http.Func();
        options.now_ms = [this]() {
            now_probes.fetch_add(1, std::memory_order_relaxed);
            return now.load();
        };
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
// SV-06 同代并发合同(经 QqTokenManager 门面过共用缓存状态机)
// ---------------------------------------------------------------------------

TEST_CASE("qq_auth: 并发取 token 单飞——一次成功两调用共享(SV-06)") {
    Fixture fixture;
    // 预热一枚 token,再拨过到期时刻(7200-300=6900s)。
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    REQUIRE(tokens.GetValidToken().has_value());
    fixture.now += 7'000'000;
    // 闸门回包:把重刷扣在在途,等第二只调用进场。
    auto gate = std::make_shared<ReplyGate>();
    fixture.http.replies.push_back(
        {200, R"({"access_token":"T2","expires_in":7200})", "", gate});
    std::optional<std::expected<std::string, QqTokenManager::Error>> r1;
    std::optional<std::expected<std::string, QqTokenManager::Error>> r2;
    std::thread leader([&] { r1 = tokens.GetValidToken(); });
    gate->entered.get_future().wait();
    fixture.now_probes = 0;  // leader 进场探针已花掉,重置后只数后来者
    std::thread waiter([&] { r2 = tokens.GetValidToken(); });
    while (fixture.now_probes.load(std::memory_order_relaxed) < 1) {
        std::this_thread::yield();
    }
    gate->release.set_value();
    leader.join();
    waiter.join();

    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    REQUIRE(r1->has_value());
    REQUIRE(r2->has_value());
    CHECK(r1->value() == "T2");
    CHECK(r2->value() == "T2");
    CHECK(fixture.http.calls.size() == 2);  // 预热 1 + 并发 1,单飞不放大
}

TEST_CASE("qq_auth: 并发取 token 单飞——同代失败共享,独立调用重试(SV-06)") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    REQUIRE(tokens.GetValidToken().has_value());
    fixture.now += 7'000'000;
    auto gate = std::make_shared<ReplyGate>();
    fixture.http.replies.push_back({401, R"({"code":-1})", "", gate});
    std::optional<std::expected<std::string, QqTokenManager::Error>> r1;
    std::optional<std::expected<std::string, QqTokenManager::Error>> r2;
    std::thread leader([&] { r1 = tokens.GetValidToken(); });
    gate->entered.get_future().wait();
    fixture.now_probes = 0;
    std::thread waiter([&] { r2 = tokens.GetValidToken(); });
    while (fixture.now_probes.load(std::memory_order_relaxed) < 1) {
        std::this_thread::yield();
    }
    gate->release.set_value();
    leader.join();
    waiter.join();

    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    REQUIRE_FALSE(r1->has_value());
    REQUIRE_FALSE(r2->has_value());
    CHECK(r1->error().kind == QqTokenManager::ErrorKind::InvalidCredentials);
    CHECK(r2->error().kind == QqTokenManager::ErrorKind::InvalidCredentials);
    CHECK(fixture.http.calls.size() == 2);  // 失败也只发一次,等待者共享错误
    // 独立后来调用:失败不永久缓存,照常重试。
    fixture.http.replies.push_back({200, R"({"access_token":"T3","expires_in":7200})"});
    const auto third = tokens.GetValidToken();
    REQUIRE(third.has_value());
    CHECK(*third == "T3");
    CHECK(fixture.http.calls.size() == 3);
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

// ---------------------------------------------------------------------------
// A03:err_code 官方形状、码字段非法/冲突不折算成成功;新业务码分型。
// ---------------------------------------------------------------------------

TEST_CASE("qq_messages: HTTP 200 + err_code 业务失败按码分型(A03)") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    QqMessageSender::Options options;
    options.http = fixture.http.Func();
    options.tokens = &tokens;
    options.api_base = "https://api.test";
    QqMessageSender sender(std::move(options));
    // 官方 API 指南失败示例原文形状(err_code + trace_id)。
    fixture.http.replies.push_back(
        {200, R"({"err_code":40034005,"message":"回复消息msg_id已过期",)"
              R"("trace_id":"4a8a61565b909f199b1ec169fdd6f49e"})"});
    const auto expired = sender.SendC2c(MakeSend("out-err1"));
    CHECK(expired.status == QqMessageSender::Outcome::Status::PermanentFail);
    CHECK(expired.error.kind == QqApiErrorKind::MsgIdExpired);
    CHECK(expired.error.platform_err_code == 40034005);
    CHECK(expired.error.trace_id == "4a8a61565b909f199b1ec169fdd6f49e");
    // err_code 字符串整数(平台数值字段两态)。
    fixture.http.replies.push_back({200, R"({"err_code":"40034100"})"});
    const auto throttled = sender.SendC2c(MakeSend("out-err2"));
    CHECK(throttled.status == QqMessageSender::Outcome::Status::DeferredRetry);
    CHECK(throttled.error.kind == QqApiErrorKind::RateLimited);
}

TEST_CASE("qq_messages: 码字段非法/冲突不冒充成功(A03)") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    QqMessageSender::Options options;
    options.http = fixture.http.Func();
    options.tokens = &tokens;
    options.api_base = "https://api.test";
    QqMessageSender sender(std::move(options));
    // 2xx + code 字段非法(解不出):成功合同无法核对,不 value_or(0) 当成功。
    fixture.http.replies.push_back({200, R"({"code":"soon"})"});
    const auto illegal = sender.SendC2c(MakeSend("out-bad1"));
    CHECK(illegal.status == QqMessageSender::Outcome::Status::PermanentFail);
    CHECK(illegal.error.kind == QqApiErrorKind::InvalidResponse);
    // 2xx + 两码冲突:不猜,不冒充成功。
    fixture.http.replies.push_back({200, R"({"code":40034005,"err_code":40054004})"});
    const auto conflict = sender.SendC2c(MakeSend("out-bad2"));
    CHECK(conflict.status == QqMessageSender::Outcome::Status::PermanentFail);
    CHECK(conflict.error.kind == QqApiErrorKind::InvalidResponse);
}

TEST_CASE("qq_messages: 40054006 好友校验失败可重试;40054016 离线可重试(A03)") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    QqMessageSender::Options options;
    options.http = fixture.http.Func();
    options.tokens = &tokens;
    options.api_base = "https://api.test";
    QqMessageSender sender(std::move(options));
    // 40054006 官方"验证好友关系失败、建议重试"——不再是 NoFriend 永久失败。
    fixture.http.replies.push_back({200, R"({"code":40054006,"message":"验证好友关系失败"})"});
    const auto friend_check = sender.SendC2c(MakeSend("out-f1"));
    CHECK(friend_check.status == QqMessageSender::Outcome::Status::DeferredRetry);
    CHECK(friend_check.error.kind == QqApiErrorKind::FriendCheckFailed);
    // 40054016 机器人已下线——状态可恢复。
    fixture.http.replies.push_back({200, R"({"code":40054016,"message":"机器人已下线"})"});
    const auto offline = sender.SendC2c(MakeSend("out-f2"));
    CHECK(offline.status == QqMessageSender::Outcome::Status::DeferredRetry);
    CHECK(offline.error.kind == QqApiErrorKind::BotOffline);
    // 对照:40054004 无好友关系仍是永久失败。
    fixture.http.replies.push_back({200, R"({"code":40054004})"});
    const auto no_friend = sender.SendC2c(MakeSend("out-f3"));
    CHECK(no_friend.status == QqMessageSender::Outcome::Status::PermanentFail);
    CHECK(no_friend.error.kind == QqApiErrorKind::NoFriend);
    // 40034128 被动回复时间或次数超限:独立分族,仍是锚点终结。
    fixture.http.replies.push_back({200, R"({"code":40034128})"});
    const auto quota = sender.SendC2c(MakeSend("out-f4"));
    CHECK(quota.status == QqMessageSender::Outcome::Status::PermanentFail);
    CHECK(quota.error.kind == QqApiErrorKind::ReplyQuotaExhausted);
    // 40034105 无权限:永久(申请权限前重试无意义)。
    fixture.http.replies.push_back({200, R"({"code":40034105})"});
    const auto no_perm = sender.SendC2c(MakeSend("out-f5"));
    CHECK(no_perm.status == QqMessageSender::Outcome::Status::PermanentFail);
    CHECK(no_perm.error.kind == QqApiErrorKind::PermissionDenied);
}

TEST_CASE("qq_messages: AckInteraction 成功合同——空成功放行,损坏不冒充(A03)") {
    Fixture fixture;
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    QqTokenManager tokens(fixture.TokenOptions());
    QqMessageSender::Options options;
    options.http = fixture.http.Func();
    options.tokens = &tokens;
    options.api_base = "https://api.test";
    QqMessageSender sender(std::move(options));
    // 204 无正文:官方"成功且无响应体"。
    fixture.http.replies.push_back({204, ""});
    CHECK(sender.AckInteraction("inter-1", 0).status == QqMessageSender::AckStatus::Acked);
    // 200 空体/纯空白:文档允许的空成功。
    fixture.http.replies.push_back({200, ""});
    CHECK(sender.AckInteraction("inter-2", 0).status == QqMessageSender::AckStatus::Acked);
    fixture.http.replies.push_back({200, "  \r\n "});
    CHECK(sender.AckInteraction("inter-3", 0).status == QqMessageSender::AckStatus::Acked);
    // 200 err_code=0:平台报成功。
    fixture.http.replies.push_back({200, R"({"err_code":0})"});
    CHECK(sender.AckInteraction("inter-4", 0).status == QqMessageSender::AckStatus::Acked);
    // 200 非空非 JSON:损坏,不冒充 Acked。
    fixture.http.replies.push_back({200, "not json"});
    const auto broken = sender.AckInteraction("inter-5", 0);
    CHECK(broken.status == QqMessageSender::AckStatus::Failed);
    CHECK(broken.error.kind == QqApiErrorKind::InvalidResponse);
    // 200 JSON 数组:非 object,损坏。
    fixture.http.replies.push_back({200, "[1,2]"});
    CHECK(sender.AckInteraction("inter-6", 0).status == QqMessageSender::AckStatus::Failed);
    // 200 业务码非 0:平台拒绝(官方回应 code=2 操作频繁)。
    fixture.http.replies.push_back({200, R"({"code":2,"message":"操作频繁"})"});
    const auto rejected = sender.AckInteraction("inter-7", 0);
    CHECK(rejected.status == QqMessageSender::AckStatus::Failed);
    CHECK(rejected.error.platform_code == 2);
    // 200 码字段非法:无法核对,不冒充。
    fixture.http.replies.push_back({200, R"({"code":"frequent"})"});
    CHECK(sender.AckInteraction("inter-8", 0).status == QqMessageSender::AckStatus::Failed);
}

}  // namespace lubancode::channel::qq
