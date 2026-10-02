#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lubancore/core.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"

namespace {
namespace sdk = lubancore;
namespace fs = std::filesystem;
namespace platform = lubancode::platform;
using Json = nlohmann::json;
using namespace std::chrono_literals;

std::string Utf8(const fs::path& path) { return platform::PathToUtf8(path); }
fs::path Path(const std::string& path) { return platform::Utf8ToPath(path); }
const char* BinaryName() {
#ifdef _WIN32
    return "rg.exe";
#else
    return "rg";
#endif
}
void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    REQUIRE_FALSE(out.fail());
}
fs::path RealResources() {
    const auto value = platform::GetEnvVar("LUBANCORE_TEST_RESOURCE_ROOT");
    INFO("SDK search requires explicit remote-CI staged resources; no fallback or skip");
    REQUIRE(value.has_value());
    const auto root = Path(*value);
    REQUIRE(root.is_absolute());
    REQUIRE(fs::is_regular_file(root / "libexec" / BinaryName()));
    return root;
}
fs::path Probe() {
#ifdef LUBANCORE_TEST_SEARCH_PROBE
    const auto executable = Path(LUBANCORE_TEST_SEARCH_PROBE);
    REQUIRE(executable.is_absolute());
    REQUIRE(fs::is_regular_file(executable));
    return executable;
#else
    FAIL("LUBANCORE_TEST_SEARCH_PROBE must be registered for SDK search acceptance");
    return {};
#endif
}
void CopyExecutable(const fs::path& source, const fs::path& target) {
    fs::create_directories(target.parent_path());
    fs::copy_file(source, target, fs::copy_options::overwrite_existing);
    fs::permissions(target, fs::status(source).permissions(), fs::perm_options::replace);
}
struct Fixture {
    fs::path root;
    Fixture() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("sdk-search-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "project");
        fs::create_directories(root / "resources");
        fs::create_directories(root / "decoy");
        root = fs::canonical(root);
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
    fs::path Project() const { return root / "project"; }
    std::unique_ptr<sdk::Runtime> Runtime(const fs::path& resources) const {
        auto runtime = sdk::Runtime::Create({Utf8(root / "data"), Utf8(resources)});
        REQUIRE(runtime.has_value());
        return std::move(*runtime);
    }
    fs::path Controlled(const std::string& name, const std::string& mode) const {
        const auto resources = root / name;
        CopyExecutable(Probe(), resources / "libexec" / BinaryName());
        Write(resources / "libexec" / "probe.mode", mode + "\n");
        return resources;
    }
};
struct CwdGuard {
    fs::path old = fs::current_path();
    explicit CwdGuard(const fs::path& cwd) { fs::current_path(cwd); }
    ~CwdGuard() { std::error_code ignored; fs::current_path(old, ignored); }
};
class EnvGuard {
public:
    EnvGuard(const char* name, const std::string& value) : name_(name), old_(platform::GetEnvVarPresent(name)) {
        Set(value.c_str());
    }
    ~EnvGuard() {
#ifdef _WIN32
        Set(old_ ? old_->c_str() : "");
#else
        if (old_) Set(old_->c_str()); else unsetenv(name_);
#endif
    }
private:
    void Set(const char* value) {
#ifdef _WIN32
        _putenv_s(name_, value);
#else
        setenv(name_, value, 1);
#endif
    }
    const char* name_;
    std::optional<std::string> old_;
};
struct Rendezvous {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned arrived = 0;
    bool Meet() {
        std::unique_lock lock(mutex);
        ++arrived;
        cv.notify_all();
        return cv.wait_for(lock, 15s, [&] { return arrived == 4; });
    }
};
struct Trace {
    std::mutex mutex;
    std::vector<std::string> inputs;
    std::vector<sdk::ToolReply> replies;
    std::shared_ptr<Rendezvous> rendezvous;
    std::atomic<unsigned> models{0};
    std::size_t turn = 0;
    bool pending = false;
    bool schema_seen = false;
};
class SearchBackend final : public sdk::Backend {
public:
    explicit SearchBackend(std::shared_ptr<Trace> trace) : trace_(std::move(trace)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancellation) override {
        ++trace_->models;
        std::unique_lock lock(trace_->mutex);
        if (cancellation.requested()) return std::unexpected(sdk::Error{"fixture.cancelled", "cancelled"});
        if (trace_->inputs.empty()) return sdk::ModelReply{"unselected search complete", {}, std::nullopt};
        const auto definition = std::find_if(request.tools.begin(), request.tools.end(), [](const auto& tool) { return tool.name == "search"; });
        if (definition == request.tools.end()) return std::unexpected(sdk::Error{"fixture.search_absent", "public search definition absent"});
        trace_->schema_seen = definition->input_schema_json.find("max_results") != std::string::npos &&
            definition->input_schema_json.find("grep") != std::string::npos && definition->input_schema_json.find("glob") != std::string::npos;
        if (!trace_->pending) {
            if (trace_->turn >= trace_->inputs.size()) return std::unexpected(sdk::Error{"fixture.extra_model", "unexpected model request"});
            if (trace_->rendezvous && trace_->turn == 0 && !trace_->rendezvous->Meet())
                return std::unexpected(sdk::Error{"fixture.parallel_timeout", "four sessions did not overlap"});
            trace_->pending = true;
            return sdk::ModelReply{"", {{"search-" + std::to_string(trace_->turn), "search", trace_->inputs[trace_->turn]}}, std::nullopt};
        }
        const auto id = "search-" + std::to_string(trace_->turn);
        std::optional<sdk::ToolReply> found;
        for (const auto& message : request.messages) for (const auto& reply : message.tool_replies) {
            if (reply.call_id == id) {
                if (found) return std::unexpected(sdk::Error{"fixture.duplicate_reply", "duplicate search reply"});
                found = reply;
            }
        }
        if (!found) return std::unexpected(sdk::Error{"fixture.reply_absent", "actual tool reply absent"});
        trace_->replies.push_back(std::move(*found));
        ++trace_->turn;
        trace_->pending = false;
        return sdk::ModelReply{"search complete", {}, std::nullopt};
    }
private:
    std::shared_ptr<Trace> trace_;
};
sdk::SessionOptions Options(const fs::path& project, const std::shared_ptr<Trace>& trace) {
    sdk::SessionOptions options;
    options.cwd = Utf8(project);
    options.model = "sdk-search-fixture";
    options.system_prompt = "Use the explicit public search fixture.";
    options.backend = std::make_unique<SearchBackend>(trace);
    options.builtin_tools = {"search"};
    options.max_steps_per_turn = 4;
    return options;
}
std::shared_ptr<Trace> Inputs(std::vector<std::string> inputs) {
    auto trace = std::make_shared<Trace>();
    trace->inputs = std::move(inputs);
    return trace;
}
std::shared_ptr<sdk::Session> Open(sdk::Runtime& runtime, sdk::SessionOptions options) {
    auto session = runtime.OpenSession(std::move(options));
    REQUIRE(session.has_value());
    return *session;
}
struct TurnResult { sdk::Receipt receipt; sdk::Operation operation; sdk::ToolReply reply; };
TurnResult Turn(const std::shared_ptr<sdk::Session>& session, const std::shared_ptr<Trace>& trace, unsigned turn) {
    const auto receipt = session->Submit("request-" + std::to_string(turn), "perform this search");
    REQUIRE(receipt.has_value());
    const auto operation = session->WaitResult(receipt->operation_id, 30s);
    REQUIRE(operation.has_value());
    INFO(operation->error);
    REQUIRE(operation->state == sdk::OperationState::Succeeded);
    REQUIRE(operation->result_persisted);
    CHECK(operation->final_text == "search complete");
    CHECK(session->PendingApprovals().empty());
    std::lock_guard lock(trace->mutex);
    REQUIRE(trace->replies.size() == turn + 1);
    CHECK(trace->models.load() == (turn + 1) * 2);
    CHECK(trace->schema_seen);
    return {*receipt, *operation, trace->replies.back()};
}
std::pair<sdk::results::v1::ToolResultIdentity, std::string> Saved(
    const std::shared_ptr<sdk::Session>& session, const std::string& operation) {
    const auto summaries = session->ListToolResults(operation);
    REQUIRE(summaries.has_value());
    const auto selected = std::find_if(summaries->begin(), summaries->end(), [](const auto& item) { return item.selected && item.tool_name == "search"; });
    REQUIRE(selected != summaries->end());
    CHECK(std::count_if(summaries->begin(), summaries->end(), [](const auto& item) { return item.selected && item.tool_name == "search"; }) == 1);
    const auto snapshot = session->ReadToolResult(selected->identity);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->result().metadata_state == sdk::results::v1::ArtifactState::Verified);
    std::string body;
    for (const auto& channel : snapshot->result().channels) if (channel.text) {
        CHECK(channel.artifact_verified);
        CHECK(channel.state == sdk::results::v1::ArtifactState::Verified);
        CHECK(channel.capture_complete);
        body += *channel.text;
    }
    REQUIRE_FALSE(body.empty());
    return {selected->identity, std::move(body)};
}
void AddMcpMarker(sdk::SessionOptions& options, const fs::path& marker) {
    sdk::McpServer server;
    server.name = "search_preflight_marker";
    server.command = Utf8(Probe());
    server.arguments = {"--mcp-marker", Utf8(marker)};
    server.tools = {"unused"};
    server.startup_timeout_ms = 1000;
    server.call_timeout_ms = 1000;
    for (const char* name : {"SystemRoot", "TEMP", "TMP"})
        if (const auto value = platform::GetEnvVar(name)) server.environment.emplace_back(name, *value);
    options.mcp_servers.push_back(std::move(server));
}
void RejectBeforeStartup(sdk::Runtime& runtime, const Fixture& fixture, const std::string& code) {
    auto trace = Inputs({R"({"mode":"grep","pattern":"never"})"});
    auto options = Options(fixture.Project(), trace);
    const auto marker = fixture.root / "mcp-started";
    fs::remove(marker);
    AddMcpMarker(options, marker);
    const auto opened = runtime.OpenSession(std::move(options));
    REQUIRE_FALSE(opened.has_value());
    INFO(opened.error().message);
    CHECK(opened.error().code == code);
    CHECK(trace->models.load() == 0);
    CHECK_FALSE(fs::exists(marker));
}
unsigned long WaitPid(const fs::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream input(path, std::ios::binary);
        unsigned long pid = 0;
        if (input >> pid && pid != 0) return pid;
        std::this_thread::sleep_for(5ms);
    }
    FAIL("controlled rg did not publish its running PID");
    return 0;
}
void Exited(unsigned long pid) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (platform::IsProcessAlive(pid) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
    CHECK_FALSE(platform::IsProcessAlive(pid));
}
struct ControlledSession {
    fs::path control;
    std::shared_ptr<sdk::Session> session;
    ~ControlledSession() {
        std::ofstream release(control / "release", std::ios::binary);
        release << "cleanup\n";
        release.close();
        if (session) (void)session->Close();
    }
};
} // namespace

TEST_CASE("SDK builtin search: grep binds omitted null empty and relative paths without host chdir") {
    Fixture fixture;
    Write(fixture.Project() / "one.txt", "ANCHOR root\n");
    Write(fixture.Project() / "nested" / "two.txt", "ANCHOR nested\n");
    Write(fixture.root / "decoy" / "one.txt", "ANCHOR DECOY\n");
    CwdGuard cwd(fixture.root / "decoy");
    auto runtime = fixture.Runtime(RealResources());
    auto trace = Inputs({R"({"mode":"grep","pattern":"ANCHOR"})", R"({"mode":"grep","pattern":"ANCHOR","path":null})",
        R"({"mode":"grep","pattern":"ANCHOR","path":""})", R"({"mode":"grep","pattern":"ANCHOR","path":"nested"})",
        R"({"mode":"grep","pattern":"ANCHOR","path":"one.txt"})"});
    auto session = Open(*runtime, Options(fixture.Project(), trace));
    for (unsigned i = 0; i != 5; ++i) {
        const auto result = Turn(session, trace, i);
        CHECK_FALSE(result.reply.is_error);
        CHECK(result.reply.text.find("DECOY") == std::string::npos);
        CHECK(result.reply.text.find(i == 3 ? "two.txt:1:ANCHOR nested" : "one.txt:1:ANCHOR root") != std::string::npos);
        if (i == 3 || i == 4) CHECK(result.reply.text.find(i == 3 ? "ANCHOR root" : "ANCHOR nested") == std::string::npos);
        CHECK(fs::equivalent(fs::current_path(), fixture.root / "decoy"));
    }
}

TEST_CASE("SDK builtin search: Unicode glob and grep preserve project filename and text") {
    Fixture fixture;
    const auto filename = Path("目录/结果🙂.txt");
    Write(fixture.Project() / filename, "中文🙂 SEARCH_UNICODE\n");
    Write(fixture.Project() / "other.md", "other\n");
    auto runtime = fixture.Runtime(RealResources());
    auto trace = Inputs({R"({"mode":"grep","pattern":"中文🙂"})", R"({"mode":"glob","pattern":"**/*.txt"})",
        R"({"mode":"glob","pattern":"*.txt","path":"目录"})"});
    auto session = Open(*runtime, Options(fixture.Project(), trace));
    const auto grep = Turn(session, trace, 0);
    CHECK_FALSE(grep.reply.is_error);
    CHECK(grep.reply.text.find("目录/结果🙂.txt:1:中文🙂 SEARCH_UNICODE") != std::string::npos);
    const auto glob = Turn(session, trace, 1);
    CHECK_FALSE(glob.reply.is_error);
    CHECK(glob.reply.text.find("目录/结果🙂.txt\n") != std::string::npos);
    CHECK(glob.reply.text.find("other.md") == std::string::npos);
    const auto relative = Turn(session, trace, 2);
    CHECK_FALSE(relative.reply.is_error);
    CHECK(relative.reply.text == "结果🙂.txt\n");
}

TEST_CASE("SDK builtin search: no matches succeed while invalid regex remains an error reply") {
    Fixture fixture;
    Write(fixture.Project() / "file.txt", "present\n");
    auto runtime = fixture.Runtime(RealResources());
    auto trace = Inputs({R"({"mode":"grep","pattern":"ABSENT_ANCHOR"})", R"({"mode":"glob","pattern":"*.absent"})",
        R"({"mode":"grep","pattern":"("})"});
    auto session = Open(*runtime, Options(fixture.Project(), trace));
    const auto grep = Turn(session, trace, 0);
    CHECK_FALSE(grep.reply.is_error);
    CHECK(grep.reply.text.find("没搜到匹配的内容") != std::string::npos);
    const auto glob = Turn(session, trace, 1);
    CHECK_FALSE(glob.reply.is_error);
    CHECK(glob.reply.text.find("没找到匹配的文件") != std::string::npos);
    const auto invalid = Turn(session, trace, 2);
    CHECK(invalid.reply.is_error);
    CHECK(invalid.reply.text.find("search_pattern_invalid") != std::string::npos);
    CHECK(invalid.reply.text.find("没搜到") == std::string::npos);
}

TEST_CASE("SDK builtin search: bounded result is durable across Close and same-ID resume") {
    Fixture fixture;
    std::string body;
    for (int i = 0; i != 20; ++i) body += "MATCH_COUNT " + std::to_string(i) + "\n";
    Write(fixture.Project() / "many.txt", body);
    auto runtime = fixture.Runtime(RealResources());
    auto trace = Inputs({R"({"mode":"grep","pattern":"MATCH_COUNT","max_results":3})"});
    auto session = Open(*runtime, Options(fixture.Project(), trace));
    const auto result = Turn(session, trace, 0);
    CHECK_FALSE(result.reply.is_error);
    CHECK(result.reply.text.find("many.txt:1:MATCH_COUNT 0") != std::string::npos);
    CHECK(result.reply.text.find("many.txt:3:MATCH_COUNT 2") != std::string::npos);
    CHECK(result.reply.text.find("many.txt:4:") == std::string::npos);
    CHECK(result.reply.text.find("已截断") != std::string::npos);
    const auto saved = Saved(session, result.receipt.operation_id);
    CHECK(saved.second == result.reply.text);
    const auto id = session->id();
    REQUIRE(session->Close().has_value());
    const auto closed = session->ReadToolResult(saved.first);
    REQUIRE(closed.has_value());
    auto resumed_trace = Inputs({});
    auto options = Options(fixture.Project(), resumed_trace);
    options.resume_session_id = id;
    auto resumed = Open(*runtime, std::move(options));
    CHECK(Saved(resumed, result.receipt.operation_id).second == saved.second);
    CHECK(resumed_trace->models.load() == 0);
    CHECK(trace->models.load() == 2);
}

TEST_CASE("SDK builtin search: four overlapping sessions bind same and different projects independently") {
    Fixture fixture;
    Write(fixture.Project() / "file.txt", "ANCHOR SHARED\n");
    Write(fixture.root / "project-b" / "file.txt", "ANCHOR PROJECT_B\n");
    Write(fixture.root / "project-c" / "file.txt", "ANCHOR PROJECT_C\n");
    Write(fixture.root / "decoy" / "file.txt", "ANCHOR DECOY\n");
    CwdGuard cwd(fixture.root / "decoy");
    auto runtime = fixture.Runtime(RealResources());
    auto rendezvous = std::make_shared<Rendezvous>();
    const std::vector<fs::path> projects{fixture.Project(), fixture.Project(), fixture.root / "project-b", fixture.root / "project-c"};
    const std::vector<std::string> expected{"SHARED", "SHARED", "PROJECT_B", "PROJECT_C"};
    std::vector<std::shared_ptr<sdk::Session>> sessions;
    std::vector<std::shared_ptr<Trace>> traces;
    std::vector<sdk::Receipt> receipts;
    for (const auto& project : projects) {
        auto trace = Inputs({R"({"mode":"grep","pattern":"ANCHOR"})"});
        trace->rendezvous = rendezvous;
        sessions.push_back(Open(*runtime, Options(project, trace)));
        traces.push_back(trace);
    }
    for (const auto& session : sessions) {
        const auto submitted = session->Submit("request-0", "perform this search");
        REQUIRE(submitted.has_value());
        receipts.push_back(*submitted);
    }
    std::vector<sdk::results::v1::ToolResultIdentity> identities;
    for (unsigned i = 0; i != 4; ++i) {
        const auto result = Turn(sessions[i], traces[i], 0); // same key: durable duplicate, not new execution
        CHECK(result.receipt.duplicate);
        CHECK(result.receipt.operation_id == receipts[i].operation_id);
        CHECK_FALSE(result.reply.is_error);
        CHECK(result.reply.text == "file.txt:1:ANCHOR " + expected[i] + "\n");
        CHECK(result.reply.text.find("DECOY") == std::string::npos);
        identities.push_back(Saved(sessions[i], result.receipt.operation_id).first);
    }
    CHECK_FALSE(sessions[1]->ReadToolResult(identities[0]).has_value());
    REQUIRE(sessions[0]->Close().has_value());
    {
        std::lock_guard lock(traces[1]->mutex);
        traces[1]->inputs.push_back(R"({"mode":"grep","pattern":"ANCHOR"})");
    }
    CHECK(Turn(sessions[1], traces[1], 1).reply.text.find("ANCHOR SHARED") != std::string::npos);
    CHECK(fs::equivalent(fs::current_path(), fixture.root / "decoy"));
}

TEST_CASE("SDK builtin search: malformed paths fail explicitly and never search host cwd") {
    Fixture fixture;
    Write(fixture.Project() / "own.txt", "ANCHOR OWN\n");
    Write(fixture.root / "decoy" / "decoy.txt", "ANCHOR DECOY\n");
    CwdGuard cwd(fixture.root / "decoy");
    auto runtime = fixture.Runtime(RealResources());
    auto trace = Inputs({R"({"mode":"grep","pattern":"ANCHOR","path":7})",
        R"({"mode":"grep","pattern":"ANCHOR","path":"bad\u0000path"})",
        R"({"mode":"grep\u0000glob","pattern":"ANCHOR"})",
        R"({"mode":"grep","pattern":"ANCHOR\u0000"})",
        R"({"mode":"grep","pattern":"ANCHOR","glob":"*.txt\u0000"})"});
    auto session = Open(*runtime, Options(fixture.Project(), trace));
    for (unsigned i = 0; i != 5; ++i) {
        const auto result = Turn(session, trace, i);
        CHECK(result.reply.is_error);
        CHECK(result.reply.text.find(i < 2 ? "sdk.tool.invalid_path" : "sdk.tool.invalid_input") != std::string::npos);
        CHECK(result.reply.text.find("DECOY") == std::string::npos);
        CHECK(result.reply.text.find("没搜到") == std::string::npos);
    }
    auto invalid_utf8 = Inputs({std::string(R"({"mode":"grep","pattern":"ANCHOR","path":")") + char(0xff) + R"("})"});
    auto rejected = Open(*runtime, Options(fixture.Project(), invalid_utf8));
    const auto submitted = rejected->Submit("invalid-utf8", "reject malformed provider input");
    REQUIRE(submitted.has_value());
    const auto failed = rejected->WaitResult(submitted->operation_id, 15s);
    REQUIRE(failed.has_value());
    CHECK(failed->state == sdk::OperationState::Failed);
    CHECK_FALSE(failed->error.empty());
    CHECK(invalid_utf8->models.load() == 1);
    std::lock_guard lock(invalid_utf8->mutex);
    CHECK(invalid_utf8->replies.empty());
}

TEST_CASE("SDK builtin search: missing explicit root ignores PATH and home and unselected search stays optional") {
    Fixture fixture;
    const auto real = RealResources();
    const auto home = fixture.root / "home-decoy";
    CopyExecutable(real / "libexec" / BinaryName(), home / ".lubancode" / "rg-stage" / "libexec" / BinaryName());
    const auto old_path = platform::GetEnvVar("PATH").value_or("");
#ifdef _WIN32
    const auto separator = ";";
#else
    const auto separator = ":";
#endif
    EnvGuard path("PATH", Utf8(real / "libexec") + separator + old_path);
    EnvGuard home_env("HOME", Utf8(home));
    EnvGuard profile("USERPROFILE", Utf8(home));
    auto runtime = fixture.Runtime(fixture.root / "resources");
    RejectBeforeStartup(*runtime, fixture, "search_backend_missing");
    auto plain = Inputs({});
    auto options = Options(fixture.Project(), plain);
    options.builtin_tools = {"read_file", "write_file", "edit_file", "run_command"};
    auto session = Open(*runtime, std::move(options));
    const auto submitted = session->Submit("without-search", "ordinary session");
    REQUIRE(submitted.has_value());
    const auto finished = session->WaitResult(submitted->operation_id, 15s);
    REQUIRE(finished.has_value());
    CHECK(finished->state == sdk::OperationState::Succeeded);
    CHECK(plain->models.load() == 1);
    for (const auto& names : std::vector<std::vector<std::string>>{{"search", "search"}, {"search", "unknown-fixture"}}) {
        auto trace = Inputs({});
        auto invalid = Options(fixture.Project(), trace);
        invalid.builtin_tools = names;
        const auto rejected = runtime->OpenSession(std::move(invalid));
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().code == "sdk.tool.unsupported_or_duplicate");
        CHECK(trace->models.load() == 0);
    }
}

TEST_CASE("SDK builtin search: non-executable preparation precedes MCP and model startup") {
    Fixture fixture;
    fs::create_directories(fixture.root / "resources" / "libexec" / BinaryName());
    auto runtime = fixture.Runtime(fixture.root / "resources");
    RejectBeforeStartup(*runtime, fixture, "search_backend_not_executable");
#ifndef _WIN32
    const auto binary = fixture.root / "resources" / "libexec" / BinaryName();
    fs::remove(binary);
    CopyExecutable(RealResources() / "libexec" / BinaryName(), binary);
    fs::permissions(binary, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    RejectBeforeStartup(*runtime, fixture, "search_backend_not_executable");
#endif
}

TEST_CASE("SDK builtin search: preparation and failure caches never cross resource roots or sessions") {
    Fixture fixture;
    Write(fixture.Project() / "one.txt", "CACHE_ANCHOR real\n");
    const auto good = fixture.root / "good-resources";
    CopyExecutable(RealResources() / "libexec" / BinaryName(), good / "libexec" / BinaryName());
    auto good_runtime = fixture.Runtime(good);
    auto good_trace = Inputs({R"({"mode":"grep","pattern":"CACHE_ANCHOR"})"});
    auto good_session = Open(*good_runtime, Options(fixture.Project(), good_trace));
    for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
            {"wrong-version", "search_backend_version_mismatch"}, {"version-fail", "search_backend_spawn_failed"}}) {
        const auto resource = fixture.Controlled(pair.first, pair.first);
        auto runtime = fixture.Runtime(resource);
        RejectBeforeStartup(*runtime, fixture, pair.second);
        fs::remove(resource / "libexec" / BinaryName());
        CopyExecutable(RealResources() / "libexec" / BinaryName(), resource / "libexec" / BinaryName());
        auto trace = Inputs({R"({"mode":"grep","pattern":"CACHE_ANCHOR"})"});
        auto recovered = Open(*runtime, Options(fixture.Project(), trace));
        CHECK(Turn(recovered, trace, 0).reply.text.find("CACHE_ANCHOR real") != std::string::npos);
        fs::remove(resource / "libexec" / BinaryName());
        RejectBeforeStartup(*runtime, fixture, "search_backend_missing");
    }
    CHECK(Turn(good_session, good_trace, 0).reply.text.find("CACHE_ANCHOR real") != std::string::npos);
}

TEST_CASE("SDK builtin search: Cancel and Close join a running rg and its pipe readers") {
    Fixture fixture;
    for (bool close : {false, true}) {
        const auto resources = fixture.Controlled(close ? "close-control" : "cancel-control", "blocking");
        auto runtime = fixture.Runtime(resources);
        auto trace = Inputs({R"({"mode":"grep","pattern":"controlled"})"});
        ControlledSession owned{resources / "libexec", Open(*runtime, Options(fixture.Project(), trace))};
        const auto submitted = owned.session->Submit("blocking-search", "wait in actual rg process");
        REQUIRE(submitted.has_value());
        const auto pid = WaitPid(resources / "libexec" / "search.pid");
        REQUIRE(platform::IsProcessAlive(pid));
        if (close) REQUIRE(owned.session->Close().has_value());
        else REQUIRE(owned.session->Cancel(submitted->operation_id).has_value());
        const auto terminal = owned.session->WaitResult(submitted->operation_id, 10s);
        REQUIRE(terminal.has_value());
        CHECK(terminal->state == sdk::OperationState::Cancelled);
        CHECK(terminal->result_persisted);
        Exited(pid);
        CHECK_FALSE(fs::exists(resources / "libexec" / "natural-completion"));
        REQUIRE(owned.session->Close().has_value());
        fs::remove(resources / "libexec" / BinaryName()); // Windows also rejects leaked executable handles.
        CHECK_FALSE(fs::exists(resources / "libexec" / BinaryName()));
        CHECK(trace->models.load() == 1);
    }
}
