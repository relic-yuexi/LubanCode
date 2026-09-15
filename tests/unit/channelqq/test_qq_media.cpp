// QQ v2 富媒体传输册(QQ 接入单 Q4):附件受控下载与分片两步上传。
// 全程 mock HttpFunc,零网络。覆盖:
//   - 下载:url 安全校验(https/域族白名单/字面 IP/userinfo)、大小帽、
//     错误分型、凭据不进文案(query 带 token 的脱敏记法);
//   - 上传:官方口径的分片序列(upload_prepare -> 逐片 PUT(无 QQ 头)->
//     upload_part_finish -> files 合并拿 file_info)、MD5/SHA1 已知向量、
//     file_info 的 ttl 缓存与过期重传、失败分型、SDK/文档漂移的 index
//     两案(0 起官方与 1 起 SDK,偏移按数组序都不出错)。
// 真平台媒体行为(真 url 时效、真 media_id、真分片大小)归 Q3 真机,
// 本册只钉协议形状与本地合同——如实未验。
#include <doctest/doctest.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "channel/qq/qq_auth.hpp"
#include "channel/qq/qq_media.hpp"

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

    static bool HasHeader(const Call& call, const std::string& name, const std::string& prefix) {
        for (const auto& entry : call.headers) {
            if (entry.first == name && entry.second.rfind(prefix, 0) == 0) {
                return true;
            }
        }
        return false;
    }
};

struct Fixture {
    ScriptedHttp http;
    std::atomic<std::int64_t> now{10'000};
    std::optional<QqTokenManager> tokens;

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

    QqMediaUploader::Options UploaderOptions() {
        QqMediaUploader::Options options;
        options.http = http.Func();
        options.tokens = &*tokens;
        options.api_base = "https://api.test";
        options.now_ms = [this]() { return now.load(); };
        options.max_upload_bytes = 1024;
        return options;
    }
};

// 预置一枚已缓存的 token(先排响应再建 manager,首取即命中脚本)。
void PrimeToken(Fixture& fixture) {
    fixture.http.replies.push_back({200, R"({"access_token":"T1","expires_in":7200})"});
    fixture.tokens.emplace(fixture.TokenOptions());
}

}  // namespace

// ---------------------------------------------------------------------------
// 媒体 URL 卫生(纯函数)
// ---------------------------------------------------------------------------

TEST_CASE("qq_media: RedactMediaUrl 折叠 query 与 fragment") {
    CHECK(RedactMediaUrl("https://multimedia.nt.qq.com/download?appid=1&sign=TOKEN")
              == "https://multimedia.nt.qq.com/download?(redacted)");
    CHECK(RedactMediaUrl("https://host/path") == "https://host/path");
    CHECK(RedactMediaUrl("https://host/path#frag") == "https://host/path#(redacted)");
    CHECK(RedactMediaUrl("not a url") == "not a url");
}

TEST_CASE("qq_media: 下载 url 安全校验(https/白名单域族/字面 IP/userinfo)") {
    CHECK_FALSE(ValidateMediaDownloadUrl(
        "https://multimedia.nt.qq.com/download?sign=X",
        DefaultMediaHostAllowSuffixes()).has_value());
    CHECK_FALSE(ValidateMediaDownloadUrl(
        "https://IMG.example.qpic.cn/a.png",  // 大小写不敏感
        DefaultMediaHostAllowSuffixes()).has_value());
    CHECK_FALSE(ValidateMediaDownloadUrl(
        "https://bucket.cos.ap-guangzhou.myqcloud.com/file.bin",
        DefaultMediaHostAllowSuffixes()).has_value());
    // 非 https / 未知域 / 字面 IP(公网私网都拒)/ userinfo 一律拒。
    const auto insecure = ValidateMediaDownloadUrl(
        "http://multimedia.nt.qq.com/a", DefaultMediaHostAllowSuffixes());
    REQUIRE(insecure.has_value());
    CHECK(insecure->rfind("url_insecure", 0) == 0);
    CHECK(ValidateMediaDownloadUrl("https://evil.example.com/a",
                                   DefaultMediaHostAllowSuffixes())
              .has_value());
    CHECK(ValidateMediaDownloadUrl("https://112.34.5.6/a", DefaultMediaHostAllowSuffixes())
              .has_value());
    CHECK(ValidateMediaDownloadUrl("https://127.0.0.1/a", DefaultMediaHostAllowSuffixes())
              .has_value());
    CHECK(ValidateMediaDownloadUrl("https://user:pass@multimedia.nt.qq.com/a",
                                   DefaultMediaHostAllowSuffixes())
              .has_value());
    // 白名单后缀必须整段匹配:evil.qq.com.example.com 不算 qq.com 域。
    CHECK(ValidateMediaDownloadUrl("https://evil.qq.com.example.net/a",
                                   DefaultMediaHostAllowSuffixes())
              .has_value());
}

TEST_CASE("qq_media: MIME -> file_type 映射") {
    CHECK(QqFileTypeFromMimeType("image/png") == 1);
    CHECK(QqFileTypeFromMimeType("image/jpeg") == 1);
    CHECK(QqFileTypeFromMimeType("video/mp4") == 2);
    CHECK(QqFileTypeFromMimeType("voice") == 3);
    CHECK(QqFileTypeFromMimeType("audio/silk") == 3);
    CHECK(QqFileTypeFromMimeType("text/plain") == 4);
    CHECK(QqFileTypeFromMimeType("") == 4);
    CHECK(QqFileTypeFromMimeType("application/pdf") == 4);
}

// ---------------------------------------------------------------------------
// 附件下载
// ---------------------------------------------------------------------------

TEST_CASE("qq_media: 下载成功——GET 不带 Authorization,大小核字节数") {
    Fixture fixture;
    fixture.http.replies.push_back({200, "ABCDEF"});
    QqMediaDownloadLimits limits;
    const auto result = DownloadQqAttachment(
        fixture.http.Func(), "https://multimedia.nt.qq.com/download?token=SECTOK", limits);
    REQUIRE(result.has_value());
    CHECK(result->bytes == "ABCDEF");
    CHECK(result->size_bytes == 6);
    REQUIRE(fixture.http.calls.size() == 1);
    CHECK(fixture.http.calls[0].method == "GET");
    // 凭据在 url query(平台签名入口),不进请求头。
    CHECK_FALSE(ScriptedHttp::HasHeader(fixture.http.calls[0], "Authorization", "QQBot"));
}

TEST_CASE("qq_media: 下载帽与错误分型;文案不带 url query 的 token") {
    QqMediaDownloadLimits limits;
    limits.max_bytes = 4;
    Fixture over_cap;
    over_cap.http.replies.push_back({200, "TOO_LONG_BODY"});
    const auto too_large = DownloadQqAttachment(
        over_cap.http.Func(), "https://multimedia.nt.qq.com/d?token=SECTOK", limits);
    REQUIRE_FALSE(too_large.has_value());
    CHECK(too_large.error().code == "too_large");
    CHECK(too_large.error().detail.find("SECTOK") == std::string::npos);  // 脱敏

    Fixture network;
    network.http.replies.push_back({200, "", "http timeout"});
    const auto network_error = DownloadQqAttachment(
        network.http.Func(), "https://multimedia.nt.qq.com/d", limits);
    REQUIRE_FALSE(network_error.has_value());
    CHECK(network_error.error().code == "network_error");

    Fixture not_found;
    not_found.http.replies.push_back({404, ""});
    const auto missing = DownloadQqAttachment(
        not_found.http.Func(), "https://multimedia.nt.qq.com/gone?token=SECTOK", limits);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == "not_found");
    CHECK(missing.error().detail.find("SECTOK") == std::string::npos);

    Fixture server;
    server.http.replies.push_back({503, ""});
    const auto server_error = DownloadQqAttachment(
        server.http.Func(), "https://multimedia.nt.qq.com/d", limits);
    REQUIRE_FALSE(server_error.has_value());
    CHECK(server_error.error().code == "platform_error");

    Fixture empty;
    empty.http.replies.push_back({200, ""});
    const auto empty_body = DownloadQqAttachment(
        empty.http.Func(), "https://multimedia.nt.qq.com/d", limits);
    REQUIRE_FALSE(empty_body.has_value());
    CHECK(empty_body.error().code == "invalid_response");
}

// ---------------------------------------------------------------------------
// 分片两步上传
// ---------------------------------------------------------------------------

TEST_CASE("qq_media: 两步上传——prepare/PUT(无 QQ 头)/finish/files 序列与 MD5 向量") {
    Fixture fixture;
    PrimeToken(fixture);
    // upload_prepare:block_size=5,两片(index 从 0 起,官方口径)。
    fixture.http.replies.push_back({200, R"({
      "upload_id": "UP1", "block_size": "5",
      "parts": [{"index": 0, "presigned_url": "https://cos.test/p0?sign=A", "block_size": "5"},
                {"index": 1, "presigned_url": "https://cos.test/p1?sign=B", "block_size": "2"}],
      "upload_config": {"concurrency": 2, "retry_timeout": 300, "retry_delay": 1}
    })"});
    fixture.http.replies.push_back({200, ""});   // PUT 片 0
    fixture.http.replies.push_back({200, "{}"}); // finish 片 0
    fixture.http.replies.push_back({200, ""});   // PUT 片 1
    fixture.http.replies.push_back({200, "{}"}); // finish 片 1
    fixture.http.replies.push_back({200, R"({"file_uuid":"F1","file_info":"FILE_INFO_1","ttl":3600})"});

    QqMediaUploader uploader(fixture.UploaderOptions());
    const auto outcome = uploader.UploadFile("OPEN1", "note.txt", "text/plain", "abcdeXY");
    REQUIRE(outcome.status == QqMediaUploader::Outcome::Status::Uploaded);
    CHECK(outcome.file_info == "FILE_INFO_1");
    CHECK(outcome.ttl_secs == 3600);

    // 序列:token -> prepare -> PUT -> finish -> PUT -> finish -> files。
    REQUIRE(fixture.http.calls.size() == 7);
    CHECK(fixture.http.calls[1].url == "https://api.test/v2/users/OPEN1/upload_prepare");
    CHECK(fixture.http.calls[2].method == "PUT");
    CHECK(fixture.http.calls[2].url == "https://cos.test/p0?sign=A");
    // 预签名 URL 自带鉴权(COS 签名),不带 QQ Authorization(§十 10.1)。
    CHECK_FALSE(ScriptedHttp::HasHeader(fixture.http.calls[2], "Authorization", "QQBot"));
    CHECK(ScriptedHttp::HasHeader(fixture.http.calls[1], "Authorization", "QQBot T1"));
    CHECK(fixture.http.calls[4].url == "https://cos.test/p1?sign=B");
    // PUT 载荷按 parts 数组序取偏移:片 0 = 前 5 字节,片 1 = 余下 2 字节
    //(不按 index 值算偏移——0 起与 1 起两案同稳)。
    CHECK(fixture.http.calls[2].body == "abcde");
    CHECK(fixture.http.calls[4].body == "XY");
    CHECK(fixture.http.calls[6].url == "https://api.test/v2/users/OPEN1/files");

    // prepare 请求体:MD5/SHA1 真值(python hashlib 预算;md5_10m 短文件
    // = 整串 MD5)。
    const auto prepare = nlohmann::json::parse(fixture.http.calls[1].body);
    CHECK(prepare.at("file_type") == 4);
    CHECK(prepare.at("file_size") == "7");  // 官方:字符串
    CHECK(prepare.at("file_name") == "note.txt");
    CHECK(prepare.at("md5") == "0beeb51e99e741359c217e71e752fee6");  // md5("abcdeXY")
    CHECK(prepare.at("sha1") == "ea559aa1942dfd773b3a7bb4e5e24d9fe80f7274");
    CHECK(prepare.at("md5_10m") == "0beeb51e99e741359c217e71e752fee6");
    // files 合并:srv_send_msg 恒 false。
    const auto merge = nlohmann::json::parse(fixture.http.calls[6].body);
    CHECK(merge.at("srv_send_msg") == false);
    CHECK(merge.at("upload_id") == "UP1");
    // finish 片 0:block_size 是该片实际字节(字符串),md5 是该片 MD5。
    const auto finish0 = nlohmann::json::parse(fixture.http.calls[3].body);
    CHECK(finish0.at("part_index") == 0);
    CHECK(finish0.at("block_size") == "5");
    CHECK(finish0.at("md5") == "ab56b4d92b40713acc5af89985d4b786");  // md5("abcde")
}

TEST_CASE("qq_media: SDK 漂移案——index 从 1 起照样按数组序上传") {
    Fixture fixture;
    PrimeToken(fixture);
    fixture.http.replies.push_back({200, R"({
      "upload_id": "UP9", "block_size": "5",
      "parts": [{"index": 1, "presigned_url": "https://cos.test/q0?sign=A", "block_size": "5"},
                {"index": 2, "presigned_url": "https://cos.test/q1?sign=B", "block_size": "2"}]
    })"});
    fixture.http.replies.push_back({200, ""});
    fixture.http.replies.push_back({200, "{}"});
    fixture.http.replies.push_back({200, ""});
    fixture.http.replies.push_back({200, "{}"});
    fixture.http.replies.push_back({200, R"({"file_uuid":"F2","file_info":"FILE_INFO_2","ttl":0})"});

    QqMediaUploader uploader(fixture.UploaderOptions());
    const auto outcome = uploader.UploadFile("OPEN1", "n.txt", "text/plain", "abcdeXY");
    REQUIRE(outcome.status == QqMediaUploader::Outcome::Status::Uploaded);
    CHECK(outcome.file_info == "FILE_INFO_2");
    CHECK(outcome.ttl_secs == 0);  // 官方:0 = 长期
    // 偏移仍按数组序:片 0 = 前 5 字节(SDK 的 (index-1)*block 同结果;
    // index 若从 1 错当 0 起会出负偏移——这里钉死两案同稳)。
    REQUIRE(fixture.http.calls.size() == 7);
    CHECK(fixture.http.calls[2].body == "abcde");
    CHECK(fixture.http.calls[4].body == "XY");
    // part_finish 回显平台原 index(1/2),不自作主张改写。
    const auto finish0 = nlohmann::json::parse(fixture.http.calls[3].body);
    CHECK(finish0.at("part_index") == 1);
    const auto finish1 = nlohmann::json::parse(fixture.http.calls[5].body);
    CHECK(finish1.at("part_index") == 2);
}

TEST_CASE("qq_media: file_info 缓存——同内容 ttl 窗内零网络,过期重传") {
    Fixture fixture;
    PrimeToken(fixture);
    fixture.http.replies.push_back({200, R"({
      "upload_id": "UP1", "block_size": "5",
      "parts": [{"index": 0, "presigned_url": "https://cos.test/p0?sign=A", "block_size": "5"}]
    })"});
    fixture.http.replies.push_back({200, ""});
    fixture.http.replies.push_back({200, "{}"});
    fixture.http.replies.push_back({200, R"({"file_uuid":"F1","file_info":"FI","ttl":10})"});

    QqMediaUploader uploader(fixture.UploaderOptions());
    const auto first = uploader.UploadFile("OPEN1", "a.txt", "text/plain", "hello");
    REQUIRE(first.status == QqMediaUploader::Outcome::Status::Uploaded);
    const std::size_t calls_after_first = fixture.http.calls.size();
    CHECK(uploader.cached_file_info_count() == 1);

    // 同内容重取:缓存命中,零网络(token 都不碰)。
    fixture.now += 5'000;  // 5s < ttl 10s
    const auto cached = uploader.UploadFile("OPEN1", "a.txt", "text/plain", "hello");
    REQUIRE(cached.status == QqMediaUploader::Outcome::Status::Uploaded);
    CHECK(cached.file_info == "FI");
    CHECK(fixture.http.calls.size() == calls_after_first);

    // 过期(ttl 10s 已过):重传同一原件,重新走全流程(token T1 仍在
    // 7200s 有效窗内,不重取——脚本只排媒体四步)。
    fixture.http.replies.push_back({200, R"({
      "upload_id": "UP2", "block_size": "5",
      "parts": [{"index": 0, "presigned_url": "https://cos.test/p0b?sign=A", "block_size": "5"}]
    })"});
    fixture.http.replies.push_back({200, ""});
    fixture.http.replies.push_back({200, "{}"});
    fixture.http.replies.push_back({200, R"({"file_uuid":"F2","file_info":"FI2","ttl":0})"});
    fixture.now += 20'000;
    const auto reuploaded = uploader.UploadFile("OPEN1", "a.txt", "text/plain", "hello");
    REQUIRE(reuploaded.status == QqMediaUploader::Outcome::Status::Uploaded);
    CHECK(reuploaded.file_info == "FI2");
    CHECK(fixture.http.calls.size() > calls_after_first);
}

TEST_CASE("qq_media: 上传失败分型(大小帽/prepare 永久拒/PUT 5xx 退避)") {
    // 超宿主帽:零请求明败。
    {
        Fixture fixture;
        PrimeToken(fixture);
        QqMediaUploader::Options options = fixture.UploaderOptions();
        options.max_upload_bytes = 4;
        QqMediaUploader uploader(options);
        const auto outcome = uploader.UploadFile("OPEN1", "a.txt", "text/plain", "toolarge");
        CHECK(outcome.status == QqMediaUploader::Outcome::Status::PermanentFail);
        CHECK(fixture.http.calls.empty());
    }
    // prepare 平台错误 850031(超大小):永久拒,不重试。
    {
        Fixture fixture;
        PrimeToken(fixture);
        fixture.http.replies.push_back(
            {200, R"({"code":850031,"message":"上传文件超过大小限制"})"});
        QqMediaUploader uploader(fixture.UploaderOptions());
        const auto outcome = uploader.UploadFile("OPEN1", "a.txt", "text/plain", "hello");
        CHECK(outcome.status == QqMediaUploader::Outcome::Status::PermanentFail);
        CHECK(outcome.error.kind == QqApiErrorKind::ContentRejected);
    }
    // PUT 5xx:可重试(DeferredRetry);失败文案的 url 脱敏(签名不进日志)。
    {
        Fixture fixture;
        PrimeToken(fixture);
        fixture.http.replies.push_back({200, R"({
          "upload_id": "UP1", "block_size": "5",
          "parts": [{"index": 0, "presigned_url": "https://cos.test/p0?sign=A", "block_size": "5"}]
        })"});
        fixture.http.replies.push_back({503, ""});
        QqMediaUploader uploader(fixture.UploaderOptions());
        const auto outcome = uploader.UploadFile("OPEN1", "a.txt", "text/plain", "hello");
        CHECK(outcome.status == QqMediaUploader::Outcome::Status::DeferredRetry);
        CHECK(outcome.error.kind == QqApiErrorKind::ServerError);
        CHECK(outcome.error.detail.find("sign=A") == std::string::npos);
        CHECK(outcome.error.detail.find("cos.test") != std::string::npos);
    }
    // files 合并 40093001(BDH 通道异常):官方建议重试。
    {
        Fixture fixture;
        PrimeToken(fixture);
        fixture.http.replies.push_back({200, R"({
          "upload_id": "UP1", "block_size": "5",
          "parts": [{"index": 0, "presigned_url": "https://cos.test/p0?sign=A", "block_size": "5"}]
        })"});
        fixture.http.replies.push_back({200, ""});
        fixture.http.replies.push_back({200, "{}"});
        fixture.http.replies.push_back({200, R"({"code":40093001,"message":"BDH 通道异常"})"});
        QqMediaUploader uploader(fixture.UploaderOptions());
        const auto outcome = uploader.UploadFile("OPEN1", "a.txt", "text/plain", "hello");
        CHECK(outcome.status == QqMediaUploader::Outcome::Status::DeferredRetry);
    }
    // 空文件/空名:前置明败。
    {
        Fixture fixture;
        PrimeToken(fixture);
        QqMediaUploader uploader(fixture.UploaderOptions());
        const auto empty = uploader.UploadFile("OPEN1", "a.txt", "text/plain", "");
        CHECK(empty.status == QqMediaUploader::Outcome::Status::PermanentFail);
        const auto no_name = uploader.UploadFile("OPEN1", "", "text/plain", "x");
        CHECK(no_name.status == QqMediaUploader::Outcome::Status::PermanentFail);
    }
}

TEST_CASE("qq_media: MD5/SHA-1 内核向量(经 prepare 请求体钉死标准值)") {
    Fixture fixture;
    PrimeToken(fixture);
    fixture.http.replies.push_back({200, R"({
      "upload_id": "UP1", "block_size": "16",
      "parts": [{"index": 0, "presigned_url": "https://cos.test/p?sign=A", "block_size": "16"}]
    })"});
    fixture.http.replies.push_back({200, ""});
    fixture.http.replies.push_back({200, "{}"});
    fixture.http.replies.push_back({200, R"({"file_uuid":"F","file_info":"FI","ttl":0})"});
    QqMediaUploader uploader(fixture.UploaderOptions());
    const auto outcome = uploader.UploadFile("OPEN1", "v.txt", "text/plain", "abc");
    REQUIRE(outcome.status == QqMediaUploader::Outcome::Status::Uploaded);
    const auto prepare = nlohmann::json::parse(fixture.http.calls[1].body);
    // RFC 1321 / FIPS 180-1 标准测试向量(md5("abc") / sha1("abc"))。
    CHECK(prepare.at("md5") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(prepare.at("sha1") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(prepare.at("md5_10m") == "900150983cd24fb0d6963f7d28e17f72");
}

}  // namespace lubancode::channel::qq
