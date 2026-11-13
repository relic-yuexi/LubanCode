#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "fake_http_server.hpp"

namespace lubancore_consumer {
void WebSearchCase(const std::string&, const std::filesystem::path&, const std::string&,
                   const std::function<bool(const std::string&, const std::string&)>&);
}
namespace {
using namespace lubancode::test_support;
struct Directory {
    std::filesystem::path path;
    Directory() {
        static std::atomic<unsigned> serial{0};
        path = std::filesystem::temp_directory_path() / ("sdk-web-search-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
    }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
FakeHttpResponse Response(std::string body, int status = 200) {
    FakeHttpResponse response;
    response.status = status; response.body = std::move(body);
    response.headers = {{"Content-Type", "application/json"}};
    return response;
}
const std::string tavily = R"({"results":[{"title":"搜索结果","url":"https://example.com","content":"中文摘要"},{"title":"second"},{"title":"third"}]})";
void Run(const std::string& name) {
    Directory directory;
    FakeHttpServer server(FakeHttpServer::ThreadMode::Owned);
    REQUIRE(server.port() > 0);
    if (name == "providers") {
        server.Enqueue(Response(tavily));
        server.Enqueue(Response(R"({"web":{"results":[{"title":"搜索结果","url":"https://example.com","description":"摘要"}]}})"));
        server.Enqueue(Response(R"({"organic":[{"title":"搜索结果","link":"https://example.com","snippet":"摘要"}]})"));
    } else if (name == "lifecycle") {
        server.Enqueue(Response(std::string(2048, 'B')));
        server.Enqueue(Response("fixture-search-key:invalid-json"));
        server.Enqueue(Response("fixture-search-key:http-failure", 500));
        auto redirect = Response("", 302); redirect.headers.emplace_back("Location", "/unvisited");
        server.Enqueue(std::move(redirect));
        auto header = Response("{}"); header.headers.emplace_back("X-Large", std::string(4096, 'H'));
        server.Enqueue(std::move(header));
        auto delay = Response(tavily); delay.delay_before_response = std::chrono::seconds(15);
        server.Enqueue(delay);
        server.Enqueue(Response("{\"results\":[{\"title\":\"" + std::string(2000, 'U') + "中文\"}]}"));
        server.Enqueue(delay);
        server.Enqueue(Response(tavily)); server.Enqueue(Response(tavily));
        server.Enqueue(Response(tavily)); server.Enqueue(Response(tavily));
    }
    const auto observed = [&](const std::string& path, const std::string& mode) {
        const auto requests = server.requests();
        for (const auto& request : requests) {
            if (!request.target.starts_with(path)) continue;
            if (mode == "entered") return true;
            std::string key;
            for (const auto& [header, value] : request.headers) {
                if (header == "authorization") key = value;
                if (header == "x-subscription-token" || header == "x-api-key") key = value;
            }
            if (mode == "resumed") { if (key == "Bearer resumed-search-key") return true; else continue; }
            if (path == "/provider/1") return request.method == "GET" &&
                request.target == "/provider/1?q=a%26b%20%E4%B8%AD%E6%96%87&count=2" && key == "fixture-search-key";
            const auto parsed = nlohmann::json::parse(request.body, nullptr, false);
            if (!parsed.is_object()) return false;
            if (path == "/provider/0") return request.method == "POST" && key == "Bearer fixture-search-key" &&
                parsed.value("query", "") == "a&b 中文" && parsed.value("max_results", 0) == 2;
            if (path == "/provider/2") return request.method == "POST" && key == "fixture-search-key" &&
                parsed.value("q", "") == "a&b 中文" && parsed.value("num", 0) == 2;
        }
        return false;
    };
    REQUIRE_NOTHROW(lubancore_consumer::WebSearchCase(name, directory.path,
        "http://127.0.0.1:" + std::to_string(server.port()), observed));
    const auto requests = server.requests();
    CHECK(requests.size() == (name == "admission" ? 0 : name == "providers" ? 3 : 12));
    for (const auto& request : requests) {
        CHECK_FALSE(request.target.starts_with("/unused")); CHECK_FALSE(request.target.starts_with("/unvisited"));
    }
    server.StopAndJoin();
    REQUIRE(server.owned_threads_quiescent());
}
}
TEST_CASE("SDK web search rejects invalid admission before any HTTP") { Run("admission"); }
TEST_CASE("SDK web search sends actual bounded authenticated provider requests") { Run("providers"); }
TEST_CASE("SDK web search bounds HTTP and owns cancellation and same-ID reopening") { Run("lifecycle"); }
