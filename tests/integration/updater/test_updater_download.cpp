// 更新助手 C++ 化·批二第②单:断点续传下载器册。全册只打本机回环假服务
// (fake_http_server),不碰真网。覆盖(对齐 scripts/updater.py
// download_with_resume 的护栏与语义):
//   - 206 续传:预置 .part 前缀 → 请求头带 Range: bytes=N- + UA,落盘 =
//     前缀+余段,Content-Range 错位/总长对不上当场报;
//   - 200 重下:服务端不认 Range 回 200 → 从头写(旧 .part 作废);
//   - 中途断流:半截应答 → 重试(退避注 0)→ 第二轮续传成功;三轮全断
//     → 耗尽报"下载失败(重试 3 次)",.part 留作续传点;
//   - 5xx 属网络层:重试后成功;
//   - 坏摘要:收尾整文件对账红 → 删 .part;
//   - 硬顶:小 cap 注入 → 逐块验帽,超帽即断删件;
//   - 声明大小不符:Content-Length 对不上 → 头阶段就断;416 空续传边界
//     (起点恰是声明大小)对账放行,起点超出则报错删件;
//   - 进度回调:16 MiB 节拍 + 成功收尾终值一次;
//   - 取消旗:预置位零连接;流中置位即断,.part 字节数如实。
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fake_http_server.hpp"
#include "platform/sha256.hpp"
#include "updater/download.hpp"

namespace {

namespace fs = std::filesystem;
using lubancode::test_support::FakeHttpRequest;
using lubancode::test_support::FakeHttpResponse;
using lubancode::test_support::FakeHttpServer;
using lubancode::updater::DownloadOptions;
using lubancode::updater::DownloadWithResume;

fs::path TempRoot(const std::string& name) {
    const fs::path root = fs::temp_directory_path() / ("lubancode-updater-dl-" + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

void WriteFile(const fs::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string Url(const FakeHttpServer& server) {
    return "http://127.0.0.1:" + std::to_string(server.port()) + "/pkg.zip";
}

std::optional<std::string> HeaderOf(const FakeHttpRequest& request, const std::string& name) {
    for (const auto& [key, value] : request.headers) {  // 夹具已把名字小写化
        if (key == name) return value;
    }
    return std::nullopt;
}

// 测试档:退避注 0(CI 不真睡 2^n)、停滞窗口收小(半截流场景 1 秒内
// 断线重试,不陪假服务睡满 10 秒)、连接上限 5 秒。
DownloadOptions FastOptions() {
    DownloadOptions options;
    options.max_retries = 3;
    options.backoff_base_secs = 0.0;
    options.connect_timeout_secs = 5;
    options.low_speed_limit = 1;
    options.low_speed_window_secs = 1;
    return options;
}

FakeHttpResponse Ok200(std::string body) {
    FakeHttpResponse response;
    response.status = 200;
    response.body = std::move(body);
    return response;
}

}  // namespace

TEST_CASE("updater.download:206 续传——Range 头、前缀+余段落盘、UA 上门牌") {
    const fs::path root = TempRoot("resume-206");
    const fs::path dest = root / "pkg.zip";
    const std::string prefix = "PREFIXAB";  // 8 字节
    const std::string suffix = "SUFFIXCD";  // 8 字节
    const std::string full = prefix + suffix;
    WriteFile(fs::path(dest) += ".part", prefix);

    FakeHttpServer server;
    FakeHttpResponse partial;
    partial.status = 206;
    partial.headers.emplace_back("Content-Range", "bytes 8-15/16");
    partial.body = suffix;
    server.Enqueue(partial);

    const auto result = DownloadWithResume(Url(server), dest, lubancode::platform::Sha256Hex(full),
                                           full.size(), FastOptions(), nullptr, nullptr);
    REQUIRE(result.has_value());
    CHECK(ReadFile(dest) == full);
    CHECK_FALSE(fs::exists(fs::path(dest) += ".part"));

    const std::vector<FakeHttpRequest> requests = server.requests();
    REQUIRE(requests.size() == 1);
    CHECK(HeaderOf(requests[0], "range") == std::optional<std::string>("bytes=8-"));
    CHECK(HeaderOf(requests[0], "user-agent") == std::optional<std::string>("lubancode-updater"));
}

TEST_CASE("updater.download:200 重下——服务端不认 Range,旧 .part 作废从头写") {
    const fs::path root = TempRoot("restart-200");
    const fs::path dest = root / "pkg.zip";
    WriteFile(fs::path(dest) += ".part", "STALE-OLD");

    FakeHttpServer server;
    server.Enqueue(Ok200("FRESH"));

    const auto result = DownloadWithResume(Url(server), dest, lubancode::platform::Sha256Hex("FRESH"),
                                           std::nullopt, FastOptions(), nullptr, nullptr);
    REQUIRE(result.has_value());
    CHECK(ReadFile(dest) == "FRESH");

    const std::vector<FakeHttpRequest> requests = server.requests();
    REQUIRE(requests.size() == 1);
    CHECK(HeaderOf(requests[0], "range") == std::optional<std::string>("bytes=9-"));
}

TEST_CASE("updater.download:中途断流——半截应答断线重试,第二轮续传补齐") {
    const fs::path root = TempRoot("retry-resume");
    const fs::path dest = root / "pkg.zip";
    const std::string full = "0123456789";

    FakeHttpServer server;
    // 第一轮:200 声明 10 字节,只发 5 字节就挂死(假服务睡 10s 后关连接,
    // 客户端停滞探测 1s 先断)。
    FakeHttpResponse truncated;
    truncated.status = 200;
    truncated.body = full;
    truncated.stall_after_body_bytes = 5;
    server.Enqueue(truncated);
    // 第二轮:.part 已有 5 字节 → 带 Range 续传,206 补余段。
    FakeHttpResponse rest;
    rest.status = 206;
    rest.headers.emplace_back("Content-Range", "bytes 5-9/10");
    rest.body = full.substr(5);
    server.Enqueue(rest);

    const auto result = DownloadWithResume(Url(server), dest, lubancode::platform::Sha256Hex(full),
                                           full.size(), FastOptions(), nullptr, nullptr);
    REQUIRE(result.has_value());
    CHECK(ReadFile(dest) == full);

    const std::vector<FakeHttpRequest> requests = server.requests();
    REQUIRE(requests.size() == 2);
    CHECK_FALSE(HeaderOf(requests[0], "range").has_value());
    CHECK(HeaderOf(requests[1], "range") == std::optional<std::string>("bytes=5-"));
}

TEST_CASE("updater.download:重试耗尽——三轮全断报总账,.part 留作续传点") {
    const fs::path root = TempRoot("retry-exhausted");
    const fs::path dest = root / "pkg.zip";
    const fs::path partial = fs::path(dest) += ".part";

    FakeHttpServer server;
    for (int i = 0; i < 3; ++i) {
        FakeHttpResponse truncated;
        truncated.status = 200;
        truncated.body = "0123456789";
        truncated.stall_after_body_bytes = 4;
        server.Enqueue(truncated);
    }

    const auto result = DownloadWithResume(Url(server), dest, lubancode::platform::Sha256Hex("0123456789"),
                                           std::nullopt, FastOptions(), nullptr, nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("下载失败(重试 3 次)") != std::string::npos);
    CHECK(server.requests().size() == 3);
    // 三轮都断在半路,.part 留着最后一轮已收的 4 字节(python 同款:网络
    // 失败不删件,给下一程续传)。
    CHECK(fs::exists(partial));
    CHECK(ReadFile(partial) == "0123");
    CHECK_FALSE(fs::exists(dest));
}

TEST_CASE("updater.download:5xx 属网络层——重试后成功") {
    const fs::path root = TempRoot("retry-5xx");
    const fs::path dest = root / "pkg.zip";

    FakeHttpServer server;
    FakeHttpResponse boom;
    boom.status = 500;
    boom.body = "upstream exploded";
    server.Enqueue(boom);
    server.Enqueue(Ok200("PKG"));

    const auto result = DownloadWithResume(Url(server), dest, lubancode::platform::Sha256Hex("PKG"),
                                           std::nullopt, FastOptions(), nullptr, nullptr);
    REQUIRE(result.has_value());
    CHECK(ReadFile(dest) == "PKG");
    CHECK(server.requests().size() == 2);
}

TEST_CASE("updater.download:4xx 不重试——404 直接报") {
    const fs::path root = TempRoot("no-retry-4xx");
    const fs::path dest = root / "pkg.zip";

    FakeHttpServer server;
    FakeHttpResponse missing;
    missing.status = 404;
    missing.body = "no such asset";
    server.Enqueue(missing);

    const auto result = DownloadWithResume(Url(server), dest, std::string(64, '0'),
                                           std::nullopt, FastOptions(), nullptr, nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("下载返回 HTTP 404") != std::string::npos);
    CHECK(result.error().find("no such asset") != std::string::npos);
    CHECK(server.requests().size() == 1);
    CHECK_FALSE(fs::exists(dest));
}

TEST_CASE("updater.download:坏摘要——整文件对账红,删 .part 报错") {
    const fs::path root = TempRoot("bad-digest");
    const fs::path dest = root / "pkg.zip";
    const fs::path partial = fs::path(dest) += ".part";

    FakeHttpServer server;
    server.Enqueue(Ok200("AAA"));

    const auto result = DownloadWithResume(Url(server), dest, lubancode::platform::Sha256Hex("BBB"),
                                           std::nullopt, FastOptions(), nullptr, nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("摘要不符") != std::string::npos);
    CHECK(result.error().find(lubancode::platform::Sha256Hex("AAA")) != std::string::npos);
    CHECK_FALSE(fs::exists(partial));
    CHECK_FALSE(fs::exists(dest));
}

TEST_CASE("updater.download:硬顶——小 cap 注入,逐块验帽超帽即弃") {
    const fs::path root = TempRoot("hard-cap");
    const fs::path dest = root / "pkg.zip";
    const fs::path partial = fs::path(dest) += ".part";

    FakeHttpServer server;
    server.Enqueue(Ok200("TEN-BYTES"));

    DownloadOptions options = FastOptions();
    options.hard_cap_bytes = 5;
    const auto result = DownloadWithResume(Url(server), dest, lubancode::platform::Sha256Hex("TEN-BYTES"),
                                           std::nullopt, options, nullptr, nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("下载超过大小上限(5 字节") != std::string::npos);
    CHECK_FALSE(fs::exists(partial));
    CHECK_FALSE(fs::exists(dest));
    CHECK(server.requests().size() == 1);  // 超帽是确定性错误,不重试
}

TEST_CASE("updater.download:声明大小不符——Content-Length 对不上,头阶段就断") {
    const fs::path root = TempRoot("size-mismatch");
    const fs::path dest = root / "pkg.zip";
    const fs::path partial = fs::path(dest) += ".part";

    FakeHttpServer server;
    FakeHttpResponse mismatch;
    mismatch.status = 200;
    mismatch.headers.emplace_back("Content-Length", "10");  // 清单只声明 8
    mismatch.body = "TEN-BYTES";
    server.Enqueue(mismatch);

    const auto result = DownloadWithResume(Url(server), dest, lubancode::platform::Sha256Hex("TEN-BYTES"),
                                           8, FastOptions(), nullptr, nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("声明大小对不上") != std::string::npos);
    CHECK(result.error().find("Content-Length 10") != std::string::npos);
    CHECK_FALSE(fs::exists(partial));
    CHECK_FALSE(fs::exists(dest));
    CHECK(server.requests().size() == 1);
}

TEST_CASE("updater.download:416 空续传边界——起点恰是声明大小则对账放行") {
    const fs::path root = TempRoot("boundary-416");
    const fs::path dest = root / "pkg.zip";
    const std::string full = "COMPLETE10";

    SUBCASE("已收全:416 后整文件对账,过门改名") {
        WriteFile(fs::path(dest) += ".part", full);
        FakeHttpServer server;
        FakeHttpResponse unsatisfied;
        unsatisfied.status = 416;
        unsatisfied.body = "range not satisfiable";
        server.Enqueue(unsatisfied);

        const auto result = DownloadWithResume(Url(server), dest, lubancode::platform::Sha256Hex(full),
                                               full.size(), FastOptions(), nullptr, nullptr);
        REQUIRE(result.has_value());
        CHECK(ReadFile(dest) == full);
        const std::vector<FakeHttpRequest> requests = server.requests();
        REQUIRE(requests.size() == 1);
        CHECK(HeaderOf(requests[0], "range") == std::optional<std::string>("bytes=10-"));
    }

    SUBCASE("起点超出服务端文件:报错删件,不重试") {
        WriteFile(fs::path(dest) += ".part", "SHORT");
        FakeHttpServer server;
        FakeHttpResponse unsatisfied;
        unsatisfied.status = 416;
        unsatisfied.body = "range not satisfiable";
        server.Enqueue(unsatisfied);

        const auto result = DownloadWithResume(Url(server), dest, lubancode::platform::Sha256Hex(full),
                                               full.size(), FastOptions(), nullptr, nullptr);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("HTTP 416") != std::string::npos);
        CHECK_FALSE(fs::exists(fs::path(dest) += ".part"));
        CHECK_FALSE(fs::exists(dest));
        CHECK(server.requests().size() == 1);
    }
}

TEST_CASE("updater.download:进度回调——16MiB 节拍与成功终值") {
    const fs::path root = TempRoot("progress");
    const fs::path dest = root / "pkg.zip";
    const std::uint64_t mark = 16ull << 20;
    const std::string big(static_cast<std::size_t>(mark) + 1, 'x');

    FakeHttpServer server;
    server.Enqueue(Ok200(big));

    std::vector<std::uint64_t> seen;
    const auto result = DownloadWithResume(
        Url(server), dest, lubancode::platform::Sha256Hex(big), big.size(), FastOptions(), nullptr,
        [&seen](std::uint64_t bytes) { seen.push_back(bytes); });
    REQUIRE(result.has_value());
    REQUIRE(seen.size() >= 2);  // 16MiB 节拍一次 + 终值一次
    // 节拍值是"跨过标记那一刻的累计字节"(python 同款,分块可能略过线),
    // 只钉区间;终值钉死。
    CHECK(seen.front() >= mark);
    CHECK(seen.front() < 2 * mark);
    CHECK(seen.back() == big.size());
}

TEST_CASE("updater.download:取消旗——预置位零连接;流中置位即断,.part 如实") {
    const fs::path root = TempRoot("cancel");
    const fs::path dest = root / "pkg.zip";
    const fs::path partial = fs::path(dest) += ".part";

    SUBCASE("预置位:一个包都不发") {
        FakeHttpServer server;
        server.Enqueue(Ok200("NEVER"));
        std::atomic<bool> cancel{true};
        const auto result = DownloadWithResume(Url(server), dest, std::string(64, '0'), std::nullopt,
                                               FastOptions(), &cancel, nullptr);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("取消") != std::string::npos);
        CHECK(server.connection_count() == 0);
        CHECK_FALSE(fs::exists(partial));
    }

    SUBCASE("流中置位:半截字节如实落 .part") {
        FakeHttpServer server;
        FakeHttpResponse stalled;
        stalled.status = 200;
        stalled.body = "0123456789";
        stalled.stall_after_body_bytes = 5;  // 5 字节后挂死,等取消落锤
        server.Enqueue(stalled);
        std::atomic<bool> cancel{false};
        std::thread canceller([&cancel]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            cancel.store(true);
        });
        const auto result = DownloadWithResume(Url(server), dest, std::string(64, '0'), std::nullopt,
                                               FastOptions(), &cancel, nullptr);
        canceller.join();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("取消") != std::string::npos);
        CHECK(fs::exists(partial));
        CHECK(ReadFile(partial) == "01234");
        CHECK_FALSE(fs::exists(dest));
    }
}

TEST_CASE("updater.download:206 续段错位——Content-Range 起点对不上即报") {
    const fs::path root = TempRoot("range-misaligned");
    const fs::path dest = root / "pkg.zip";
    WriteFile(fs::path(dest) += ".part", "PREFIXAB");

    FakeHttpServer server;
    FakeHttpResponse misaligned;
    misaligned.status = 206;
    misaligned.headers.emplace_back("Content-Range", "bytes 5-15/16");  // 要的是 8-
    misaligned.body = "XXSUFFIXCD";
    server.Enqueue(misaligned);

    const auto result = DownloadWithResume(Url(server), dest, lubancode::platform::Sha256Hex("PREFIXABSUFFIXCD"),
                                           16, FastOptions(), nullptr, nullptr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("206 续段错位") != std::string::npos);
    CHECK_FALSE(fs::exists(fs::path(dest) += ".part"));
    CHECK(server.requests().size() == 1);
}
