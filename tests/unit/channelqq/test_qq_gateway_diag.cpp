// 网关 400 诊断册(证书部分解析误判修复单 §四/§六):非 2xx 网关响应的
// 平台 code/trace 分类稳定说明、敏感 message 不透传、鉴权失效才受控刷
// token、429 服从 Retry-After(有上限)、信任根阻断无效联网重试。分类是
// 纯函数直钉全表;adapter 集成路径经假 HTTP/假传输零网络。
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/bridge_protocol.hpp"
#include "channel/frame.hpp"
#include "channel/qq/qq_adapter.hpp"
#include "channel/qq/qq_gateway.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::channel::qq {
namespace {

// 固定假敏感值:平台 message 若被透传,断言能当场抓住(§六)。
constexpr const char* kFakeSecret = "ZZFAKE-SECRET-VALUE-DO-NOT-PRINT";

// ---------------------------------------------------------------------------
// 假件:最小传输(本册的重头在 provider/HTTP 阶段,传输不该被连到)与可
// 编排的假 HTTP(gateway 状态/体/诊断头可改,请求计数可查)。
// ---------------------------------------------------------------------------

class NullTransport final : public IGatewayTransport {
public:
    std::expected<void, GatewayConnectError> Connect(const std::string&) override {
        // 被连到即测试编排失败(本册失败都应发生在 provider 阶段);仍按
        // 失败返回,不让网关线程误入 READY。
        return std::unexpected(
            GatewayConnectError{kStageConnecting, "unexpected_connect", "should not connect"});
    }
    std::expected<void, std::string> SendText(const std::string&) override { return {}; }
    std::expected<std::string, WsError> ReadMessage(int) override {
        return std::unexpected(WsError{WsError::Kind::Timeout, "quiet", 0});
    }
    void Cancel() override {}
    void Close(std::uint16_t, const std::string&) override {}
};

struct ScriptHttp {
    mutable std::mutex mutex;
    int token_calls = 0;
    int gateway_calls = 0;
    int gateway_status = 200;
    std::string gateway_body = R"({"url":"wss://fake-gw.test/ws"})";
    std::vector<std::pair<std::string, std::string>> gateway_headers;

    QqHttpFunc Func() {
        return [this](const QqHttpRequest& request) -> std::expected<QqHttpResponse, std::string> {
            const std::lock_guard<std::mutex> lock(mutex);
            if (request.url.find("/app/getAppAccessToken") != std::string::npos) {
                ++token_calls;
                return QqHttpResponse{
                    200, R"({"access_token":"TT1","expires_in":7200})"};
            }
            if (request.url.find("/gateway") != std::string::npos) {
                ++gateway_calls;
                QqHttpResponse response{gateway_status, gateway_body};
                response.diagnostic_headers = gateway_headers;
                return response;
            }
            return QqHttpResponse{404, "{}"};
        };
    }
};

struct DiagHarness {
    std::filesystem::path state_root;
    ScriptHttp http;
    std::unique_ptr<QqBotAdapter> adapter;
    int next_id = 1;

    explicit DiagHarness(const char* tag)
        : state_root(std::filesystem::temp_directory_path() /
                     ("lubancode-qq-gw-diag-test-" + std::string(tag))) {
        std::error_code ec;
        std::filesystem::remove_all(state_root, ec);
        std::filesystem::create_directories(state_root, ec);
    }
    ~DiagHarness() { adapter.reset(); }

    QqBotAdapter::Options MakeOptions() {
        QqBotAdapter::Options options;
        options.channel_id = "qqbot";
        options.account_id = "main";
        options.config = channel::MakeQqTemplateAccount();
        options.config.app_id = "APP1";
        options.credential = ResolvedChannelCredential{};
        options.credential.secret = "SECRET1";
        options.credential.source = ResolvedChannelCredential::Source::InlinePlaintext;
        options.state_root = state_root;
        options.http = http.Func();
        options.transport_factory = []() {
            return std::unique_ptr<IGatewayTransport>(std::make_unique<NullTransport>());
        };
        options.now_ms = []() { return platform::WallClockNowMs(); };
        options.api_base = "https://api.test";
        options.bots_base = "https://bots.test";
        return options;
    }

    void Start() {
        // 手工路径:initialize + start 两帧直写(不经 manager Pump)。
        nlohmann::json initialize = channel::BuildRequestJson(
            next_id++, channel::BridgeMethod::Initialize,
            nlohmann::json{{"protocol_version", "2026-08-29"},
                           {"channel_id", "qqbot"},
                           {"account_id", "main"},
                           {"state_dir", "/tmp/qq-gw-diag-state"},
                           {"host", nlohmann::json{{"name", "test-host"}}}});
        auto encoded = channel::EncodeFrame(initialize);
        REQUIRE(encoded.has_value());
        adapter->WriteToSidecar(encoded->data(), encoded->size());
        nlohmann::json start = channel::BuildRequestJson(
            next_id++, channel::BridgeMethod::Start, nlohmann::json{{"transport", "websocket"}});
        encoded = channel::EncodeFrame(start);
        REQUIRE(encoded.has_value());
        adapter->WriteToSidecar(encoded->data(), encoded->size());
    }

    bool WaitFailure(const std::function<bool(const ConnectionSnapshot&)>& pred,
                     int timeout_ms = 8'000) {
        const auto deadline = platform::WallClockNowMs() + timeout_ms;
        while (platform::WallClockNowMs() < deadline) {
            if (pred(adapter->ConnectionState())) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// ClassifyGatewayHttpFailure:纯函数直钉全表(§四)。
// ---------------------------------------------------------------------------

TEST_CASE("qq_gateway_diag: 分类——400 JSON 带平台 code,trace 透传,message 不透传") {
    const std::string body = std::string(R"({"code":11253,"message":"bad request )") +
                               kFakeSecret + R"(","trace_id":"trace-abc-1"})";
    const auto cls = ClassifyGatewayHttpFailure(400, body, {});
    CHECK(cls.code == "gateway_url_bad_request");
    CHECK(cls.detail.find("平台code=11253") != std::string::npos);
    CHECK(cls.detail.find("trace=trace-abc-1") != std::string::npos);
    CHECK(cls.detail.find(kFakeSecret) == std::string::npos);  // 原始 message 不打印
    CHECK(cls.retry_after_ms == 0);
}

TEST_CASE("qq_gateway_diag: 分类——400 非 JSON / JSON 无 code") {
    const auto not_json = ClassifyGatewayHttpFailure(400, "plain text error", {});
    CHECK(not_json.code == "gateway_url_bad_response");
    CHECK(not_json.detail.find("非 JSON") != std::string::npos);

    const auto no_code = ClassifyGatewayHttpFailure(400, R"({"message":"x"})", {});
    CHECK(no_code.code == "gateway_url_bad_request");
    CHECK(no_code.detail.find("无平台 code") != std::string::npos);
}

TEST_CASE("qq_gateway_diag: 分类——401/403/5xx/未知 4xx 各归稳定码") {
    CHECK(ClassifyGatewayHttpFailure(401, "{}", {}).code == "gateway_url_unauthorized");
    CHECK(ClassifyGatewayHttpFailure(403, "{}", {}).code == "gateway_url_forbidden");
    CHECK(ClassifyGatewayHttpFailure(500, "{}", {}).code == "gateway_url_server_error");
    CHECK(ClassifyGatewayHttpFailure(503, "{}", {}).code == "gateway_url_server_error");
    const auto unknown = ClassifyGatewayHttpFailure(404, "{}", {});
    CHECK(unknown.code == "gateway_url_http_failed");
    CHECK(unknown.detail.find("404") != std::string::npos);
}

TEST_CASE("qq_gateway_diag: 分类——429 Retry-After 数字秒生效,HTTP-date 不解析") {
    const auto seconds = ClassifyGatewayHttpFailure(
        429, R"({"code":11244})", {{"retry-after", "30"}});
    CHECK(seconds.code == "gateway_url_rate_limited");
    CHECK(seconds.retry_after_ms == 30'000);
    CHECK(seconds.detail.find("retry_after=30s") != std::string::npos);

    const auto http_date = ClassifyGatewayHttpFailure(
        429, "{}", {{"retry-after", "Wed, 21 Oct 2026 07:28:00 GMT"}});
    CHECK(http_date.code == "gateway_url_rate_limited");
    CHECK(http_date.retry_after_ms == 0);  // 只认纯数字秒,不猜
    CHECK(http_date.detail.find("无 Retry-After") != std::string::npos);

    const auto none = ClassifyGatewayHttpFailure(429, "{}", {});
    CHECK(none.retry_after_ms == 0);
}

TEST_CASE("qq_gateway_diag: 分类——trace 走白名单诊断头兜底,值超长截断") {
    const auto from_header = ClassifyGatewayHttpFailure(400, "not json",
                                                        {{"x-trace-id", "tid-9"}});
    CHECK(from_header.detail.find("trace=tid-9") != std::string::npos);
    // 头名大小写不敏感(HTTP 头名本就不分大小写;直调/假件给原始大小写也认)。
    const auto mixed_case = ClassifyGatewayHttpFailure(400, "not json",
                                                       {{"X-Trace-Id", "tid-mixed"}});
    CHECK(mixed_case.detail.find("trace=tid-mixed") != std::string::npos);
    // 非 trace 名不在白名单,不进诊断(头投影在适配层已白名单,这里防御)。
    const auto no_leak = ClassifyGatewayHttpFailure(400, "not json", {{"server", "nginx"}});
    CHECK(no_leak.detail.find("nginx") == std::string::npos);

    const std::string long_trace(300, 'x');
    const auto body = R"({"code":1,"trace_id":")" + long_trace + R"("})";
    const auto capped = ClassifyGatewayHttpFailure(400, body, {});
    CHECK(capped.detail.find("...") != std::string::npos);  // 128 帽截断
    CHECK(capped.detail.size() < 300);
}

// ---------------------------------------------------------------------------
// adapter 集成:FetchGatewayUrl 走真路,失败账/受控刷新/退避/阻断逐项对。
// ---------------------------------------------------------------------------

TEST_CASE("qq_gateway_diag: 400 集成——稳定码进失败账,detail 无敏感值,不刷 token") {
    DiagHarness harness("bad_request");
    harness.http.gateway_status = 400;
    harness.http.gateway_body = std::string(R"({"code":11253,"message":"err )") +
                                           kFakeSecret + R"(","trace_id":"trace-400"})";
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeOptions());
    harness.Start();
    REQUIRE(harness.WaitFailure([](const ConnectionSnapshot& snapshot) {
        return snapshot.last_failure.has_value() &&
               snapshot.last_failure->error_code == "gateway_url_bad_request";
    }));
    const auto failure = harness.adapter->ConnectionState().last_failure;
    REQUIRE(failure.has_value());
    CHECK(failure->stage == kStageFetchingGatewayUrl);
    CHECK(failure->detail.find("平台code=11253") != std::string::npos);
    CHECK(failure->detail.find("trace=trace-400") != std::string::npos);
    CHECK(failure->detail.find(kFakeSecret) == std::string::npos);
    CHECK(failure->attempt >= 1);  // 尝试编号入账(§四)
    // 400 不重置密钥:token 只取过一次(缓存有效期内不再取);gateway 按
    // 退避重试(次数可能 >1),但每次用的都是同一枚缓存 token。
    {
        const std::lock_guard<std::mutex> lock(harness.http.mutex);
        CHECK(harness.http.token_calls == 1);
        CHECK(harness.http.gateway_calls >= 1);
    }
}

TEST_CASE("qq_gateway_diag: 401 集成——鉴权失效受控刷新 token;401 码入账") {
    DiagHarness harness("unauthorized");
    harness.http.gateway_status = 401;
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeOptions());
    harness.Start();
    REQUIRE(harness.WaitFailure([](const ConnectionSnapshot& snapshot) {
        return snapshot.last_failure.has_value() &&
               snapshot.last_failure->error_code == "gateway_url_unauthorized";
    }));
    // 仅明确鉴权失效才失效缓存:401 后 Invalidate,下一轮重新取 token
    // (token_calls >= 2);每轮 provider 一次,刷新受控不连环。
    const auto deadline = platform::WallClockNowMs() + 8'000;
    bool refreshed = false;
    while (platform::WallClockNowMs() < deadline) {
        const std::lock_guard<std::mutex> lock(harness.http.mutex);
        if (harness.http.token_calls >= 2) {
            refreshed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(refreshed);
}

TEST_CASE("qq_gateway_diag: 429 集成——退避服从有效 Retry-After(封 60s 帽)") {
    DiagHarness harness("rate_limited");
    harness.http.gateway_status = 429;
    harness.http.gateway_body = R"({"code":11244})";
    harness.http.gateway_headers = {{"Retry-After", "30"}};
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeOptions());
    harness.Start();
    REQUIRE(harness.WaitFailure([](const ConnectionSnapshot& snapshot) {
        return snapshot.last_failure.has_value() &&
               snapshot.last_failure->error_code == "gateway_url_rate_limited";
    }));
    // Retry-After 30s 高于阶梯首档(1s):next_retry 距今至少 ~25s(留余量),
    // 且封 60s 帽(不因服务端钉死更久)。不睡等,拿账即断。
    bool obeys = false;
    const auto deadline = platform::WallClockNowMs() + 8'000;
    while (platform::WallClockNowMs() < deadline) {
        const ConnectionSnapshot snapshot = harness.adapter->ConnectionState();
        const std::int64_t remaining = snapshot.next_retry_at_ms - platform::WallClockNowMs();
        if (snapshot.next_retry_at_ms > 0) {
            CHECK(remaining <= 61'000);  // 上限在帽内
            if (remaining >= 25'000) {
                obeys = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(obeys);
}

TEST_CASE("qq_gateway_diag: 200 畸形响应——bad_response 不联网重试风暴") {
    DiagHarness harness("bad_response");
    harness.http.gateway_status = 200;
    harness.http.gateway_body = R"({"no_url": true})";
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeOptions());
    harness.Start();
    REQUIRE(harness.WaitFailure([](const ConnectionSnapshot& snapshot) {
        return snapshot.last_failure.has_value() &&
               snapshot.last_failure->error_code == "gateway_url_bad_response";
    }));
}

TEST_CASE("qq_gateway_diag: 信任根预检失败——阻断 token/gateway 请求(§四)") {
    DiagHarness harness("trust_blocked");
    auto options = harness.MakeOptions();
    options.trust_load_block_code = kTlsCodeTrustStoreLoadFailed;
    options.trust_load_block_detail = "TLS 信任根不可用(装配预检): 显式信任锚含 1 张坏证(测试注入)";
    harness.adapter = std::make_unique<QqBotAdapter>(std::move(options));
    harness.Start();
    // 网关线程在跑(退避轮转),但一次 token/gateway 请求都不发——无效
    // 信任根下联网注定徒劳;失败账带装配预检的稳定码。
    REQUIRE(harness.WaitFailure([](const ConnectionSnapshot& snapshot) {
        return snapshot.last_failure.has_value() &&
               snapshot.last_failure->error_code == kTlsCodeTrustStoreLoadFailed;
    }));
    const auto failure = harness.adapter->ConnectionState().last_failure;
    REQUIRE(failure.has_value());
    CHECK(failure->stage == kStageConnecting);  // 本地阻断,未进联网阶段
    CHECK(failure->detail.find("装配预检") != std::string::npos);
    // 多跑几轮退避,账上始终零请求(不持续请求 token/gateway)。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        const std::lock_guard<std::mutex> lock(harness.http.mutex);
        CHECK(harness.http.token_calls == 0);
        CHECK(harness.http.gateway_calls == 0);
    }
}

TEST_CASE("qq_gateway_diag: 敏感值不进宿主通知帧——Status backoff 帧无假密值") {
    DiagHarness harness("no_leak_frames");
    harness.http.gateway_status = 400;
    harness.http.gateway_body = std::string(R"({"code":11253,"message":"err )") +
                                           kFakeSecret + R"("})";
    harness.adapter = std::make_unique<QqBotAdapter>(harness.MakeOptions());
    harness.Start();
    REQUIRE(harness.WaitFailure([](const ConnectionSnapshot& snapshot) {
        return snapshot.last_failure.has_value() &&
               snapshot.last_failure->error_code == "gateway_url_bad_request";
    }));
    // 多拍 drain:Status(backoff) 通知的 detail 与任何字段都不带假敏感值。
    bool saw_status_backoff = false;
    const auto deadline = platform::WallClockNowMs() + 8'000;
    while (platform::WallClockNowMs() < deadline) {
        channel::FrameDecoder decoder;
        const auto bytes = harness.adapter->DrainFromSidecar();
        decoder.Feed(bytes.data(), bytes.size());
        while (true) {
            auto next = decoder.TryDecodeNext();
            if (!next.has_value() || !next->has_value()) {
                break;
            }
            const std::string dumped = (*next)->dump();
            CHECK(dumped.find(kFakeSecret) == std::string::npos);
            if ((*next)->value("method", "") == "channel.status" &&
                (*next)->at("params").at("state") == "backoff") {
                saw_status_backoff = true;
            }
        }
        if (saw_status_backoff) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(saw_status_backoff);
}

}  // namespace lubancode::channel::qq
