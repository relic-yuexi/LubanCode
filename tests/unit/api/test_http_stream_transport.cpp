// 批六(API 传输合流):HttpStreamTransport 的纯函数件单测——状态行抠码、
// 网络错误分型、请求体 dump 兜底、无进展墙期限账,外加归一到 types.hpp 的三个共用小件
// (ApplyExtraHeaders/RoleToString/MergeExtraBody)。
// 真socket 那头的传输行为(取消/硬墙钟/空闲超时/非 2xx/帧溢出)由
// tests/integration/process/test_network_timeout.cpp 拿假服务器钉着,这里
// 不重复搭服务器。
// 文案断言不用写死中文串,一律跟 cli::trf 的同键结果比——语言包切换
// (别的测试册可能 SetLanguage)不会弄红这里。

#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "api/http_stream_transport.hpp"
#include "api/types.hpp"
#include "cli/i18n.hpp"

using lubancode::api::ApplyExtraHeaders;
using lubancode::api::ClassifyNetworkError;
using lubancode::api::DumpRequestBody;
using lubancode::api::ExtractStatusCode;
using lubancode::api::MergeExtraBody;
using lubancode::api::NoProgressDeadline;
using lubancode::api::RoleToString;
namespace cli = lubancode::cli;

namespace {

// cpr::Error 只有"curl 错误码 -> ErrorCode"一个转换构造,没有直接收
// ErrorCode 的口子;code/message 都是公开成员,现场拼一只最省事。
cpr::Error MakeError(cpr::ErrorCode code, const std::string& message) {
    cpr::Error error;
    error.code = code;
    error.message = message;
    return error;
}

}  // namespace

// ---------------------------------------------------------------------------
// ExtractStatusCode
// ---------------------------------------------------------------------------

TEST_CASE("ExtractStatusCode: 从 HTTP 状态行里抠出状态码") {
    CHECK(ExtractStatusCode("HTTP/1.1 200 OK") == 200);
    CHECK(ExtractStatusCode("HTTP/1.1 404 Not Found") == 404);
    CHECK(ExtractStatusCode("HTTP/1.1 500") == 500);
    CHECK(ExtractStatusCode("HTTP/1.0 301 Moved Permanently") == 301);
    CHECK(ExtractStatusCode("HTTP/2 418") == 418);
}

TEST_CASE("ExtractStatusCode: 非状态行/残行抠不出,返回 0") {
    CHECK(ExtractStatusCode("Content-Type: text/event-stream") == 0);
    CHECK(ExtractStatusCode("X-HTTP/1.1 200 OK") == 0);  // 前缀不在行首
    CHECK(ExtractStatusCode("HTTP/1.1") == 0);           // 只有版本号没有码
    CHECK(ExtractStatusCode("HTTP/1.1 abc OK") == 0);    // 码位不是数字
    CHECK(ExtractStatusCode("") == 0);
}

// ---------------------------------------------------------------------------
// ClassifyNetworkError
// ---------------------------------------------------------------------------

TEST_CASE("ClassifyNetworkError: 硬墙钟分型最优先,压过其余一切错误码") {
    // 就连 OPERATION_TIMEDOUT 也让位——墙是我们自己落的锤。
    const auto msg = ClassifyNetworkError(MakeError(cpr::ErrorCode::OPERATION_TIMEDOUT, "t"), true, 15000, 60, true,
                                          30);
    CHECK(msg == cli::trf("error.network.hard_timeout", 30));
}

TEST_CASE("ClassifyNetworkError: OPERATION_TIMEDOUT 按'收到过字节没有'分型") {
    const cpr::Error error = MakeError(cpr::ErrorCode::OPERATION_TIMEDOUT, "timeout");
    CHECK(ClassifyNetworkError(error, true, 15000, 45, false, 30) ==
          cli::trf("error.network.stream_idle_timeout", 45));
    CHECK(ClassifyNetworkError(error, false, 15000, 45, false, 30) ==
          cli::trf("error.network.connect_timeout", 15));  // 15000ms -> 15s
}

TEST_CASE("ClassifyNetworkError: 连不上三兄弟走 connect_failed 文案,原始 curl 串不丢") {
    for (const cpr::ErrorCode code :
         {cpr::ErrorCode::COULDNT_CONNECT, cpr::ErrorCode::COULDNT_RESOLVE_HOST,
          cpr::ErrorCode::COULDNT_RESOLVE_PROXY}) {
        CHECK(ClassifyNetworkError(MakeError(code, "raw curl msg"), false, 15000, 45, false, 30) ==
              cli::trf("error.network.connect_failed", "raw curl msg"));
    }
}

TEST_CASE("ClassifyNetworkError: 其余错误码原样透传 curl message,不过度包装") {
    CHECK(ClassifyNetworkError(MakeError(cpr::ErrorCode::SEND_ERROR, "send failed"), false, 15000, 45, false, 30) ==
          "send failed");
    CHECK(ClassifyNetworkError(MakeError(cpr::ErrorCode::OK, ""), false, 15000, 45, false, 30).empty());
}

// ---------------------------------------------------------------------------
// NoProgressDeadline(无进展墙账本,Terminal-Bench 超时单)
// 时间点全由用例递,不睡真钟:t0 起步,秒级推演。
// ---------------------------------------------------------------------------

namespace {
using WallClock = NoProgressDeadline::Clock;
const WallClock::time_point kT0{};
WallClock::time_point At(int secs) { return kT0 + std::chrono::seconds(secs); }
}  // namespace

TEST_CASE("NoProgressDeadline: 持续来字节,总时长远超一格墙也不到期") {
    NoProgressDeadline wall(300, kT0);
    REQUIRE(wall.enabled());
    // 每 10 秒来一段正文,一路流到第 1200 秒(四格墙)——旧死墙第 300 秒就砍。
    for (int t = 10; t <= 1200; t += 10) {
        CHECK_FALSE(wall.Expired(At(t)));
        wall.OnBodyBytes(64, At(t));
    }
    CHECK(wall.deadline() == At(1500));
    CHECK_FALSE(wall.Expired(At(1499)));
    CHECK(wall.Expired(At(1500)));  // 最后一个字节之后满一格,照样落锤
}

TEST_CASE("NoProgressDeadline: 一个字节都不来,从发出起算到点落锤") {
    NoProgressDeadline wall(300, kT0);
    CHECK_FALSE(wall.Expired(At(299)));
    CHECK(wall.Expired(At(300)));
    // 进度回调每秒照醒,但累计量为 0 不涨:不算进展,不顺延。
    wall.OnDownloadProgress(0, At(299));
    CHECK(wall.deadline() == At(300));
    // 空段(写回调零长)也不算进展。
    wall.OnBodyBytes(0, At(299));
    CHECK(wall.deadline() == At(300));
}

TEST_CASE("NoProgressDeadline: 进度回调只认累计下载量创新高") {
    NoProgressDeadline wall(60, kT0);
    wall.OnDownloadProgress(100, At(50));
    CHECK(wall.deadline() == At(110));
    wall.OnDownloadProgress(100, At(100));  // 量没涨:不顺延
    CHECK(wall.deadline() == At(110));
    wall.OnDownloadProgress(40, At(105));  // 量回退:不顺延
    CHECK(wall.deadline() == At(110));
    wall.OnDownloadProgress(101, At(109));
    CHECK(wall.deadline() == At(169));
    CHECK_FALSE(wall.Expired(At(168)));
    CHECK(wall.Expired(At(169)));
}

TEST_CASE("NoProgressDeadline: timeout <= 0 不设墙,永不到期") {
    for (const int secs : {0, -5}) {
        NoProgressDeadline wall(secs, kT0);
        CHECK_FALSE(wall.enabled());
        CHECK_FALSE(wall.Expired(kT0));
        CHECK_FALSE(wall.Expired(At(100000)));
        wall.OnBodyBytes(1, At(10));
        CHECK_FALSE(wall.Expired(At(100000)));
    }
}

// ---------------------------------------------------------------------------
// DumpRequestBody
// ---------------------------------------------------------------------------

TEST_CASE("DumpRequestBody: 干净树与 json::dump 逐字节一致") {
    nlohmann::json body;
    body["model"] = "test-model";
    body["stream"] = true;
    body["nested"] = nlohmann::json{{"a", 1}, {"b", nlohmann::json::array({1, 2, 3})}};
    CHECK(DumpRequestBody("test", body) == body.dump());
}

TEST_CASE("DumpRequestBody: 坏 UTF-8 不抛异常,按 U+FFFD 清洗后照发") {
    nlohmann::json body;
    // 合法中文里夹一枚孤立坏字节(0xFF)——SanitizeExternalText 对"混合
    // 内容"的固定走法是逐段替换成 U+FFFD,不走 Windows 下按 ACP 整段重解
    // 的那条岔路(那条路给纯坏字节串准备的),断言才能跨平台钉死。
    body["text"] = "中文夹一枚坏字节\xFF至此";
    // 先钉前提:这棵树直接 dump() 必抛(type_error.316)——兜底存在的理由。
    bool threw = false;
    try {
        (void)body.dump();
    } catch (const nlohmann::json::exception&) {
        threw = true;
    }
    REQUIRE(threw);

    const std::string dumped = DumpRequestBody("test", body);
    CHECK(dumped.find("\xEF\xBF\xBD") != std::string::npos);  // U+FFFD 替换符
    // 出口必须能重新解析:parse 带 warn_unused_result,结果得有人接(抛了
    // 异常 doctest 自会判败,与 CHECK_NOTHROW 同效)。
    const bool reparsed = !nlohmann::json::parse(dumped).is_discarded();
    CHECK(reparsed);
    CHECK(dumped.find("中文夹一枚坏字节") != std::string::npos);  // 好字节原样保留
}

// ---------------------------------------------------------------------------
// 归一到 types.hpp 的共用小件
// ---------------------------------------------------------------------------

TEST_CASE("ApplyExtraHeaders: 空值删头、非空覆盖/追加") {
    std::map<std::string, std::string> base{{"Content-Type", "application/json"},
                                            {"Authorization", "Bearer old"}};
    auto merged = ApplyExtraHeaders(std::move(base), {{"X-Custom", "1"}, {"Authorization", "Bearer new"}});
    CHECK(merged.at("Authorization") == "Bearer new");
    CHECK(merged.at("X-Custom") == "1");
    CHECK(merged.at("Content-Type") == "application/json");

    auto pruned = ApplyExtraHeaders(std::move(merged), {{"X-Custom", ""}});
    CHECK(pruned.find("X-Custom") == pruned.end());
}

TEST_CASE("RoleToString: user 恒为 user,另一角按各家 wire 给名") {
    CHECK(RoleToString(lubancode::api::Role::User, "assistant") == "user");
    CHECK(RoleToString(lubancode::api::Role::Assistant, "assistant") == "assistant");
    CHECK(RoleToString(lubancode::api::Role::Assistant, "model") == "model");
    CHECK(RoleToString(lubancode::api::Role::User, "model") == "user");
}

TEST_CASE("MergeExtraBody: 顶层浅合并,非 object 不动,同名键整个覆盖") {
    nlohmann::json body;
    body["model"] = "m1";
    body["nested"] = nlohmann::json{{"keep", 1}};

    MergeExtraBody(body, nlohmann::json{{"model", "m2"}, {"nested", nlohmann::json{{"other", 2}}}});
    CHECK(body.at("model") == "m2");
    CHECK(body.at("nested") == nlohmann::json{{"other", 2}});  // 嵌套整个替换,不深合并
    CHECK(body.at("nested").contains("keep") == false);

    MergeExtraBody(body, nlohmann::json::array({1, 2}));  // 非 object:原样跳过
    MergeExtraBody(body, nlohmann::json());               // null:原样跳过
    CHECK(body.size() == 2);

    nlohmann::json additions;
    MergeExtraBody(additions, nlohmann::json{{"x", 1}});
    REQUIRE(additions.is_object());
    CHECK(additions.at("x") == 1);
}
