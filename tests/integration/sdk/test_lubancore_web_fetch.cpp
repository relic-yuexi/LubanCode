#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "fake_http_server.hpp"

namespace lubancore_consumer {
void WebFetchCase(const std::string&, const std::filesystem::path&, const std::string&,
                  const std::function<bool(const std::string&, const std::string&)>&);
}
namespace {
namespace fs = std::filesystem;
using lubancode::test_support::FakeHttpResponse;
using lubancode::test_support::FakeHttpServer;
struct Directory {
    fs::path path;
    Directory() {
        static std::atomic<unsigned> next{0};
        path = fs::temp_directory_path() / ("sdk-web-fetch-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++next));
    }
    ~Directory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
FakeHttpResponse Text(std::string body, int status = 200) {
    FakeHttpResponse response; response.status = status;
    response.headers = {{"Content-Type", "text/plain"}}; response.body = std::move(body); return response;
}
FakeHttpResponse Redirect(std::string target, std::string body = {}) {
    FakeHttpResponse response; response.status = 302;
    response.headers = {{"Location", std::move(target)}}; response.body = std::move(body); return response;
}
std::vector<std::string> Targets(const std::string& name) {
    if (name == "admission") return {"/admission/plain"};
    if (name == "content") return {"/content/html", "/content/utf8", "/content/binary", "/content/status"};
    if (name == "limits") return {"/limits/exact", "/limits/body", "/limits/gzip", "/limits/header", "/limits/output",
        "/limits/cumulative-body", "/limits/body-final", "/limits/cumulative-header", "/limits/header-final"};
    if (name == "redirects") return {"/redirects/relative", "/redirects/final", "/redirects/loop-a", "/redirects/loop-b",
        "/redirects/limit", "/redirects/scheme", "/redirects/duplicate"};
    if (name == "cancel") return {"/cancel/wait", "/cancel/close", "/cancel/shutdown", "/cancel/timeout"};
    if (name == "isolation") return {"/isolation/0", "/isolation/1", "/isolation/2", "/isolation/3", "/isolation/1"};
    FAIL("unknown web fetch case"); return {};
}
FakeHttpResponse Response(const std::string& target) {
    if (target == "/admission/plain") return Text("web-fetch-owned-body");
    if (target == "/content/html") {
        auto value = Text("<style>badstyle</style><h1>Page</h1><p>hello &amp; world</p><script>SECRET_SCRIPT</script><p>中文</p>");
        value.headers = {{"Content-Type", "text/html"}}; return value;
    }
    if (target == "/content/utf8") return Text("中文");
    if (target == "/content/binary") return Text(std::string("a\0b", 3));
    if (target == "/content/status") return Text("not found", 404);
    if (target == "/limits/exact") return Text(std::string(128, 'E'));
    if (target == "/limits/body") return Text(std::string(129, 'E'));
    if (target == "/limits/gzip") {
        // gzip of 8192 'G' bytes. The 44 wire bytes fit 128, decoded bytes do not.
        const std::string hex = "1f8b08000000000002ffedc1010d000000c2a078efdfc81c6e400100000000000000ef06326659fd00200000";
        std::string compressed;
        for (std::size_t i = 0; i < hex.size(); i += 2)
            compressed.push_back(static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        auto value = Text(std::move(compressed)); value.headers.emplace_back("Content-Encoding", "gzip"); return value;
    }
    if (target == "/limits/header") { auto value = Text(""); value.headers.emplace_back("X-Large", std::string(4096, 'H')); return value; }
    if (target == "/limits/output") return Text(std::string(1024, 'U'));
    if (target == "/limits/cumulative-body") return Redirect("/limits/body-final", std::string(80, 'B'));
    if (target == "/limits/body-final") return Text(std::string(80, 'B'));
    if (target == "/limits/cumulative-header") return Redirect("/limits/header-final");
    if (target == "/limits/header-final") { auto value = Text("OK"); value.headers = {{"X-Pad", std::string(60, 'P')}}; return value; }
    if (target == "/redirects/relative") return Redirect("final");
    if (target == "/redirects/final") return Text("relative-final");
    if (target == "/redirects/loop-a") return Redirect("/redirects/loop-b");
    if (target == "/redirects/loop-b") return Redirect("/redirects/loop-a");
    if (target == "/redirects/limit") return Redirect("/redirects/unvisited");
    if (target == "/redirects/scheme") return Redirect("file:///not-an-http-resource");
    if (target == "/redirects/duplicate") { auto value = Redirect("/one"); value.headers.emplace_back("Location", "/two"); return value; }
    if (target.starts_with("/cancel/")) { auto value = Text("too late"); value.delay_before_response = std::chrono::seconds(15); return value; }
    if (target.starts_with("/isolation/")) return Text(std::string(256, 'I'));
    FAIL("unknown HTTP fixture target"); return Text("unexpected fixture target", 500);
}
void Run(const std::string& name) {
    Directory directory; FakeHttpServer server(FakeHttpServer::ThreadMode::Owned); REQUIRE(server.port() > 0);
    auto expected = Targets(name);
    for (const auto& target : expected) server.Enqueue(Response(target));
    const auto observed = [&](const std::string& target, const std::string& user_agent) {
        for (const auto& request : server.requests()) if (request.method == "GET" && request.target == target)
            for (const auto& [key, value] : request.headers) if (key == "user-agent" && value == user_agent) return true;
        return false;
    };
    REQUIRE_NOTHROW(lubancore_consumer::WebFetchCase(name, directory.path,
        "http://127.0.0.1:" + std::to_string(server.port()), observed));
    const auto actual = server.requests();
    REQUIRE(actual.size() == expected.size());
    std::vector<std::string> targets;
    for (const auto& request : actual) { REQUIRE(request.method == "GET"); targets.push_back(request.target); }
    if (name == "isolation") { std::sort(targets.begin(), targets.end()); std::sort(expected.begin(), expected.end()); }
    REQUIRE(targets == expected);
    const auto stop_started = std::chrono::steady_clock::now();
    server.StopAndJoin();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stop_started).count();
    const bool quiescent = server.owned_threads_quiescent();
    REQUIRE(quiescent);
    REQUIRE(elapsed < 3000);
    std::cout << "[sdk-web-fetch-fixture] {\"path\":\"" << name << "\",\"quiescent\":"
              << (quiescent ? "true" : "false") << ",\"requests\":" << actual.size()
              << ",\"stop_elapsed_ms\":" << elapsed << "}\n";
}
} // namespace

TEST_CASE("SDK web fetch admits only explicit bounded host declarations") { Run("admission"); }
TEST_CASE("SDK web fetch returns actual cleaned HTML and complete UTF-8 prefixes") { Run("content"); }
TEST_CASE("SDK web fetch bounds actual headers decoded bodies and cumulative redirects") { Run("limits"); }
TEST_CASE("SDK web fetch follows only finite valid relative HTTP redirects") { Run("redirects"); }
TEST_CASE("SDK web fetch cancels actual pending HTTP before Close and Shutdown return") { Run("cancel"); }
TEST_CASE("SDK web fetch isolates four live Session limits and returned owners") { Run("isolation"); }
