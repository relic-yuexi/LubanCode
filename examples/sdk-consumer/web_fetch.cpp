// Public installed SDK + STL only. The host supplies an explicit loopback fixture.
#include <lubancore/core.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace lubancore_consumer {
namespace {
namespace sdk = lubancore;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using ObservedRequest = std::function<bool(const std::string&, const std::string&)>;
void Check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error("web-fetch: " + message); }
template<class T> T Take(sdk::Result<T> value, const char* action) {
    if (!value) throw std::runtime_error(std::string("web-fetch: ") + action + ": " + value.error().code + " " + value.error().message);
    return std::move(*value);
}
void Take(sdk::Result<void> value, const char* action) {
    if (!value) throw std::runtime_error(std::string("web-fetch: ") + action + ": " + value.error().code + " " + value.error().message);
}
std::string Utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
struct Directory {
    fs::path root;
    explicit Directory(const fs::path& base) {
        Check(base.is_absolute(), "state root must be absolute");
        static std::atomic<unsigned> next{0};
        root = base / ("web-fetch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++next));
        fs::create_directories(root / "project");
        fs::create_directories(root / "other-project");
        fs::create_directories(root / "third-project");
        fs::create_directories(root / "resources");
    }
    ~Directory() { std::error_code ignored; fs::remove_all(root, ignored); }
};
struct Barrier {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned arrived = 0;
    void Meet() {
        std::unique_lock lock(mutex); ++arrived; cv.notify_all();
        Check(cv.wait_for(lock, 10s, [&] { return arrived == 4; }), "four actual session models did not overlap");
    }
};
struct State {
    std::atomic<unsigned> models{0}, destroyed{0};
    std::mutex mutex;
    std::optional<sdk::ToolReply> reply;
    std::shared_ptr<Barrier> barrier;
    std::string model, input;
    bool admitted = true;
    unsigned max_model_calls = 2;
};
class Backend final : public sdk::Backend {
public:
    explicit Backend(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~Backend() override { ++state_->destroyed; }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        Check(request.model == state_->model, "model crossed a Session");
        const auto count = std::count_if(request.tools.begin(), request.tools.end(), [](const auto& tool) { return tool.name == "web_fetch"; });
        Check(count == (state_->admitted ? 1 : 0), "explicit web_fetch admission was not reflected in the actual model request");
        const auto call = ++state_->models;
        if (!state_->admitted) {
            Check(call == 1, "default-off session made another model request");
            return sdk::ModelReply{"fetch-off", {}, std::nullopt};
        }
        Check(call <= state_->max_model_calls, "tool invocation reran or produced extra model rounds");
        const auto call_id = "fetch-" + state_->model + "-" + std::to_string((call - 1) / 2);
        if (call % 2 == 1) {
            if (state_->barrier) state_->barrier->Meet();
            return sdk::ModelReply{"", {{call_id, "web_fetch", state_->input}}, std::nullopt};
        }
        unsigned found = 0;
        std::lock_guard lock(state_->mutex);
        for (const auto& message : request.messages) for (const auto& reply : message.tool_replies)
            if (reply.call_id == call_id) { state_->reply = reply; ++found; }
        Check(found == 1, "actual web_fetch reply did not reach model history exactly once");
        return sdk::ModelReply{"fetch-finished:" + state_->model, {}, std::nullopt};
    }
private:
    std::shared_ptr<State> state_;
};
std::unique_ptr<sdk::Runtime> Runtime(const Directory& directory, const char* data = "data") {
    return Take(sdk::Runtime::Create({Utf8(directory.root / data), Utf8(directory.root / "resources")}), "Runtime::Create");
}
std::shared_ptr<State> NewState(const std::string& model, const std::string& url, std::uint64_t max_bytes = 102400) {
    Check(url.find_first_of("\"\\\r\n") == std::string::npos, "fixture URL must be directly representable");
    auto state = std::make_shared<State>(); state->model = model;
    state->input = "{\"url\":\"" + url + "\",\"max_bytes\":" + std::to_string(max_bytes) + "}";
    return state;
}
sdk::SessionOptions Options(const Directory& directory, const std::shared_ptr<State>& state,
                             sdk::web_fetch::v1::Options limits = {}, bool other_project = false) {
    sdk::SessionOptions options;
    options.cwd = Utf8(directory.root / (other_project ? "other-project" : "project"));
    options.model = state->model; options.backend = std::make_unique<Backend>(state);
    options.max_steps_per_turn = 4;
    if (state->admitted) { options.builtin_tools = {"web_fetch"}; options.web_fetch = std::move(limits); }
    return options;
}
void Close(const std::shared_ptr<sdk::Session>& session, const std::shared_ptr<State>& state) {
    Take(session->Close(), "Close"); Take(session->Close(), "Close again");
    Check(state->destroyed == 1, "actual backend owner outlived Close or was destroyed twice");
    Check(!session->Submit("closed", "must reject"), "closed session accepted a new fetch");
}
sdk::Receipt Submit(const std::shared_ptr<sdk::Session>& session, const std::string& key = "fetch") {
    return Take(session->Submit(key, "fetch the explicit document"), "Submit");
}
sdk::ToolReply Finish(const std::shared_ptr<sdk::Session>& session, const std::shared_ptr<State>& state,
                      const sdk::Receipt& receipt, unsigned expected_models = 2) {
    const auto operation = Take(session->WaitResult(receipt.operation_id, 30s), "WaitResult");
    Check(operation.state == sdk::OperationState::Succeeded && operation.result_persisted &&
        operation.final_text == "fetch-finished:" + state->model, "fetch operation did not complete durably: " + operation.error);
    Check(state->models == expected_models && session->PendingApprovals().empty(), "model round count or built-in approval rule changed");
    std::lock_guard lock(state->mutex);
    Check(state->reply.has_value(), "backend did not retain the actual reply");
    return *state->reply;
}
void Seen(const ObservedRequest& observed, const std::string& target, const std::string& agent = "lubancore") {
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (!observed(target, agent) && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(10ms);
    Check(observed(target, agent), "real fixture did not receive " + target + " with this Session's User-Agent");
}
sdk::ToolReply Fetch(sdk::Runtime& runtime, const Directory& directory, const std::string& base_url,
    const std::string& target, const std::string& name, const ObservedRequest& observed,
    sdk::web_fetch::v1::Options limits = {}, std::uint64_t requested = 102400, bool unknown_path = false) {
    auto state = NewState(name, base_url + target, requested);
    if (unknown_path) {
        state->input.pop_back();
        state->input += ",\"path\":\"not-a-filesystem-input\\u0000\"}";
    }
    const auto agent = limits.user_agent;
    auto session = Take(runtime.OpenSession(Options(directory, state, std::move(limits))), "OpenSession");
    const auto reply = Finish(session, state, Submit(session));
    Seen(observed, target, agent); Close(session, state); return reply;
}
void Error(const sdk::ToolReply& reply, const std::string& code) {
    Check(reply.is_error && reply.text.starts_with("web_fetch." + code + ":"), "wrong actual fetch error: " + reply.text);
}
void Admission(const Directory& directory, const std::string& url, const ObservedRequest& observed) {
    auto runtime = Runtime(directory);
    auto off = NewState("off", url + "/admission/never"); off->admitted = false;
    auto plain = Take(runtime->OpenSession(Options(directory, off)), "default OpenSession");
    const auto receipt = Submit(plain); const auto result = Take(plain->WaitResult(receipt.operation_id, 30s), "off result");
    Check(result.state == sdk::OperationState::Succeeded && result.final_text == "fetch-off" && off->models == 1, "default enabled web_fetch");
    Close(plain, off);
    for (unsigned bad = 0; bad != 10; ++bad) {
        auto state = NewState("invalid", url + "/admission/never"); auto options = Options(directory, state);
        if (bad == 0) options.builtin_tools.clear();
        if (bad == 1) options.web_fetch->connect_timeout_ms = 0;
        if (bad == 2) options.web_fetch->max_download_bytes = 0;
        if (bad == 3) options.web_fetch->max_header_bytes = 512 * 1024 + 1;
        if (bad == 4) options.web_fetch->max_output_bytes = 255;
        if (bad == 5) options.web_fetch->total_timeout_ms = 120001;
        if (bad == 6) options.web_fetch->max_redirects = 11;
        if (bad == 7) options.web_fetch->user_agent = "bad\r\nheader";
        if (bad == 8) options.builtin_tools.push_back("web_fetch");
        if (bad == 9) {
            sdk::Tool tool; tool.name = "web_fetch"; tool.description = "collision";
            tool.execute = [](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
                throw std::runtime_error("duplicate custom tool executed");
            }; options.custom_tools.push_back(std::move(tool));
        }
        auto refused = runtime->OpenSession(std::move(options));
        Check(!refused && state->models == 0 && state->destroyed == 1, "invalid host declaration ran work or retained its backend");
    }
    Check(!observed("/admission/never", "lubancore"), "off or refused session sent a request");
    auto state = NewState("explicit-default", url + "/admission/plain");
    auto options = Options(directory, state); options.web_fetch.reset();
    auto session = Take(runtime->OpenSession(std::move(options)), "explicit default limits");
    const auto reply = Finish(session, state, Submit(session));
    Check(!reply.is_error && reply.text.ends_with("web-fetch-owned-body"), "explicit default fetch returned another body");
    Seen(observed, "/admission/plain"); Close(session, state); Take(runtime->Shutdown(), "admission Shutdown");
}
void Content(const Directory& directory, const std::string& url, const ObservedRequest& observed) {
    auto runtime = Runtime(directory);
    const auto html = Fetch(*runtime, directory, url, "/content/html", "html", observed, {}, 102400, true);
    Check(!html.is_error && html.text.find("hello & world") != std::string::npos && html.text.find("中文") != std::string::npos &&
        html.text.find("SECRET_SCRIPT") == std::string::npos && html.text.find("badstyle") == std::string::npos, "HTML cleaning differs");
    const auto utf8 = Fetch(*runtime, directory, url, "/content/utf8", "utf8", observed, {}, 4);
    Check(!utf8.is_error && utf8.text.ends_with("中") && utf8.text.find("已截断") != std::string::npos, "UTF-8 body cap split a character");
    Error(Fetch(*runtime, directory, url, "/content/binary", "binary", observed), "binary_body");
    Error(Fetch(*runtime, directory, url, "/content/status", "status", observed), "http_status");
    Take(runtime->Shutdown(), "content Shutdown");
}
void Limits(const Directory& directory, const std::string& url, const ObservedRequest& observed) {
    auto runtime = Runtime(directory); sdk::web_fetch::v1::Options limits;
    limits.max_download_bytes = 128;
    const auto exact = Fetch(*runtime, directory, url, "/limits/exact", "exact", observed, limits);
    Check(!exact.is_error && exact.text.ends_with(std::string(128, 'E')), "exact download cap was refused");
    Error(Fetch(*runtime, directory, url, "/limits/body", "body", observed, limits), "download_limit");
    const auto capabilities = Take(sdk::web_fetch::v1::DescribeCapabilities(), "DescribeCapabilities");
    const std::string gzip_outcome = capabilities.gzip_decoding ? "download_limit" : "unsupported_encoding";
    Error(Fetch(*runtime, directory, url, "/limits/gzip", "gzip", observed, limits), gzip_outcome);
    std::cout << "[sdk-web-fetch-decoder] {\"gzip_decoding\":" << (capabilities.gzip_decoding ? "true" : "false")
              << ",\"outcome\":\"" << gzip_outcome << "\"}\n";
    limits = {}; limits.max_header_bytes = 128;
    Error(Fetch(*runtime, directory, url, "/limits/header", "header", observed, limits), "header_limit");
    limits = {}; limits.max_output_bytes = 256;
    const auto output = Fetch(*runtime, directory, url, "/limits/output", "output", observed, limits, 1000000);
    Check(!output.is_error && output.text.size() <= 256 && output.text.find("已截断") != std::string::npos, "model widened whole-output host limit");
    limits = {}; limits.max_download_bytes = 120;
    Error(Fetch(*runtime, directory, url, "/limits/cumulative-body", "total-body", observed, limits), "download_limit");
    Seen(observed, "/limits/body-final");
    limits = {}; limits.max_header_bytes = 200;
    Error(Fetch(*runtime, directory, url, "/limits/cumulative-header", "total-header", observed, limits), "header_limit");
    Seen(observed, "/limits/header-final"); Take(runtime->Shutdown(), "limits Shutdown");
}
void Redirects(const Directory& directory, const std::string& url, const ObservedRequest& observed) {
    auto runtime = Runtime(directory);
    const auto relative = Fetch(*runtime, directory, url, "/redirects/relative", "relative", observed);
    Check(!relative.is_error && relative.text.ends_with("relative-final") && relative.text.find(url + "/redirects/final") != std::string::npos,
          "relative redirect did not retain the final URL and actual body");
    Seen(observed, "/redirects/final");
    Error(Fetch(*runtime, directory, url, "/redirects/loop-a", "loop", observed), "redirect_loop");
    Seen(observed, "/redirects/loop-b");
    sdk::web_fetch::v1::Options limits; limits.max_redirects = 0;
    Error(Fetch(*runtime, directory, url, "/redirects/limit", "limit", observed, limits), "redirect_limit");
    Check(!observed("/redirects/unvisited", "lubancore"), "redirect limit still followed the target");
    Error(Fetch(*runtime, directory, url, "/redirects/scheme", "scheme", observed), "redirect_invalid");
    Error(Fetch(*runtime, directory, url, "/redirects/duplicate", "duplicate", observed), "redirect_invalid");
    Take(runtime->Shutdown(), "redirect Shutdown");
}
void Cancel(const Directory& directory, const std::string& url, const ObservedRequest& observed) {
    auto runtime = Runtime(directory);
    for (const auto& kind : {std::string("wait"), std::string("close"), std::string("shutdown")}) {
        auto state = NewState(kind, url + "/cancel/" + kind);
        auto session = Take(runtime->OpenSession(Options(directory, state)), "cancel OpenSession");
        const auto receipt = Submit(session); Seen(observed, "/cancel/" + kind);
        const auto start = std::chrono::steady_clock::now();
        if (kind == "wait") Take(session->Cancel(receipt.operation_id), "Cancel");
        if (kind == "close") Take(session->Close(), "Close during actual HTTP");
        if (kind == "shutdown") Take(runtime->Shutdown(), "Shutdown during actual HTTP");
        const auto result = Take(session->WaitResult(receipt.operation_id, 5s), "cancel WaitResult");
        Check(std::chrono::steady_clock::now() - start < 4s, "cancellation waited for the fixture's delayed response");
        Check(result.state == sdk::OperationState::Cancelled && result.result_persisted && state->models == 1,
            "cancelled HTTP continued model work or lost its final receipt");
        Close(session, state);
        Check(Take(session->ReadOperation(receipt.operation_id), "closed result").state == sdk::OperationState::Cancelled,
            "closed operation was not owned");
    }
    auto next = Runtime(directory, "timeout-data"); sdk::web_fetch::v1::Options limits;
    limits.connect_timeout_ms = 1000; limits.total_timeout_ms = 2000;
    auto timed = NewState("timeout", url + "/cancel/timeout");
    auto session = Take(next->OpenSession(Options(directory, timed, limits)), "timeout OpenSession");
    const auto started = std::chrono::steady_clock::now();
    const auto receipt = Submit(session);
    Seen(observed, "/cancel/timeout");
    Error(Finish(session, timed, receipt), "timeout");
    Check(std::chrono::steady_clock::now() - started < 6s, "hard timeout waited for the 15-second server response");
    Close(session, timed);
    Take(next->Shutdown(), "timeout Shutdown");
}
void Isolation(const Directory& directory, const std::string& url, const ObservedRequest& observed) {
    auto runtime = Runtime(directory); auto other = Runtime(directory, "other-data");
    auto barrier = std::make_shared<Barrier>();
    std::vector<std::shared_ptr<State>> states;
    std::vector<std::shared_ptr<sdk::Session>> sessions;
    std::vector<sdk::Receipt> receipts;
    for (unsigned i = 0; i != 4; ++i) {
        const auto name = "isolation-" + std::to_string(i);
        auto state = NewState(name, url + "/isolation/" + std::to_string(i)); state->barrier = barrier;
        if (i == 1) state->max_model_calls = 4;
        sdk::web_fetch::v1::Options limits; limits.user_agent = name; limits.max_download_bytes = i == 0 ? 128 : 1024;
        auto& owner = i == 3 ? other : runtime;
        auto options = Options(directory, state, limits, i == 2);
        if (i == 3) options.cwd = Utf8(directory.root / "third-project");
        sessions.push_back(Take(owner->OpenSession(std::move(options)), "isolation OpenSession"));
        states.push_back(std::move(state));
    }
    for (const auto& session : sessions) receipts.push_back(Submit(session));
    std::vector<sdk::ToolReply> held;
    for (unsigned i = 0; i != 4; ++i) {
        auto reply = Finish(sessions[i], states[i], receipts[i]);
        Seen(observed, "/isolation/" + std::to_string(i), "isolation-" + std::to_string(i));
        if (i == 0) Error(reply, "download_limit");
        else Check(!reply.is_error && reply.text.ends_with(std::string(256, 'I')), "host limit or response crossed Sessions");
        held.push_back(std::move(reply));
    }
    Close(sessions[0], states[0]);
    Check(states[1]->destroyed == 0 && states[2]->destroyed == 0 && states[3]->destroyed == 0, "closing one fetch retired a peer");
    states[1]->barrier.reset();
    const auto continued = Finish(sessions[1], states[1], Submit(sessions[1], "after-peer-close"), 4);
    Check(!continued.is_error && continued.text.ends_with(std::string(256, 'I')), "peer could not execute after another Session closed");
    for (unsigned i = 1; i != 4; ++i) Close(sessions[i], states[i]);
    Take(runtime->Shutdown(), "isolation Shutdown"); Take(other->Shutdown(), "other Shutdown");
    const auto retired = directory.root / "retired-data"; fs::rename(directory.root / "data", retired); fs::remove_all(retired);
    Check(held[1].text.ends_with(std::string(256, 'I')), "returned reply borrowed closed resources");
}
} // namespace

void WebFetchCase(const std::string& name, const fs::path& base, const std::string& base_url,
                  const std::function<bool(const std::string&, const std::string&)>& observed) {
    Check(base_url.starts_with("http://127.0.0.1:"), "acceptance requires explicit loopback fixture");
    Check(static_cast<bool>(observed), "acceptance requires actual request observations");
    Directory directory(base);
    if (name == "admission") Admission(directory, base_url, observed);
    else if (name == "content") Content(directory, base_url, observed);
    else if (name == "limits") Limits(directory, base_url, observed);
    else if (name == "redirects") Redirects(directory, base_url, observed);
    else if (name == "cancel") Cancel(directory, base_url, observed);
    else if (name == "isolation") Isolation(directory, base_url, observed);
    else throw std::runtime_error("unknown web-fetch fixture case");
    std::cout << "[sdk-web-fetch-path] " << name << '\n';
}

void WebFetch(const fs::path& base, const std::string& base_url, const fs::path& requests_file) {
    Check(requests_file.is_absolute(), "request evidence path must be explicit and absolute");
    const auto observed = [&](const std::string& target, const std::string& agent) {
        std::ifstream input(requests_file, std::ios::binary);
        for (std::string line; std::getline(input, line);)
            if (line == target + "\t" + agent) return true;
        return false;
    };
    for (const auto* name : {"admission", "content", "limits", "redirects", "cancel", "isolation"})
        WebFetchCase(name, base / name, base_url, observed);
    std::cout << "[sdk-web-fetch-consumer] complete\n";
}
} // namespace lubancore_consumer
