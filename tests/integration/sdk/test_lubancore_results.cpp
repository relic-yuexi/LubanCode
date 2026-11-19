#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "lubancore/core.hpp"
#include "tools/path_utils.hpp"
#include "workspace/identity.hpp"
#include "workspace/index.hpp"

namespace {
namespace sdk = lubancore;
namespace result = sdk::results::v1;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct Fixture {
    fs::path root;
    Fixture() {
        static std::atomic<int> serial{0};
        root = fs::temp_directory_path() / ("lubancore-results-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "cwd");
        fs::create_directories(root / "resources");
        root = fs::canonical(root);
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    std::string Utf8(const fs::path& path) const { return lubancode::tools::PathToUtf8(path); }
    sdk::RuntimeOptions Roots() const { return {Utf8(root / "data"), Utf8(root / "resources")}; }
    fs::path SessionDir(const std::string& id) const {
        const auto identity = lubancode::workspace::ResolveWorkspaceIdentity(root / "cwd", root / "data");
        REQUIRE(identity.has_value());
        const auto directory = lubancode::workspace::index::ResolveDirByWorkspaceKey(
            root / "data" / "workspaces", identity->workspace_key);
        REQUIRE(directory.has_value());
        return *directory / "sessions" / id;
    }
};
struct Gate {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    ~Gate() { try { release.set_value(); } catch (...) {} }
};
struct LedgerCorruption {
    fs::path path;
    std::uintmax_t bytes_before = 0;
    std::uintmax_t bytes_after = 0;
    std::atomic<int> calls{0};
};
class ToolBackend final : public sdk::Backend {
public:
    ToolBackend(std::shared_ptr<std::atomic<int>> calls, std::shared_ptr<Gate> gate = {},
        std::shared_ptr<LedgerCorruption> corrupt = {}) : calls_(std::move(calls)), gate_(std::move(gate)), corrupt_(std::move(corrupt)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        ++*calls_;
        if (!request.messages.empty() && request.messages.back().role == "user" &&
            request.messages.back().tool_replies.empty()) {
            if (gate_ && ++turns_ == 2) {
                gate_->entered.set_value();
                if (gate_->released.wait_for(15s) != std::future_status::ready)
                    return std::unexpected(sdk::Error{"fixture.gate_timeout", "release did not arrive"});
            }
            return sdk::ModelReply{"", {{"provider-call", "result_fixture", "{}"}}, std::nullopt};
        }
        if (corrupt_ && !corrupt_->path.empty()) {
            // A new JournalWriter uses wbx and keeps its current offset: an
            // external append would be overwritten by the next ordinary write.
            // Damage an already committed byte without moving that offset or
            // changing file length. Later appends cannot heal the prefix.
            std::error_code ec;
            corrupt_->bytes_before = fs::file_size(corrupt_->path, ec);
            if (ec || corrupt_->bytes_before == 0)
                return std::unexpected(sdk::Error{"fixture.inject_failed", "journal has no committed prefix"});
            std::fstream injected(corrupt_->path, std::ios::binary | std::ios::in | std::ios::out);
            if (!injected) return std::unexpected(sdk::Error{"fixture.inject_failed", "cannot corrupt test ledger"});
            char original = 0;
            injected.get(original);
            if (!injected || original != '{')
                return std::unexpected(sdk::Error{"fixture.inject_failed", "committed journal prefix is unexpected"});
            injected.seekp(0);
            injected.put('!');
            injected.flush();
            if (!injected.good())
                return std::unexpected(sdk::Error{"fixture.inject_failed", "cannot flush corrupted prefix"});
            corrupt_->bytes_after = fs::file_size(corrupt_->path, ec);
            if (ec || corrupt_->bytes_after != corrupt_->bytes_before)
                return std::unexpected(sdk::Error{"fixture.inject_failed", "prefix corruption changed journal length"});
            ++corrupt_->calls;
        }
        return sdk::ModelReply{"tool complete", {}, std::nullopt};
    }
private:
    std::shared_ptr<std::atomic<int>> calls_;
    std::shared_ptr<Gate> gate_;
    std::shared_ptr<LedgerCorruption> corrupt_;
    int turns_ = 0;
};
sdk::SessionOptions Options(const Fixture& fixture, std::shared_ptr<std::atomic<int>> models,
    std::shared_ptr<std::atomic<int>> tools, std::string text,
    std::shared_ptr<Gate> gate = {}, std::shared_ptr<LedgerCorruption> corrupt = {}) {
    sdk::SessionOptions options;
    options.cwd = fixture.Utf8(fixture.root / "cwd");
    options.model = "result-model";
    options.system_prompt = "Retain tool result materials.";
    options.backend = std::make_unique<ToolBackend>(std::move(models), std::move(gate), std::move(corrupt));
    options.max_steps_per_turn = 4;
    sdk::Tool tool;
    tool.name = "result_fixture";
    tool.description = "Return persisted text.";
    tool.requires_approval = false;
    tool.execute = [tools, text = std::move(text)](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
        ++*tools;
        return sdk::ToolResult{text, false};
    };
    options.custom_tools.push_back(std::move(tool));
    return options;
}
sdk::Receipt Turn(const std::shared_ptr<sdk::Session>& session, const std::string& key = "first") {
    const auto receipt = session->Submit(key, key);
    REQUIRE(receipt.has_value());
    const auto operation = session->WaitResult(receipt->operation_id, 30s);
    REQUIRE(operation.has_value());
    INFO(operation->error);
    REQUIRE(operation->state == sdk::OperationState::Succeeded);
    REQUIRE(operation->result_persisted);
    return *receipt;
}
result::ToolResultIdentity Selected(const std::shared_ptr<sdk::Session>& session, const std::string& operation_id) {
    const auto list = session->ListToolResults(operation_id);
    REQUIRE(list.has_value());
    const auto selected = std::find_if(list->begin(), list->end(), [](const auto& entry) {
        return entry.selected && entry.identity.result_id.starts_with("res-");
    });
    REQUIRE(selected != list->end());
    return selected->identity;
}
const result::ToolResultChannel& Combined(const result::SavedSnapshot& snapshot) {
    const auto& channels = snapshot.result().channels;
    const auto found = std::find_if(channels.begin(), channels.end(), [](const auto& channel) { return channel.channel == "combined"; });
    REQUIRE(found != channels.end());
    return *found;
}
std::string Body() {
    std::string body = "original start\n";
    for (int index = 0; index < 6000; ++index) body += "保存原文🙂\n";
    return body + "original end";
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.is_open());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    REQUIRE(output.good());
}
} // namespace

TEST_CASE("SDK results: original material survives event overflow, Close and same-ID resume") {
    Fixture fixture;
    const auto body = Body();
    auto models = std::make_shared<std::atomic<int>>(0);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto opened = (*runtime)->OpenSession(Options(fixture, models, tools, body));
    REQUIRE(opened.has_value());
    auto session = *opened;
    auto events = session->Subscribe(1);
    REQUIRE(events.has_value());
    const auto receipt = Turn(session);
    const auto identity = Selected(session, receipt.operation_id);
    const auto original = session->ReadToolResult(identity);
    REQUIRE(original.has_value());
    CHECK(original->result().metadata_state == result::ArtifactState::Verified);
    CHECK((original->policy() == result::SessionResultPolicy{session->id(), result::Mode::Preview, 1}));
    CHECK(identity.tool_call_id != "provider-call");
    REQUIRE(Combined(*original).text.has_value());
    CHECK(*Combined(*original).text == body);
    CHECK(Combined(*original).artifact_verified);
    CHECK(Combined(*original).capture_complete);
    const auto overflow = (*events)->Next(0ms);
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error().code == "sdk.events.overflow");
    REQUIRE(session->Close().has_value());
    const auto closed = session->ReadToolResult(identity);
    REQUIRE(closed.has_value());
    CHECK(Combined(*closed).text == Combined(*original).text);
    REQUIRE((*runtime)->Shutdown().has_value());
    auto resumed_runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(resumed_runtime.has_value());
    auto options = Options(fixture, models, tools, body);
    options.resume_session_id = identity.session_id;
    auto resumed = (*resumed_runtime)->OpenSession(std::move(options));
    REQUIRE(resumed.has_value());
    const auto restored = (*resumed)->ReadToolResult(identity);
    REQUIRE(restored.has_value());
    CHECK(Combined(*restored).text == Combined(*original).text);
    CHECK(restored->policy() == original->policy());
    const auto duplicate = (*resumed)->Submit("first", "first");
    REQUIRE(duplicate.has_value());
    CHECK(duplicate->duplicate);
    CHECK(duplicate->operation_id == receipt.operation_id);
    REQUIRE((*resumed_runtime)->Shutdown().has_value());
    CHECK(models->load() == 2);
    CHECK(tools->load() == 1);
}

TEST_CASE("SDK results: every identity component is bound to this session and operation") {
    Fixture fixture;
    auto models = std::make_shared<std::atomic<int>>(0);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto first = (*runtime)->OpenSession(Options(fixture, models, tools, "first result"));
    auto second = (*runtime)->OpenSession(Options(fixture, models, tools, "second result"));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const auto receipt = Turn(*first);
    const auto identity = Selected(*first, receipt.operation_id);
    CHECK_FALSE((*second)->ReadToolResult(identity).has_value());
    for (int index = 0; index != 6; ++index) {
        auto forged = identity;
        std::string* fields[] = {&forged.session_id, &forged.operation_id, &forged.turn_id,
            &forged.tool_call_id, &forged.persisted_event_id, &forged.result_id};
        *fields[index] = "../cwd/private-file";
        CHECK_FALSE((*first)->ReadToolResult(forged).has_value());
    }
    CHECK_FALSE((*first)->ListToolResults("../cwd/private-file").has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    CHECK(tools->load() == 1);
}

TEST_CASE("SDK results: an old frozen operation can be read while the next model call is blocked") {
    Fixture fixture;
    auto models = std::make_shared<std::atomic<int>>(0);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto gate = std::make_shared<Gate>();
    auto entered = gate->entered.get_future();
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto opened = (*runtime)->OpenSession(Options(fixture, models, tools, "stable material", gate));
    REQUIRE(opened.has_value());
    auto session = *opened;
    const auto first = Turn(session);
    const auto identity = Selected(session, first.operation_id);
    const auto next = session->Submit("second", "second");
    REQUIRE(next.has_value());
    struct Release { std::shared_ptr<Gate> gate; ~Release() { try { gate->release.set_value(); } catch (...) {} } } release{gate};
    REQUIRE(entered.wait_for(10s) == std::future_status::ready);
    const auto not_ready = session->ListToolResults(next->operation_id);
    REQUIRE_FALSE(not_ready.has_value());
    CHECK(not_ready.error().code == "sdk.result.not_ready");
    auto query = std::async(std::launch::async, [session, identity] { return session->ReadToolResult(identity); });
    const bool ready = query.wait_for(3s) == std::future_status::ready;
    CHECK(ready);
    gate->release.set_value();
    const auto old = query.get();
    REQUIRE(old.has_value());
    CHECK(Combined(*old).text == "stable material");
    const auto done = session->WaitResult(next->operation_id, 30s);
    REQUIRE(done.has_value());
    CHECK(done->state == sdk::OperationState::Succeeded);
    REQUIRE((*runtime)->Shutdown().has_value());
    CHECK(tools->load() == 2);
}

TEST_CASE("SDK results: missing, changed and wrong-sized artifacts are never returned as original text") {
    Fixture fixture;
    const std::string body = "stored bytes";
    auto models = std::make_shared<std::atomic<int>>(0);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto opened = (*runtime)->OpenSession(Options(fixture, models, tools, body));
    REQUIRE(opened.has_value());
    auto session = *opened;
    const auto identity = Selected(session, Turn(session).operation_id);
    REQUIRE(session->Close().has_value());
    const auto artifact = fixture.SessionDir(session->id()) / "artifacts" / (identity.result_id + ".combined.txt");
    Write(artifact, std::string(body.size(), 'x'));
    auto changed = session->ReadToolResult(identity);
    REQUIRE(changed.has_value());
    CHECK(Combined(*changed).state == result::ArtifactState::Corrupt);
    CHECK(Combined(*changed).issue_code == "sdk.result.hash_mismatch");
    CHECK_FALSE(Combined(*changed).artifact_verified);
    CHECK_FALSE(Combined(*changed).text.has_value());
    Write(artifact, body + "extra");
    auto size = session->ReadToolResult(identity);
    REQUIRE(size.has_value());
    CHECK(Combined(*size).issue_code == "sdk.result.bytes_mismatch");
    CHECK_FALSE(Combined(*size).text.has_value());
    fs::remove(artifact);
    auto missing = session->ReadToolResult(identity);
    REQUIRE(missing.has_value());
    CHECK(Combined(*missing).state == result::ArtifactState::Missing);
    CHECK_FALSE(Combined(*missing).text.has_value());
    Write(artifact, body);
    const auto restored = session->ReadToolResult(identity);
    REQUIRE(restored.has_value());
    CHECK(Combined(*restored).text == body);
    fs::remove(fixture.SessionDir(session->id()) / "artifacts" / (identity.result_id + ".json"));
    const auto descriptor_missing = session->ReadToolResult(identity);
    REQUIRE(descriptor_missing.has_value());
    CHECK(descriptor_missing->result().metadata_state == result::ArtifactState::Missing);
    CHECK(descriptor_missing->result().channels.empty());
    CHECK(session->ListToolResults(identity.operation_id).has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK results: text budget is shared and never returns an arbitrary shortened original") {
    Fixture fixture;
    const auto body = Body();
    auto models = std::make_shared<std::atomic<int>>(0);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto opened = (*runtime)->OpenSession(Options(fixture, models, tools, body));
    REQUIRE(opened.has_value());
    auto session = *opened;
    const auto identity = Selected(session, Turn(session).operation_id);
    auto limited = session->ReadToolResult(identity, {body.size() - 1});
    REQUIRE(limited.has_value());
    CHECK(Combined(*limited).state == result::ArtifactState::TooLarge);
    CHECK_FALSE(Combined(*limited).text.has_value());
    CHECK_FALSE(Combined(*limited).artifact_verified);
    auto entire = session->ReadToolResult(identity, {body.size()});
    REQUIRE(entire.has_value());
    CHECK(Combined(*entire).text == body);
    CHECK_FALSE(session->ReadToolResult(identity, {0}).has_value());
    CHECK_FALSE(session->ReadToolResult(identity, {8 * 1024 * 1024 + 1}).has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK results: saved mode and version are preserved on resume and cannot be elevated") {
    Fixture fixture;
    auto models = std::make_shared<std::atomic<int>>(0);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto options = Options(fixture, models, tools, "policy material");
    options.result_policy = result::SessionResultOptions{result::Mode::Preview, 7};
    auto opened = (*runtime)->OpenSession(std::move(options));
    REQUIRE(opened.has_value());
    const auto identity = Selected(*opened, Turn(*opened).operation_id);
    REQUIRE((*opened)->Close().has_value());
    for (const auto requested : {result::SessionResultOptions{result::Mode::Full, 7},
                                result::SessionResultOptions{result::Mode::Preview, 8}}) {
        auto changed = Options(fixture, models, tools, "policy material");
        changed.resume_session_id = identity.session_id;
        changed.result_policy = requested;
        const auto denied = (*runtime)->OpenSession(std::move(changed));
        REQUIRE_FALSE(denied.has_value());
        CHECK(denied.error().code == "sdk.result.policy_resume_mismatch");
    }
    auto resumed_options = Options(fixture, models, tools, "policy material");
    resumed_options.resume_session_id = identity.session_id;
    auto resumed = (*runtime)->OpenSession(std::move(resumed_options));
    REQUIRE(resumed.has_value());
    const auto snapshot = (*resumed)->ReadToolResult(identity);
    REQUIRE(snapshot.has_value());
    CHECK((snapshot->policy() == result::SessionResultPolicy{identity.session_id, result::Mode::Preview, 7}));
    REQUIRE((*runtime)->Shutdown().has_value());
    CHECK(models->load() == 2);
    CHECK(tools->load() == 1);
}

TEST_CASE("SDK results: a legacy session without a result policy resumes only as Preview v1") {
    Fixture fixture;
    auto models = std::make_shared<std::atomic<int>>(0);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto opened = (*runtime)->OpenSession(Options(fixture, models, tools, "legacy material"));
    REQUIRE(opened.has_value());
    const auto identity = Selected(*opened, Turn(*opened).operation_id);
    REQUIRE((*opened)->Close().has_value());
    fs::remove(fixture.SessionDir(identity.session_id) / "sdk-result-policy.json");
    auto elevated = Options(fixture, models, tools, "legacy material");
    elevated.resume_session_id = identity.session_id;
    elevated.result_policy = result::SessionResultOptions{result::Mode::Full, 1};
    const auto denied = (*runtime)->OpenSession(std::move(elevated));
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error().code == "sdk.result.policy_resume_mismatch");
    auto options = Options(fixture, models, tools, "legacy material");
    options.resume_session_id = identity.session_id;
    auto resumed = (*runtime)->OpenSession(std::move(options));
    REQUIRE(resumed.has_value());
    const auto snapshot = (*resumed)->ReadToolResult(identity);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->policy().mode == result::Mode::Preview);
    CHECK(snapshot->policy().version == 1);
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK results: a failed verified-ledger index is an explicit query error, never an empty list") {
    Fixture fixture;
    auto models = std::make_shared<std::atomic<int>>(0);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto corrupt = std::make_shared<LedgerCorruption>();
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto opened = (*runtime)->OpenSession(Options(fixture, models, tools, "material before ledger corruption", {}, corrupt));
    REQUIRE(opened.has_value());
    corrupt->path = fixture.SessionDir((*opened)->id()) / ((*opened)->id() + ".jsonl");
    const auto receipt = (*opened)->Submit("corrupt-index", "corrupt-index");
    REQUIRE(receipt.has_value());
    const auto completed = (*opened)->WaitResult(receipt->operation_id, 30s);
    REQUIRE(completed.has_value());
    // The corrupt main also prevents validating returned usage ownership.
    // Keep the actual counters, but never publish a confirmed success/result.
    CHECK(completed->state == sdk::OperationState::Indeterminate);
    CHECK_FALSE(completed->result_persisted);
    CHECK(completed->error.find("sdk.usage.binding_unconfirmed") != std::string::npos);
    REQUIRE(completed->usage);
    CHECK_FALSE(completed->usage->attempts_complete);
    REQUIRE(corrupt->calls.load() == 1);
    CHECK(models->load() == 2);
    REQUIRE(corrupt->bytes_before > 0);
    CHECK(corrupt->bytes_after == corrupt->bytes_before);
    std::ifstream damaged(corrupt->path, std::ios::binary);
    char prefix = 0;
    damaged.get(prefix);
    REQUIRE(damaged.good());
    REQUIRE(prefix == '!');
    const auto list = (*opened)->ListToolResults(receipt->operation_id);
    REQUIRE_FALSE(list.has_value());
    CHECK(list.error().code == "sdk.result.index_unavailable");
    // The intentionally corrupted ledger may also report a close error. Shutdown
    // still joins the worker and releases resources; that diagnostic is retained.
    (void)(*runtime)->Shutdown();
    CHECK(tools->load() == 1);
}

TEST_CASE("SDK results: zero captured text has an explicit empty channel and preserves Full policy") {
    Fixture fixture;
    auto models = std::make_shared<std::atomic<int>>(0);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto options = Options(fixture, models, tools, "");
    options.result_policy = result::SessionResultOptions{result::Mode::Full, 7};
    auto opened = (*runtime)->OpenSession(std::move(options));
    REQUIRE(opened.has_value());
    const auto identity = Selected(*opened, Turn(*opened).operation_id);
    const auto empty = (*opened)->ReadToolResult(identity);
    REQUIRE(empty.has_value());
    CHECK(empty->result().metadata_state == result::ArtifactState::Verified);
    CHECK(Combined(*empty).state == result::ArtifactState::Empty);
    CHECK(Combined(*empty).artifact_id.empty());
    CHECK(Combined(*empty).captured_bytes == 0);
    CHECK(Combined(*empty).capture_complete);
    CHECK(Combined(*empty).artifact_verified);
    REQUIRE(Combined(*empty).text.has_value());
    CHECK(Combined(*empty).text->empty());
    REQUIRE((*opened)->Close().has_value());
    auto resume = Options(fixture, models, tools, "");
    resume.resume_session_id = identity.session_id;
    auto resumed = (*runtime)->OpenSession(std::move(resume));
    REQUIRE(resumed.has_value());
    const auto restored = (*resumed)->ReadToolResult(identity);
    REQUIRE(restored.has_value());
    CHECK((restored->policy() == result::SessionResultPolicy{identity.session_id, result::Mode::Full, 7}));
    CHECK(Combined(*restored).state == result::ArtifactState::Empty);
    REQUIRE((*runtime)->Shutdown().has_value());
    CHECK(tools->load() == 1);
}

TEST_CASE("SDK results: recovery rejects two durable operations bound to the same V3 turn") {
    Fixture fixture;
    auto models = std::make_shared<std::atomic<int>>(0);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto opened = (*runtime)->OpenSession(Options(fixture, models, tools, "turn binding"));
    REQUIRE(opened.has_value());
    auto session = *opened;
    const auto first = Turn(session, "first");
    const auto first_operation = session->ReadOperation(first.operation_id);
    REQUIRE(first_operation.has_value());
    const auto second = Turn(session, "second");
    REQUIRE(session->Close().has_value());
    const auto path = fixture.SessionDir(session->id()) / "operations.jsonl";
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::string rewritten, line;
    bool changed = false;
    while (std::getline(input, line)) {
        auto row = nlohmann::json::parse(line);
        if (row.at("kind") == "operation.final" && row.at("operationId") == second.operation_id) {
            row["turnId"] = first_operation->turn_id;
            changed = true;
        }
        rewritten += row.dump() + "\n";
    }
    input.close();
    REQUIRE(changed);
    Write(path, rewritten);
    auto options = Options(fixture, models, tools, "turn binding");
    options.resume_session_id = session->id();
    const auto denied = (*runtime)->OpenSession(std::move(options));
    REQUIRE_FALSE(denied.has_value());
    CHECK(denied.error().code == "sdk.resume.operation_ledger_invalid");
    REQUIRE((*runtime)->Shutdown().has_value());
    CHECK(models->load() == 4);
    CHECK(tools->load() == 2);
}

TEST_CASE("SDK results: saved policy types and session identity are checked without numeric coercion") {
    Fixture fixture;
    auto models = std::make_shared<std::atomic<int>>(0);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto opened = (*runtime)->OpenSession(Options(fixture, models, tools, "policy type check"));
    REQUIRE(opened.has_value());
    const auto identity = Selected(*opened, Turn(*opened).operation_id);
    REQUIRE((*opened)->Close().has_value());
    const auto path = fixture.SessionDir(identity.session_id) / "sdk-result-policy.json";
    const nlohmann::json original{{"schemaVersion", 1}, {"sessionId", identity.session_id}, {"mode", "preview"}, {"version", 1}};
    for (int malformed = 0; malformed != 5; ++malformed) {
        auto policy = original;
        if (malformed == 0) policy["schemaVersion"] = 1.25;
        if (malformed == 1) policy["schemaVersion"] = true;
        if (malformed == 2) policy["version"] = 1.5;
        if (malformed == 3) policy["sessionId"] = "another-session";
        if (malformed == 4) policy["allowFullToolResults"] = true;
        Write(path, policy.dump());
        auto options = Options(fixture, models, tools, "policy type check");
        options.resume_session_id = identity.session_id;
        const auto denied = (*runtime)->OpenSession(std::move(options));
        REQUIRE_FALSE(denied.has_value());
        CHECK(denied.error().code == "sdk.result.policy_invalid");
    }
    Write(path, original.dump());
    REQUIRE((*runtime)->Shutdown().has_value());
    CHECK(models->load() == 2);
    CHECK(tools->load() == 1);
}
