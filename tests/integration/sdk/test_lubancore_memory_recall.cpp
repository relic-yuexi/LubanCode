#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <lubancore/core.hpp>
#include <lubancore/memory.hpp>
#include <nlohmann/json.hpp>

#include "memory/frontmatter.hpp"
#include "platform/sha256.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/v3/reader.hpp"

namespace {
namespace sdk = lubancore;
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
using namespace std::chrono_literals;
constexpr auto kNeedle = "SDKMEMNEEDLE";
constexpr auto kId = "preference.sdk-fixture";
std::string Utf8(const fs::path& path) { return lubancode::tools::PathToUtf8(path); }
void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
    out.close();
    REQUIRE_FALSE(out.fail());
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::size_t Count(const std::string& text, const std::string& needle) {
    std::size_t count = 0, offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) { ++count; offset += needle.size(); }
    return count;
}
struct Fixture {
    fs::path root;
    Fixture() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("sdk-memory-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "cwd");
        fs::create_directories(root / "resources");
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    sdk::RuntimeOptions Roots() const { return {Utf8(root / "data"), Utf8(root / "resources")}; }
};
using GenerateFunction = std::function<sdk::Result<sdk::ModelReply>(const sdk::ModelRequest&, sdk::Cancellation)>;
class Backend final : public sdk::Backend {
public:
    explicit Backend(GenerateFunction generate) : generate_(std::move(generate)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancellation) override {
        return generate_(request, cancellation);
    }
private:
    GenerateFunction generate_;
};
struct Capture {
    std::mutex mutex;
    std::vector<sdk::ModelRequest> requests;
    GenerateFunction Function() {
        return [this](const sdk::ModelRequest& request, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
            std::lock_guard lock(mutex);
            requests.push_back(request);
            return sdk::ModelReply{"memory-answer", {}, sdk::Usage{4, 3}};
        };
    }
    std::size_t Calls() { std::lock_guard lock(mutex); return requests.size(); }
    sdk::ModelRequest At(std::size_t index) { std::lock_guard lock(mutex); REQUIRE(index < requests.size()); return requests[index]; }
};
sdk::SessionOptions Options(const Fixture& fixture, GenerateFunction generate, fs::path cwd = {}) {
    sdk::SessionOptions options;
    options.cwd = Utf8(cwd.empty() ? fixture.root / "cwd" : cwd);
    options.model = "memory-model";
    options.system_prompt = "MEMORY_USER";
    options.backend = std::make_unique<Backend>(std::move(generate));
    options.memory = sdk::memory::v1::RecallOptions{};
    return options;
}
sdk::memory::v1::Snapshot Snapshot(const std::shared_ptr<sdk::Session>& session) {
    auto value = session->DescribeMemory();
    REQUIRE(value.has_value());
    return *value;
}
fs::path Memory(const std::shared_ptr<sdk::Session>& session) { return lubancode::tools::Utf8ToPath(Snapshot(session).memory_directory); }
fs::path Directory(const std::shared_ptr<sdk::Session>& session) { return Memory(session).parent_path() / "sessions" / session->id(); }
void Seed(const fs::path& memory, const std::string& marker = "MEMORY_OLD_MARKER", const std::string& status = "active",
          lubancode::memory::MemoryScope scope = {}, const std::string& expiry = {}, Json fingerprints = Json::object()) {
    lubancode::memory::MemoryEntry entry;
    entry.schema = 3; entry.id = kId; entry.name = kId; entry.title = kNeedle;
    entry.summary = std::string(kNeedle) + " project preference";
    entry.kind = lubancode::memory::MemoryKind::Preference;
    entry.status = status; entry.confidence = "user-stated"; entry.scope = std::move(scope);
    entry.expires_at = expiry; entry.keywords = {kNeedle};
    Write(memory / "preferences" / (std::string(kId) + ".md"),
        lubancode::memory::frontmatter::BuildTopicText(entry, fingerprints, std::string(kNeedle) + " " + marker));
}
Json CatalogRow() {
    return {{"schema", 3}, {"id", kId}, {"kind", "preference"}, {"file", "preferences/preference.sdk-fixture.md"},
        {"title", kNeedle}, {"summary", kNeedle}, {"status", "active"},
        {"scope", {{"level", "project"}, {"kind", "project"}, {"value", ""}}}, {"fingerprints", Json::object()}};
}
void Catalog(const fs::path& memory, Json row) { Write(memory / ".state" / "catalog.json", Json{{"entries", Json::array({row})}}.dump()); }
std::string Body(const sdk::ModelRequest& request) {
    std::string body;
    for (const auto& message : request.messages) body += message.text + "\n";
    return body;
}
sdk::Receipt Turn(const std::shared_ptr<sdk::Session>& session, const std::string& key,
                  const std::string& text = kNeedle, sdk::OperationState expected = sdk::OperationState::Succeeded) {
    auto receipt = session->Submit(key, text);
    REQUIRE(receipt.has_value());
    auto operation = session->WaitResult(receipt->operation_id, 30s);
    REQUIRE(operation.has_value());
    INFO(operation->error);
    CHECK(operation->state == expected);
    CHECK(operation->result_persisted == (expected != sdk::OperationState::Indeterminate));
    return *receipt;
}
sdk::memory::v1::RecallReport Report(const std::shared_ptr<sdk::Session>& session, const sdk::Receipt& receipt) {
    auto report = session->GetMemoryRecall(receipt.operation_id);
    REQUIRE(report.has_value());
    CHECK(report->session_id == session->id());
    CHECK(report->operation_id == receipt.operation_id);
    return *report;
}
bool SameReport(const sdk::memory::v1::RecallReport& actual, const sdk::memory::v1::RecallReport& expected) {
    return actual.enabled == expected.enabled && actual.session_id == expected.session_id &&
        actual.operation_id == expected.operation_id && actual.turn_id == expected.turn_id &&
        actual.workspace_key == expected.workspace_key && actual.plan_sha256 == expected.plan_sha256 &&
        actual.state == expected.state && actual.error == expected.error &&
        actual.context_message_id == expected.context_message_id && actual.context_sha256 == expected.context_sha256 &&
        actual.bytes == expected.bytes && actual.entries.size() == expected.entries.size() &&
        std::equal(actual.entries.begin(), actual.entries.end(), expected.entries.begin(), [](const auto& a, const auto& b) {
            return a.id == b.id && a.score == b.score && a.selected == b.selected && a.stale == b.stale &&
                a.expired == b.expired && a.scope_blocked == b.scope_blocked && a.budget_dropped == b.budget_dropped &&
                a.below_threshold == b.below_threshold && a.weak == b.weak && a.reason == b.reason && a.bytes == b.bytes;
        });
}
void RewriteReport(const fs::path& path, const std::function<void(Json&)>& change) {
    auto envelope = Json::parse(Read(path));
    change(envelope["payload"]);
    envelope["sha256"] = lubancode::platform::Sha256Hex(envelope["payload"].dump());
    Write(path, envelope.dump());
}
void RemoveFinal(const fs::path& directory) {
    std::string rewritten;
    std::ifstream stream(directory / "operations.jsonl");
    std::string line;
    while (std::getline(stream, line)) {
        const auto fact = Json::parse(line);
        if (fact.value("kind", "") != "operation.final") rewritten += line + "\n";
    }
    stream.close();
    Write(directory / "operations.jsonl", rewritten);
}
} // namespace

TEST_CASE("SDK Memory: omitted capability never reads the project corpus") {
    Fixture fixture; Capture capture;
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
    auto options = Options(fixture, capture.Function()); options.memory.reset();
    auto session = (*runtime)->OpenSession(std::move(options)); REQUIRE(session.has_value());
    Write(Memory(*session), "bad root is never read");
    const auto receipt = Turn(*session, "disabled");
    CHECK_FALSE(Snapshot(*session).enabled);
    CHECK(Report(*session, receipt).state == "disabled");
    CHECK(capture.Calls() == 1);
    CHECK_FALSE(fs::exists(Directory(*session) / "sdk-memory-plan.json"));
    CHECK_FALSE(fs::exists(Directory(*session) / "sdk-memory-recalls"));
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK Memory: real recall adopts the complete input once and verifies fragment blobs") {
    Fixture fixture; Capture capture;
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(fixture, capture.Function())); REQUIRE(session.has_value());
    Seed(Memory(*session), "MEMORY_BLOB_MARKER " + std::string(800, 'x'));
    const auto receipt = Turn(*session, "adopted");
    const auto report = Report(*session, receipt);
    REQUIRE(report.state == "admitted"); REQUIRE(report.entries.size() == 1); CHECK(report.entries[0].selected);
    CHECK(Count(Body(capture.At(0)), "MEMORY_BLOB_MARKER") == 1);
    auto ledger = v3::ReadV3Ledger(Directory(*session) / ((*session)->id() + ".jsonl")); REQUIRE(ledger.has_value());
    const auto* context = ledger->FindMessage(report.context_message_id); REQUIRE(context != nullptr);
    const auto text = context->message.at("content").get<std::string>();
    const auto request = capture.At(0);
    CHECK(std::count_if(request.messages.begin(), request.messages.end(), [&](const auto& message) {
        return message.role == "user" && message.text == text;
    }) == 1);
    CHECK(text.size() == report.bytes); CHECK(lubancode::platform::Sha256Hex(text) == report.context_sha256);
    CHECK(context->origin == v3::MessageOrigin::ContextRuntime); CHECK(context->display == v3::DisplayMode::Hidden);
    int facts = 0, prepared = 0;
    for (const auto& event : ledger->events) {
        if (event.kind == v3::EventKindV3::MemoryRecallInjected) {
            ++facts; CHECK(event.payload.at("contextMessageRef").get<std::string>() == report.context_message_id);
            REQUIRE(event.payload.contains("snapshotRef")); CHECK_FALSE(event.payload.contains("snapshotInline"));
            const auto fragment = Read(Directory(*session) / lubancode::tools::Utf8ToPath(event.payload.at("snapshotRef").get<std::string>()));
            CHECK(fragment.size() == event.payload.at("injectedBytes").get<std::size_t>());
            CHECK(lubancode::platform::Sha256Hex(fragment) == event.payload.at("contentSha256").get<std::string>());
        }
        if (event.kind == v3::EventKindV3::ModelRequestPrepared) {
            ++prepared; CHECK(v3::CheckPreparedAgainstChain(*ledger, event.event_id).empty());
            const auto& refs = event.payload.at("inputMessageRefs");
            CHECK(std::count(refs.begin(), refs.end(), Json(report.context_message_id)) == 1);
        }
    }
    CHECK(facts == 1); CHECK(prepared == 1);
    auto duplicate = (*session)->Submit("adopted", kNeedle); REQUIRE(duplicate.has_value()); CHECK(duplicate->duplicate);
    CHECK(duplicate->operation_id == receipt.operation_id); CHECK(capture.Calls() == 1);
    REQUIRE((*session)->Close().has_value()); CHECK(Report(*session, receipt).context_sha256 == report.context_sha256);
    CHECK_FALSE((*session)->GetMemoryRecall("unknown").has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK Memory: absent corpus and no match return owned empty reports without shared writes") {
    Fixture fixture; Capture capture;
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(fixture, capture.Function())); REQUIRE(session.has_value());
    auto empty = Report(*session, Turn(*session, "absent")); CHECK(empty.state == "no_match"); CHECK(empty.bytes == 0);
    Seed(Memory(*session));
    auto no_match = Report(*session, Turn(*session, "unrelated", "ZXQUNRELATED987"));
    CHECK(no_match.state == "no_match"); CHECK(no_match.context_message_id.empty());
    CHECK(Body(capture.At(1)).find("MEMORY_OLD_MARKER") == std::string::npos);
    CHECK_FALSE(fs::exists(Memory(*session) / ".state" / "catalog.json"));
    CHECK_FALSE(fs::exists(Memory(*session) / ".state" / "recall-traces" / "trace-last.json"));
    CHECK_FALSE(fs::exists(Memory(*session) / ".queue"));
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK Memory: actual topic scope overrides stale project catalog policy") {
    for (const bool user_scope : {false, true}) {
        Fixture fixture; Capture capture;
        auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(fixture, capture.Function())); REQUIRE(session.has_value());
        Catalog(Memory(*session), CatalogRow());
        lubancode::memory::MemoryScope scope;
        scope.level = user_scope ? "user" : "project"; scope.kind = user_scope ? "user" : "subtree";
        scope.value = user_scope ? "" : "elsewhere";
        Seed(Memory(*session), "BLOCKED_SCOPE_MARKER", "active", scope);
        const auto receipt = Turn(*session, "scope", kNeedle, user_scope ? sdk::OperationState::Failed : sdk::OperationState::Succeeded);
        if (user_scope) { CHECK(Report(*session, receipt).state == "failed"); CHECK(capture.Calls() == 0); }
        else {
            const auto report = Report(*session, receipt); REQUIRE(report.entries.size() == 1); CHECK(report.entries[0].scope_blocked);
            CHECK(Body(capture.At(0)).find("BLOCKED_SCOPE_MARKER") == std::string::npos);
        }
        REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK Memory: actual archive and expiry override stale active catalog metadata") {
    for (const bool expired : {false, true}) {
        Fixture fixture; Capture capture;
        auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(fixture, capture.Function())); REQUIRE(session.has_value());
        Catalog(Memory(*session), CatalogRow());
        Seed(Memory(*session), "BLOCKED_DATE_MARKER", expired ? "active" : "archived", {}, expired ? "2000-01-01" : "");
        const auto report = Report(*session, Turn(*session, "date")); CHECK(report.state == "no_match");
        CHECK(Body(capture.At(0)).find("BLOCKED_DATE_MARKER") == std::string::npos);
        if (expired) { REQUIRE(report.entries.size() == 1); CHECK(report.entries[0].expired); }
        REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK Memory: raw JSON and YAML policy types cannot fall through legacy defaults") {
    const std::vector<std::pair<std::string, Json>> bad = {{"scope", 7}, {"scope", Json::array()},
        {"fingerprints", Json::array()}, {"expires_at", 7}, {"evidence", Json::array({7})},
        {"scope", {{"level", ""}}}, {"status", ""}, {"confidence", ""}};
    for (const bool legacy_topic : {false, true}) for (const auto& [field, value] : bad) {
        Fixture fixture; Capture capture;
        auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(fixture, capture.Function())); REQUIRE(session.has_value());
        Seed(Memory(*session)); auto row = CatalogRow(); row[field] = value;
        if (legacy_topic) Write(Memory(*session) / "preferences" / "preference.sdk-fixture.md",
            "<!-- lubancode-memory\n" + row.dump() + "\n-->\n# SDKMEMNEEDLE\nSDKMEMNEEDLE BAD_POLICY_BODY");
        else Catalog(Memory(*session), row);
        const auto report = Report(*session, Turn(*session, "bad-type", kNeedle, sdk::OperationState::Failed));
        CHECK(report.state == "failed"); CHECK(report.error.starts_with("sdk.memory.read_failed")); CHECK(capture.Calls() == 0);
        REQUIRE((*runtime)->Shutdown().has_value());
    }
    for (const auto& policy : {"scope: [7]", "fingerprints: []", "expires: 123", "evidence: [7]", "status: ''", "confidence: ''", "scope: {level: ''}"}) {
        Fixture fixture; Capture capture;
        auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(fixture, capture.Function())); REQUIRE(session.has_value());
        Write(Memory(*session) / "preferences" / "preference.sdk-fixture.md",
            "---\nname: fixture\ndescription: SDKMEMNEEDLE\nmetadata:\n  schema: 3\n  type: preference\n  id: preference.sdk-fixture\n  " +
            std::string(policy) + "\n---\n# SDKMEMNEEDLE\nSDKMEMNEEDLE BAD_YAML_BODY\n");
        CHECK(Report(*session, Turn(*session, "yaml-type", kNeedle, sdk::OperationState::Failed)).state == "failed");
        CHECK(capture.Calls() == 0); REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK Memory: invalid text and hard read caps fail before model invocation") {
    for (int variant = 0; variant < 5; ++variant) {
        Fixture fixture; Capture capture;
        auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(fixture, capture.Function())); REQUIRE(session.has_value());
        Seed(Memory(*session)); const auto topic = Memory(*session) / "preferences" / "preference.sdk-fixture.md";
        if (variant == 0) Write(topic, Read(topic) + std::string(1, '\0'));
        if (variant == 1) Write(topic, Read(topic) + std::string(1, static_cast<char>(0xff)));
        if (variant == 2) Write(topic, Read(topic) + std::string(16384, 'x'));
        if (variant == 3) Write(Memory(*session) / ".state" / "catalog.json", std::string(4194305, ' '));
        if (variant == 4) {
            Write(fixture.root / "cwd" / "proof.txt", std::string(16777217, 'x'));
            Seed(Memory(*session), "OVER_CAP_EVIDENCE", "active", {}, "", {{"proof.txt", "fnv1a64:0000000000000000"}});
        }
        CHECK(Report(*session, Turn(*session, "invalid-file", kNeedle, sdk::OperationState::Failed)).state == "failed");
        CHECK(capture.Calls() == 0); REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK Memory: corrupt directories and dangling catalog links never become empty corpora") {
    for (int variant = 0; variant < 3; ++variant) {
        Fixture fixture; Capture capture;
        auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(fixture, capture.Function())); REQUIRE(session.has_value());
        const auto memory = Memory(*session);
        if (variant == 0) Write(memory / ".state", "not a directory");
        if (variant == 1) fs::create_directories(memory / ".state" / "catalog.json");
        if (variant == 2) {
            fs::create_directories(memory); std::error_code ec;
            fs::create_directory_symlink(fixture.root / "missing", memory / ".state", ec);
            if (ec) { INFO("symlink creation unavailable; directory corruption is still tested"); continue; }
        }
        CHECK(Report(*session, Turn(*session, "corrupt-path", kNeedle, sdk::OperationState::Failed)).state == "failed");
        CHECK(capture.Calls() == 0); REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK Memory: invalid options reject opening before external resources") {
    Fixture fixture; Capture capture;
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
    for (const auto& value : {sdk::memory::v1::RecallOptions{0, 3}, {65537, 3}, {8192, 0}, {8192, 33}}) {
        auto options = Options(fixture, capture.Function()); options.memory = value;
        sdk::McpServer mcp; mcp.name = "never-start"; mcp.command = Utf8(fixture.root / "missing-executable");
        options.mcp_servers.push_back(mcp);
        auto session = (*runtime)->OpenSession(std::move(options)); REQUIRE_FALSE(session.has_value());
        CHECK(session.error().code == "sdk.memory.invalid_options");
    }
    CHECK(capture.Calls() == 0); REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK Memory: same-ID resume preserves plans reports and immutable prior request snapshots") {
    Fixture fixture; Capture capture;
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(fixture, capture.Function())); REQUIRE(session.has_value());
    const auto snapshot = Snapshot(*session); const auto directory = Directory(*session); const auto memory = Memory(*session);
    Seed(memory); const auto old_receipt = Turn(*session, "old"); const auto old_report = Report(*session, old_receipt);
    REQUIRE((*session)->Close().has_value()); Seed(memory, "MEMORY_LIVE_MARKER");
    auto options = Options(fixture, capture.Function()); options.memory.reset(); options.resume_session_id = snapshot.session_id; options.system_prompt.clear();
    auto resumed = (*runtime)->OpenSession(std::move(options)); REQUIRE(resumed.has_value());
    CHECK(Snapshot(*resumed).plan_sha256 == snapshot.plan_sha256); CHECK(Snapshot(*resumed).enabled);
    CHECK(Report(*resumed, old_receipt).context_sha256 == old_report.context_sha256); CHECK(capture.Calls() == 1);
    Turn(*resumed, "new"); CHECK(capture.Calls() == 2);
    CHECK(Count(Body(capture.At(1)), "MEMORY_OLD_MARKER") == 1); CHECK(Count(Body(capture.At(1)), "MEMORY_LIVE_MARKER") == 1);
    auto duplicate = (*resumed)->Submit("old", kNeedle); REQUIRE(duplicate.has_value()); CHECK(duplicate->duplicate); CHECK(capture.Calls() == 2);
    REQUIRE((*resumed)->Close().has_value());
    auto bad = Options(fixture, capture.Function()); bad.resume_session_id = snapshot.session_id; bad.memory->max_results = 4;
    auto mismatch = (*runtime)->OpenSession(std::move(bad)); REQUIRE_FALSE(mismatch.has_value()); CHECK(mismatch.error().code == "sdk.memory.resume_mismatch");
    CHECK(Read(directory / "sdk-memory-plan.json").find(snapshot.plan_sha256) == std::string::npos);
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK Memory: complete finals cannot claim missing forged downgraded or foreign reports") {
    for (int variant = 0; variant < 9; ++variant) {
        Fixture fixture; Capture capture;
        auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(fixture, capture.Function())); REQUIRE(session.has_value());
        Seed(Memory(*session)); const auto receipt = Turn(*session, "report"); const auto id = (*session)->id();
        const auto file = Directory(*session) / "sdk-memory-recalls" / (receipt.operation_id + ".json");
        REQUIRE((*session)->Close().has_value());
        if (variant == 0) fs::remove(file);
        else RewriteReport(file, [&](Json& report) {
            if (variant == 1) report["contextMessageId"] = "missing-message";
            if (variant == 2) report["contextSha256"] = std::string(64, '0');
            if (variant == 3) report["bytes"] = 1;
            if (variant == 4 || variant == 5) {
                report["state"] = variant == 4 ? "no_match" : "failed"; report["error"] = "forged downgrade";
                report["contextMessageId"] = ""; report["contextSha256"] = ""; report["bytes"] = 0;
                for (auto& entry : report["entries"]) entry["selected"] = false;
            }
            if (variant == 6) report["entries"][0]["reason"] = std::string(1, '\0');
            if (variant == 7) report["sessionId"] = "other-session";
            if (variant == 8) report["turnId"] = "other-turn";
        });
        auto options = Options(fixture, capture.Function()); options.resume_session_id = id; options.memory.reset(); options.system_prompt.clear();
        auto resumed = (*runtime)->OpenSession(std::move(options)); REQUIRE_FALSE(resumed.has_value());
        CHECK(resumed.error().code.starts_with("sdk.memory.")); CHECK(capture.Calls() == 1);
        REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK Memory: stale evidence blocks payload and corrupt fragment blobs cannot reach the model") {
    Fixture fixture; Capture capture;
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(fixture, capture.Function())); REQUIRE(session.has_value());
    Write(fixture.root / "cwd" / "proof.txt", "changed");
    Seed(Memory(*session), "STALE_PAYLOAD_MARKER", "active", {}, "", {{"proof.txt", "fnv1a64:0000000000000000"}});
    const auto stale = Report(*session, Turn(*session, "stale")); REQUIRE(stale.entries.size() == 1); CHECK(stale.entries[0].stale); CHECK_FALSE(stale.entries[0].selected);
    CHECK(Body(capture.At(0)).find("STALE_PAYLOAD_MARKER") == std::string::npos);
    Seed(Memory(*session), "LARGE_FRAGMENT_MARKER " + std::string(800, 'x'));
    const auto receipt = Turn(*session, "blob"); const auto report = Report(*session, receipt);
    auto ledger = v3::ReadV3Ledger(Directory(*session) / ((*session)->id() + ".jsonl")); REQUIRE(ledger.has_value());
    const auto* admitted = ledger->FindMessage(report.context_message_id); REQUIRE(admitted != nullptr);
    const auto admitted_text = admitted->message.at("content").get<std::string>();
    std::string relative;
    for (const auto& event : ledger->events) if (event.kind == v3::EventKindV3::MemoryRecallInjected && event.turn_id == report.turn_id)
        relative = event.payload.at("snapshotRef").get<std::string>();
    REQUIRE_FALSE(relative.empty()); Write(Directory(*session) / lubancode::tools::Utf8ToPath(relative), "corrupt");
    const auto calls_before_failure = capture.Calls();
    const auto bad_receipt = Turn(*session, "existing-bad-blob", kNeedle, sdk::OperationState::Indeterminate);
    const auto bad = Report(*session, bad_receipt);
    CHECK(bad.state == "failed"); CHECK(bad.error.starts_with("memory.recall.snapshot_failed:"));
    CHECK(bad.context_message_id.empty()); CHECK(capture.Calls() == calls_before_failure); CHECK(calls_before_failure == 2);
    auto operation = (*session)->ReadOperation(bad_receipt.operation_id); REQUIRE(operation.has_value());
    REQUIRE_FALSE(operation->turn_id.empty()); CHECK(bad.turn_id == operation->turn_id);
    CHECK(operation->state == sdk::OperationState::Indeterminate); CHECK_FALSE(operation->result_persisted);
    auto after = v3::ReadV3Ledger(Directory(*session) / ((*session)->id() + ".jsonl")); REQUIRE(after.has_value());
    CHECK(std::none_of(after->messages.begin(), after->messages.end(), [&](const auto& message) {
        return message.turn_id == operation->turn_id && message.origin == v3::MessageOrigin::ContextRuntime;
    }));
    CHECK(std::none_of(after->events.begin(), after->events.end(), [&](const auto& event) {
        return event.turn_id == operation->turn_id &&
            (event.kind == v3::EventKindV3::MemoryRecallInjected || event.kind == v3::EventKindV3::ModelRequestPrepared);
    }));
    const auto* kept = after->FindMessage(report.context_message_id); REQUIRE(kept != nullptr);
    CHECK(kept->message.at("content").get<std::string>() == admitted_text);
    CHECK(lubancode::platform::Sha256Hex(admitted_text) == report.context_sha256);
    CHECK(std::count_if(after->events.begin(), after->events.end(), [&](const auto& event) {
        return event.turn_id == report.turn_id && event.kind == v3::EventKindV3::MemoryRecallInjected;
    }) == 1);
    CHECK(SameReport(Report(*session, receipt), report));
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK Memory: dispatch report and durable-final crash windows preserve honest indeterminate results") {
    for (int variant = 0; variant < 3; ++variant) {
        Fixture fixture; Capture capture;
        auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(fixture, capture.Function())); REQUIRE(session.has_value());
        const auto id = (*session)->id(); const auto directory = Directory(*session);
        if (variant == 2) Write(directory / "sdk-memory-recalls", "cannot persist a report");
        else Seed(Memory(*session));
        const auto receipt = Turn(*session, "crash", kNeedle, variant == 2 ? sdk::OperationState::Indeterminate : sdk::OperationState::Succeeded);
        REQUIRE((*session)->Close().has_value());
        if (variant != 2) RemoveFinal(directory);
        if (variant == 2) fs::remove(directory / "sdk-memory-recalls");
        if (variant == 0) fs::remove(directory / "sdk-memory-recalls" / (receipt.operation_id + ".json"));
        auto options = Options(fixture, capture.Function()); options.resume_session_id = id; options.memory.reset(); options.system_prompt.clear();
        auto resumed = (*runtime)->OpenSession(std::move(options)); REQUIRE(resumed.has_value());
        auto operation = (*resumed)->ReadOperation(receipt.operation_id); REQUIRE(operation.has_value());
        CHECK(operation->state == sdk::OperationState::Indeterminate); CHECK_FALSE(operation->result_persisted);
        auto report = (*resumed)->GetMemoryRecall(receipt.operation_id);
        CHECK(report.has_value() == (variant == 1));
        auto duplicate = (*resumed)->Submit("crash", kNeedle); REQUIRE(duplicate.has_value()); CHECK(duplicate->duplicate);
        CHECK(capture.Calls() == (variant == 2 ? 0 : 1)); REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK Memory: four overlapping sessions isolate recall cancellation and Skills bindings") {
    Fixture fixture;
    const auto process_cwd = fs::current_path();
    struct Gate { std::mutex mutex; std::condition_variable cv; int entered = 0; bool release = false; };
    auto gate = std::make_shared<Gate>();
    std::array<Capture, 4> captures;
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
    std::array<std::shared_ptr<sdk::Session>, 4> sessions;
    struct Release { std::shared_ptr<Gate> gate; ~Release() { std::lock_guard lock(gate->mutex); gate->release = true; gate->cv.notify_all(); } } release{gate};
    std::array<sdk::Receipt, 4> receipts;
    Write(fixture.root / "skills" / "paint" / "SKILL.md", "---\nname: paint\ndescription: fixture-paint\n---\nSKILL_BODY\n");
    for (int i = 0; i < 4; ++i) {
        const auto cwd = i < 2 ? fixture.root / "cwd" : fixture.root / ("project-" + std::to_string(i)); fs::create_directories(cwd);
        auto options = Options(fixture, [gate, &captures, i](const sdk::ModelRequest& request, sdk::Cancellation cancel) -> sdk::Result<sdk::ModelReply> {
            { std::lock_guard lock(captures[i].mutex); captures[i].requests.push_back(request); }
            std::unique_lock lock(gate->mutex); ++gate->entered; gate->cv.notify_all();
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            while (!gate->release && !cancel.requested() && std::chrono::steady_clock::now() < deadline) gate->cv.wait_for(lock, 5ms);
            if (cancel.requested()) return std::unexpected(sdk::Error{"fixture.cancelled", "cancel observed"});
            return sdk::ModelReply{"memory-answer", {}, sdk::Usage{4, 3}};
        }, cwd);
        options.model = "memory-model-" + std::to_string(i);
        options.system_prompt = "MEMORY_USER_" + std::to_string(i);
        if (i == 3) options.skills = sdk::skills::v1::Selection{Utf8(fixture.root / "skills"), {"paint"}};
        auto session = (*runtime)->OpenSession(std::move(options)); REQUIRE(session.has_value()); sessions[i] = *session;
        if (i != 1) Seed(Memory(sessions[i]), i < 2 ? "SAME_PROJECT_MARKER" : "PROJECT_" + std::to_string(i) + "_MARKER");
        auto receipt = sessions[i]->Submit("overlap", kNeedle); REQUIRE(receipt.has_value()); receipts[i] = *receipt;
    }
    { std::unique_lock lock(gate->mutex); REQUIRE(gate->cv.wait_for(lock, 10s, [&] { return gate->entered == 4; })); }
    CHECK(Memory(sessions[0]) == Memory(sessions[1])); CHECK(Memory(sessions[2]) != Memory(sessions[3]));
    REQUIRE(sessions[0]->Cancel(receipts[0].operation_id).has_value()); REQUIRE(sessions[1]->Close().has_value());
    { std::lock_guard lock(gate->mutex); gate->release = true; gate->cv.notify_all(); }
    for (int i = 0; i < 4; ++i) {
        auto operation = sessions[i]->WaitResult(receipts[i].operation_id, 30s); REQUIRE(operation.has_value());
        CHECK(operation->result_persisted); CHECK(operation->state == (i < 2 ? sdk::OperationState::Cancelled : sdk::OperationState::Succeeded));
        const auto own_report = Report(sessions[i], receipts[i]);
        CHECK(own_report.state == "admitted");
        const auto own_plan = Snapshot(sessions[i]);
        CHECK(own_report.workspace_key == own_plan.workspace_key); CHECK(own_report.plan_sha256 == own_plan.plan_sha256);
        const auto request = captures[i].At(0);
        const auto body = Body(request);
        const std::string own_marker = i < 2 ? "SAME_PROJECT_MARKER" : "PROJECT_" + std::to_string(i) + "_MARKER";
        for (const auto* marker : {"SAME_PROJECT_MARKER", "PROJECT_2_MARKER", "PROJECT_3_MARKER"})
            CHECK(Count(body, marker) == (own_marker == marker ? 1 : 0));
        CHECK(captures[i].Calls() == 1);
        CHECK(request.model == "memory-model-" + std::to_string(i));
        for (int other = 0; other < 4; ++other)
            CHECK((request.system.find("MEMORY_USER_" + std::to_string(other)) != std::string::npos) == (other == i));
        // operation_id is local to each Session. Another Session may use the
        // same string, which must still resolve to this Session's owned value.
        const auto& other_id = receipts[(i + 1) % 4].operation_id;
        const auto from_other_string = sessions[i]->GetMemoryRecall(other_id);
        if (other_id == receipts[i].operation_id) {
            REQUIRE(from_other_string.has_value());
            CHECK(from_other_string->session_id == sessions[i]->id());
            CHECK(SameReport(*from_other_string, own_report));
        } else CHECK_FALSE(from_other_string.has_value());
        CHECK(sessions[i]->PendingApprovals().empty());
    }
    auto ledger = v3::ReadV3Ledger(Directory(sessions[3]) / (sessions[3]->id() + ".jsonl")); REQUIRE(ledger.has_value());
    REQUIRE_FALSE(ledger->messages.empty()); const auto& bindings = ledger->messages.front().system_meta->at("hostBindings");
    CHECK(bindings.contains("skills")); CHECK(bindings.contains("memory"));
    CHECK(captures[3].At(0).system.find("fixture-paint") != std::string::npos);
    const auto request = captures[3].At(0);
    CHECK(std::none_of(request.tools.begin(), request.tools.end(), [](const auto& tool) { return tool.name == "memory_save"; }));
    const auto second = Turn(sessions[2], "second-operation");
    CHECK(second.operation_id != receipts[2].operation_id);
    const auto second_report = Report(sessions[2], second);
    CHECK(second_report.state == "admitted");
    CHECK(second_report.workspace_key == Snapshot(sessions[2]).workspace_key);
    CHECK(second_report.plan_sha256 == Snapshot(sessions[2]).plan_sha256);
    CHECK(captures[2].Calls() == 2);
    for (const int i : {0, 1, 3}) {
        CHECK_FALSE(sessions[i]->ReadOperation(second.operation_id).has_value());
        CHECK_FALSE(sessions[i]->GetMemoryRecall(second.operation_id).has_value());
        CHECK(captures[i].Calls() == 1);
    }
    CHECK(fs::current_path() == process_cwd);
    REQUIRE((*runtime)->Shutdown().has_value());
}
