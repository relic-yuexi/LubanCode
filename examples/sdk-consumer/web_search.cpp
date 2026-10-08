#include <lubancore/core.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <iterator>
#include <limits>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace lubancore_consumer {
namespace {
namespace sdk = lubancore;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Observer = std::function<bool(const std::string&, const std::string&)>;
void Check(bool value, const std::string& reason) { if (!value) throw std::runtime_error("web-search: " + reason); }
template<class T> T Take(sdk::Result<T> value) {
    if (!value) throw std::runtime_error("web-search: " + value.error().code);
    return std::move(*value);
}
void Take(sdk::Result<void> value) { if (!value) throw std::runtime_error("web-search: " + value.error().code); }
std::string Utf8(const fs::path& path) {
    const auto text = path.generic_u8string(); return {reinterpret_cast<const char*>(text.data()), text.size()};
}
struct Reply { std::string text; bool error = false; };
class SearchBackend final : public sdk::Backend {
public:
    SearchBackend(std::shared_ptr<Reply> result, std::string input, bool selected = true)
        : result_(std::move(result)), input_(std::move(input)), selected_(selected) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        if (calls_++ == 0) {
            bool found = false;
            for (const auto& tool : request.tools) if (tool.name == "web_search") found = true;
            Check(found == selected_, "explicit declaration");
            if (!selected_) return sdk::ModelReply{"not-selected", {}, sdk::Usage{1, 1}};
            return sdk::ModelReply{"", {{"search-call", "web_search", input_}}, sdk::Usage{1, 1}};
        }
        for (auto message = request.messages.rbegin(); message != request.messages.rend(); ++message)
            for (const auto& reply : message->tool_replies) if (reply.call_id == "search-call") {
                result_->text = reply.text; result_->error = reply.is_error;
                return sdk::ModelReply{reply.text, {}, sdk::Usage{1, 1}};
            }
        throw std::runtime_error("web-search: paired result missing");
    }
private:
    std::shared_ptr<Reply> result_;
    std::string input_;
    bool selected_;
    unsigned calls_ = 0;
};
struct World {
    fs::path root;
    std::shared_ptr<sdk::Runtime> runtime;
    explicit World(const fs::path& base) : root(base) {
        fs::create_directories(root / "project"); fs::create_directories(root / "resources");
        runtime = Take(sdk::Runtime::Create({Utf8(root / "data"), Utf8(root / "resources")}));
    }
    sdk::SessionOptions Options(const std::string& endpoint, std::shared_ptr<Reply> reply,
                                const std::string& input = R"({"query":"a&b 中文","count":2})") {
        sdk::SessionOptions options;
        options.cwd = Utf8(root / "project"); options.model = "fixture";
        options.backend = std::make_unique<SearchBackend>(std::move(reply), input);
        options.builtin_tools = {"web_search"}; options.web_search = sdk::web_search::v1::Options{};
        options.web_search->api_key = "fixture-search-key";
        options.web_search->endpoint = endpoint;
        return options;
    }
    std::string Call(sdk::SessionOptions options, const std::shared_ptr<Reply>& reply) {
        auto session = Take(runtime->OpenSession(std::move(options)));
        static std::atomic<unsigned> serial{0};
        const auto receipt = Take(session->Submit("request-" + std::to_string(++serial), "search"));
        auto result = Take(session->WaitResult(receipt.operation_id, 8s));
        Check(result.state == sdk::OperationState::Succeeded, "actual model/tool loop terminal");
        Check(result.final_text == reply->text, "actual reply adopted");
        Take(session->Close());
        return session->id();
    }
};
}

void WebSearchCase(const std::string& name, const fs::path& base, const std::string& url, const Observer& observed) {
    World world(base);
    if (name == "admission") {
        for (const std::string mode : {"not-selected", "missing", "key", "provider", "endpoint", "userinfo", "header", "results", "query", "timeout"}) {
            auto options = world.Options(url + "/unused", std::make_shared<Reply>());
            if (mode == "not-selected") options.builtin_tools.clear();
            if (mode == "missing") options.web_search.reset();
            if (mode == "key") options.web_search->api_key = "bad\r\nkey";
            if (mode == "provider") options.web_search->provider = static_cast<sdk::web_search::v1::Provider>(999);
            if (mode == "endpoint") options.web_search->endpoint = "http://example.com/search";
            if (mode == "userinfo") options.web_search->endpoint = "https://user:password@example.com/search";
            if (mode == "header") options.web_search->max_header_bytes = std::numeric_limits<std::uint64_t>::max();
            if (mode == "results") options.web_search->max_results = 11;
            if (mode == "query") options.web_search->max_query_bytes = 0;
            if (mode == "timeout") options.web_search->total_timeout_ms = 1;
            const auto opened = world.runtime->OpenSession(std::move(options));
            Check(!opened, "bad admission: " + mode);
            Check(opened.error().code == (mode == "not-selected" ? "sdk.web_search.not_selected" :
                mode == "missing" ? "sdk.web_search.missing_options" : "sdk.web_search.invalid_options"), "admission code");
        }
        auto reply = std::make_shared<Reply>();
        auto options = world.Options(url + "/unused", reply);
        options.web_search.reset(); options.builtin_tools.clear();
        options.backend = std::make_unique<SearchBackend>(reply, "", false);
        auto session = Take(world.runtime->OpenSession(std::move(options)));
        const auto accepted = Take(session->Submit("off", "search"));
        Check(Take(session->WaitResult(accepted.operation_id, 8s)).final_text == "not-selected", "default off");
        Take(session->Close());
    } else if (name == "providers") {
        const std::vector<sdk::web_search::v1::Provider> providers{sdk::web_search::v1::Provider::Tavily,
            sdk::web_search::v1::Provider::Brave, sdk::web_search::v1::Provider::Serper};
        for (std::size_t i = 0; i < providers.size(); ++i) {
            auto reply = std::make_shared<Reply>();
            auto options = world.Options(url + "/provider/" + std::to_string(i), reply);
            options.web_search->provider = providers[i]; options.web_search->max_results = 2;
            world.Call(std::move(options), reply);
            Check(!reply->error && reply->text.find("1. 搜索结果") != std::string::npos &&
                reply->text.find("3.") == std::string::npos, "bounded actual provider results");
            Check(reply->text.find("fixture-search-key") == std::string::npos, "credential not projected");
            Check(observed("/provider/" + std::to_string(i), "wire"), "actual provider wire");
        }
    } else if (name == "lifecycle") {
        const std::vector<std::pair<std::string, std::string>> failures{
            {"body", "web_search.response_limit"}, {"invalid", "web_search.invalid_response"},
            {"status", "web_search.http_status"}, {"redirect", "web_search.redirect_rejected"},
            {"header", "web_search.header_limit"}, {"timeout", "web_search.timeout"}};
        for (const auto& [route, code] : failures) {
            auto reply = std::make_shared<Reply>();
            auto options = world.Options(url + "/" + route, reply);
            options.web_search->max_response_bytes = 1024;
            if (route == "header") options.web_search->max_header_bytes = 256;
            if (route == "timeout") { options.web_search->connect_timeout_ms = 50; options.web_search->total_timeout_ms = 100; }
            world.Call(std::move(options), reply);
            Check(reply->error && reply->text == code, "actual bounded failure: " + route);
        }
        auto output = std::make_shared<Reply>();
        auto options = world.Options(url + "/output", output);
        options.web_search->max_output_bytes = 256;
        world.Call(std::move(options), output);
        Check(!output->error && output->text.size() <= 256 && output->text.ends_with("[web_search.truncated]"), "output bound");
        for (const std::string input : {R"({"query":"","count":2})", R"({"query":"test","count":"bad"})"}) {
            auto reply = std::make_shared<Reply>();
            world.Call(world.Options(url + "/unused", reply, input), reply);
            Check(reply->error, "bad query/count zero dispatch");
        }
        auto closing = std::make_shared<Reply>();
        auto session = Take(world.runtime->OpenSession(world.Options(url + "/close", closing)));
        Take(session->Submit("closing", "search"));
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!observed("/close", "entered") && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
        Check(observed("/close", "entered"), "actual pending HTTP");
        const auto started = std::chrono::steady_clock::now();
        Take(session->Close());
        Check(std::chrono::steady_clock::now() - started < 5s, "cooperative Close waits actual request");

        auto seed = std::make_shared<Reply>();
        const auto id = world.Call(world.Options(url + "/resume", seed), seed);
        auto resumed = std::make_shared<Reply>();
        auto resume_options = world.Options(url + "/resume", resumed);
        resume_options.resume_session_id = id; resume_options.web_search->api_key = "resumed-search-key";
        world.Call(std::move(resume_options), resumed);
        Check(!resumed->error && observed("/resume", "resumed"), "same-ID fresh credential");
        auto first = std::make_shared<Reply>(), second = std::make_shared<Reply>();
        auto first_options = world.Options(url + "/isolation", first);
        auto second_options = world.Options(url + "/isolation", second);
        first_options.web_search->max_results = 1;
        second_options.web_search->max_results = 2;
        second_options.web_search->api_key = "isolated-search-key";
        auto first_session = Take(world.runtime->OpenSession(std::move(first_options)));
        auto second_session = Take(world.runtime->OpenSession(std::move(second_options)));
        const auto first_receipt = Take(first_session->Submit("first", "search"));
        const auto second_receipt = Take(second_session->Submit("second", "search"));
        Check(Take(first_session->WaitResult(first_receipt.operation_id, 8s)).state == sdk::OperationState::Succeeded,
              "same-cwd first terminal");
        Check(Take(second_session->WaitResult(second_receipt.operation_id, 8s)).state == sdk::OperationState::Succeeded,
              "same-cwd second terminal");
        Check(!first->error && !second->error && first->text.find("2.") == std::string::npos &&
              second->text.find("2. second") != std::string::npos, "independent live Session result limits");
        Take(first_session->Close()); Take(second_session->Close());
        for (const auto& entry : fs::recursive_directory_iterator(world.root / "data")) if (entry.is_regular_file()) {
            std::ifstream in(entry.path(), std::ios::binary);
            const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            Check(bytes.find("fixture-search-key") == std::string::npos && bytes.find("resumed-search-key") == std::string::npos &&
                  bytes.find("isolated-search-key") == std::string::npos,
                  "credentials not persisted");
        }
    } else throw std::runtime_error("web-search: unknown case");
    Take(world.runtime->Shutdown());
}
void WebSearch(const fs::path& base, const std::string& url, const fs::path& requests) {
    const Observer observed = [&](const std::string& path, const std::string& mode) {
        std::ifstream input(requests, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        return bytes.find(path + "\t" + mode + "\n") != std::string::npos;
    };
    for (const std::string name : {"admission", "providers", "lifecycle"}) {
        WebSearchCase(name, base / name, url, observed);
        std::cout << "[sdk-web-search] " << name << " passed\n";
    }
}
} // namespace lubancore_consumer
