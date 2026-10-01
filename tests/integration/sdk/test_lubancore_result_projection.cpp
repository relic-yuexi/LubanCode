#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include "lubancore/core.hpp"
#include "lubancore/results.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "sdk/results.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/v3/result_store.hpp"

namespace {
namespace sdk = lubancore;
namespace out = lubancore::results::v1;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Store = lubancode::trajectory::v3::ResultStore;
using namespace std::chrono_literals;

struct Fixture {
    fs::path root;
    Fixture() {
        static std::atomic<int> serial{0};
        root = fs::temp_directory_path() / ("sdk-result-projection-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "cwd");
        fs::create_directories(root / "resources");
        root = fs::canonical(root);
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    std::string Utf8(const fs::path& path) const { return lubancode::tools::PathToUtf8(path); }
    sdk::RuntimeOptions Roots() const { return {Utf8(root / "data"), Utf8(root / "resources")}; }
};
class ToolBackend final : public sdk::Backend {
public:
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        for (const auto& message : request.messages)
            if (!message.tool_replies.empty()) return sdk::ModelReply{"done", {}, std::nullopt};
        return sdk::ModelReply{"", {{"projection-call", "projection_tool", "{}"}}, std::nullopt};
    }
};
sdk::SessionOptions Options(const Fixture& fixture, std::string result,
                            std::optional<out::SessionResultOptions> policy = std::nullopt) {
    sdk::SessionOptions options;
    options.cwd = fixture.Utf8(fixture.root / "cwd");
    options.model = "projection-model";
    options.system_prompt = "Return a saved tool result.";
    options.backend = std::make_unique<ToolBackend>();
    options.result_policy = policy;
    options.max_steps_per_turn = 4;
    sdk::Tool tool;
    tool.name = "projection_tool";
    tool.description = "Return durable fixture text.";
    tool.requires_approval = false;
    tool.execute = [result = std::move(result)](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
        return sdk::ToolResult{result, false};
    };
    options.custom_tools.push_back(std::move(tool));
    return options;
}
out::SavedSnapshot Turn(const std::shared_ptr<sdk::Session>& session, std::size_t read_budget = 8 * 1024 * 1024) {
    auto receipt = session->Submit("projection-key", "run tool");
    REQUIRE(receipt.has_value());
    auto operation = session->WaitResult(receipt->operation_id, 30s);
    REQUIRE(operation.has_value());
    INFO(operation->error);
    REQUIRE(operation->state == sdk::OperationState::Succeeded);
    auto refs = session->ListToolResults(receipt->operation_id);
    REQUIRE(refs.has_value());
    const auto formal = std::find_if(refs->begin(), refs->end(), [](const auto& result) {
        return result.selected && result.identity.result_id.starts_with("res-");
    });
    REQUIRE(formal != refs->end());
    auto snapshot = session->ReadToolResult(formal->identity, {read_budget});
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->result().metadata_state == out::ArtifactState::Verified);
    return std::move(*snapshot);
}
std::shared_ptr<out::ResultProjector> Projector(const out::SavedSnapshot& snapshot,
    out::NodeResultPolicy node = {false, 4096, "node-policy-1"}, std::vector<std::string> secrets = {}) {
    auto projector = out::ResultProjector::Create(std::move(node), snapshot.policy(), std::move(secrets));
    REQUIRE(projector.has_value());
    return std::move(*projector);
}
Json Wire(const out::FrozenProjection& frozen, const std::shared_ptr<out::ResultProjector>& projector) {
    auto bytes = frozen.ForTransmission(*projector);
    REQUIRE(bytes.has_value());
    return Json::parse(*bytes);
}
Store::ChannelOutput Channel(std::string kind, std::string text) {
    Store::ChannelOutput channel;
    channel.channel = std::move(kind);
    channel.data = std::move(text);
    channel.output_bytes = channel.data.size();
    return channel;
}
// Real ResultStore files feed the private durable reader; SnapshotAccess is not
// exposed or copied here. Production uses the same reader through a V3 index.
out::SavedSnapshot Durable(const Fixture& fixture, std::vector<Store::ChannelOutput> channels,
                           out::Mode mode = out::Mode::Preview) {
    auto store = Store::Open(fixture.root / "saved");
    REQUIRE(store.has_value());
    Store::PersistRequest request;
    request.result_kind = "process";
    request.tool_call_id = "durable-call";
    request.execution_event_ref = "durable-execution";
    request.outputs = std::move(channels);
    auto saved = store->Persist(request);
    REQUIRE(saved.ok);
    sdk::detail::ToolResultIndexEntry entry;
    entry.summary = {{"durable-session", "durable-operation", "durable-turn", request.tool_call_id,
                      "durable-persisted", saved.result_id}, request.attempt, true, "durable_tool"};
    entry.execution_event_id = request.execution_event_ref;
    for (const auto& ref : saved.result_ref)
        entry.artifacts.push_back({ref.at("artifactId").get<std::string>(), ref.at("kind").get<std::string>(),
            ref.at("path").get<std::string>(), ref.at("sha256").get<std::string>(),
            ref.at("bytes").get<std::uint64_t>(), ref.at("mediaType").get<std::string>()});
    auto snapshot = sdk::detail::ReadIndexedToolResult(fixture.root / "saved", entry,
        {entry.summary.identity.session_id, mode, 1}, {8 * 1024 * 1024});
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->result().metadata_state == out::ArtifactState::Verified);
    return std::move(*snapshot);
}
} // namespace

TEST_CASE("SDK result projection: actual SDK snapshot enforces two permissions and same-session binding") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    const std::string credential = "FAKE_SAVED_PROJECTION_CREDENTIAL";
    const std::string raw = "RESULT_HEAD " + credential + std::string(8000, 'x') + " RESULT_TAIL";
    auto session = (*runtime)->OpenSession(Options(fixture, raw));
    REQUIRE(session.has_value());
    auto snapshot = Turn(*session);
    REQUIRE(snapshot.policy().mode == out::Mode::Preview);
    auto host_permitted = Projector(snapshot, {true, 128, "node-policy-1"}, {credential});
    auto preview = host_permitted->Project(snapshot);
    REQUIRE(preview.has_value());
    const auto wire = Wire(*preview, host_permitted);
    CHECK(wire["mode"] == "preview");
    CHECK(wire["text"].get<std::string>().size() <= 128);
    CHECK(wire["text"].get<std::string>().find("[REDACTED]") != std::string::npos);
    CHECK(wire.dump().find(credential) == std::string::npos);
    CHECK(wire.dump().find("RESULT_TAIL") == std::string::npos);
    auto permission_upgrade = snapshot.policy(); permission_upgrade.mode = out::Mode::Full;
    auto full_projector = out::ResultProjector::Create({true, 128, "node-policy-1"}, permission_upgrade, {credential});
    REQUIRE(full_projector.has_value());
    auto escalated = (*full_projector)->Project(snapshot);
    REQUIRE_FALSE(escalated.has_value());
    CHECK(escalated.error().code == "result_sync_session_mismatch");
    auto disabled = out::ResultProjector::Create({false, 128, "node-policy-1"}, permission_upgrade);
    REQUIRE_FALSE(disabled.has_value());
    CHECK(disabled.error().code == "full_result_sync_disabled");

    auto full_session = (*runtime)->OpenSession(Options(fixture, raw, out::SessionResultOptions{out::Mode::Full, 1}));
    REQUIRE(full_session.has_value());
    auto full_snapshot = Turn(*full_session);
    auto allowed = Projector(full_snapshot, {true, 128, "node-policy-1"}, {credential});
    auto full = allowed->Project(full_snapshot);
    REQUIRE(full.has_value());
    CHECK(Wire(*full, allowed)["mode"] == "full");
    CHECK(Wire(*full, allowed)["text"].get<std::string>().find("RESULT_TAIL") != std::string::npos);
    CHECK(Wire(*full, allowed).dump().find(credential) == std::string::npos);
    CHECK_FALSE(host_permitted->Project(full_snapshot).has_value());
    CHECK_FALSE(preview->ForTransmission(*allowed).has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK result projection: close resume restores exact frozen record and detects source or policy change") {
    Fixture fixture;
    std::string session_id, operation_id, storage, first;
    out::ToolResultIdentity identity;
    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(fixture, "RESULT_HEAD " + std::string(9000, 'x') + " RESULT_TAIL"));
        REQUIRE(session.has_value());
        auto snapshot = Turn(*session);
        identity = snapshot.result().summary.identity;
        session_id = (*session)->id(); operation_id = snapshot.operation_id();
        REQUIRE((*session)->Close().has_value());
        auto projector = Projector(snapshot, {false, 64, "node-policy-1"}, {"FAKE_RESOLVED_KEY"});
        auto frozen = projector->Project(snapshot);
        REQUIRE(frozen.has_value());
        auto serialized = frozen->SerializeForStorage(); REQUIRE(serialized.has_value()); storage = *serialized;
        auto sent = frozen->ForTransmission(*projector); REQUIRE(sent.has_value()); first = *sent;
        auto copy = first; copy[0] = '!';
        CHECK(*frozen->ForTransmission(*projector) == first);
        auto changed = Projector(snapshot, {false, 64, "node-policy-2"}, {"FAKE_RESOLVED_KEY"});
        CHECK_FALSE(frozen->ForTransmission(*changed).has_value());
        auto key_changed = Projector(snapshot, {false, 64, "node-policy-1"}, {"CHANGED_RESOLVED_KEY"});
        CHECK(projector->BindingFingerprint() != key_changed->BindingFingerprint());
        CHECK_FALSE(frozen->ForTransmission(*key_changed).has_value());
        auto reordered = Projector(snapshot, {false, 64, "node-policy-1"}, {"", "FAKE_RESOLVED_KEY", "FAKE_RESOLVED_KEY"});
        CHECK(projector->BindingFingerprint() == reordered->BindingFingerprint());
    }
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
    auto options = Options(fixture, "unused"); options.resume_session_id = session_id;
    auto resumed = (*runtime)->OpenSession(std::move(options)); REQUIRE(resumed.has_value());
    auto snapshot = (*resumed)->ReadToolResult(identity); REQUIRE(snapshot.has_value());
    auto refs = (*resumed)->ListToolResults(operation_id); REQUIRE(refs.has_value());
    CHECK(std::any_of(refs->begin(), refs->end(), [&](const auto& result) {
        return result.selected && result.identity == identity;
    }));
    auto projector = Projector(*snapshot, {false, 64, "node-policy-1"}, {"FAKE_RESOLVED_KEY"});
    auto frozen = projector->RestoreSavedProjection(storage, *snapshot); REQUIRE(frozen.has_value());
    CHECK(*frozen->ForTransmission(*projector) == first);
    CHECK(*frozen->SerializeForStorage() == storage);
    auto damaged = Json::parse(storage); damaged["native"]["payload"]["text"] = "other prefix";
    auto bad = projector->RestoreSavedProjection(damaged.dump(), *snapshot);
    REQUIRE_FALSE(bad.has_value()); CHECK(bad.error().code == "result_sync_invalid_frozen_record");
    damaged = Json::parse(storage); damaged["path"] = "../secret";
    CHECK_FALSE(projector->RestoreSavedProjection(damaged.dump(), *snapshot).has_value());
    auto peer = (*runtime)->OpenSession(Options(fixture, "peer output")); REQUIRE(peer.has_value());
    auto other = Turn(*peer);
    CHECK_FALSE(projector->RestoreSavedProjection(storage, other).has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK result projection: persisted channels share one UTF8 prefix and one whole-text redaction") {
    Fixture fixture;
    const std::string key = "FAKE_CROSS_CHANNEL_SECRET";
    auto snapshot = Durable(fixture, {Channel("stdout", std::string(4090, 'a') + key.substr(0, 10)),
                                      Channel("stderr", key.substr(10) + " HIDDEN_TAIL")});
    auto projector = Projector(snapshot, {false, 4096, "node-policy-1"}, {key});
    auto frozen = projector->Project(snapshot); REQUIRE(frozen.has_value());
    auto wire = Wire(*frozen, projector);
    CHECK(wire["text"] == std::string(4090, 'a') + "[REDAC");
    CHECK(wire["channels"].size() == 2);
    CHECK(wire.dump().find("FAKE_CROSS") == std::string::npos);
    CHECK(wire.dump().find("HIDDEN_TAIL") == std::string::npos);
    const std::string unicode = "A\xE4\xB8\xAD\xF0\x9F\x98\x80" "B";
    auto unicode_snapshot = Durable(fixture, {Channel("stderr", "ignored duplicate"), Channel("stdout", "ignored duplicate"),
                                             Channel("combined", unicode)});
    auto tiny = Projector(unicode_snapshot, {false, 7, "node-policy-1"});
    auto bounded = tiny->Project(unicode_snapshot); REQUIRE(bounded.has_value());
    const auto unicode_wire = Wire(*bounded, tiny);
    CHECK(unicode_wire["text"] == unicode.substr(0, 4));
    CHECK(unicode_wire["originalBytes"] == unicode.size());
    CHECK(lubancode::platform::IsValidUtf8(unicode_wire["text"].get<std::string>()));
}

TEST_CASE("SDK result projection: binary and incomplete durable material cannot leak through metadata") {
    Fixture fixture;
    auto image = Channel("image", "DO_NOT_EXPORT_IMAGE_OR_FILE_PATH"); image.media_type = "image/png"; image.encoding = "binary";
    auto raw = Channel("raw_payload", R"({"credential":"DO_NOT_EXPORT_RAW_JSON"})"); raw.media_type = "application/json";
    auto snapshot = Durable(fixture, {image, raw});
    auto projector = Projector(snapshot);
    auto frozen = projector->Project(snapshot); REQUIRE(frozen.has_value());
    auto wire = Wire(*frozen, projector);
    CHECK(wire["status"] == "metadata_only");
    CHECK_FALSE(wire.contains("text"));
    CHECK(wire.dump().find("DO_NOT_EXPORT") == std::string::npos);
    for (const auto& c : wire["channels"]) {
        CHECK_FALSE(c.contains("path")); CHECK_FALSE(c.contains("artifact_id"));
        CHECK_FALSE(c.contains("capture_reason")); CHECK_FALSE(c.contains("media_type"));
    }
    auto full_snapshot = Durable(fixture, {image, raw}, out::Mode::Full);
    auto full_projector = Projector(full_snapshot, {true, 4096, "node-policy-1"});
    auto refused = full_projector->Project(full_snapshot); REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == "result_not_text");
    auto partial = Channel("stdout", "PARTIAL_PRIVATE_CREDENTIAL");
    partial.capture_complete = false; partial.capture_reason = "DO_NOT_EXPORT_CAPTURE_REASON";
    partial.output_bytes += 100; partial.output_bytes_lower_bound = true;
    auto incomplete = Durable(fixture, {partial});
    auto incomplete_projector = Projector(incomplete);
    auto metadata = incomplete_projector->Project(incomplete); REQUIRE(metadata.has_value());
    auto incomplete_wire = Wire(*metadata, incomplete_projector);
    CHECK(incomplete_wire["status"] == "capture_incomplete");
    CHECK_FALSE(incomplete_wire.contains("text"));
    CHECK(incomplete_wire.dump().find("PARTIAL_PRIVATE") == std::string::npos);
    CHECK(incomplete_wire.dump().find("DO_NOT_EXPORT_CAPTURE_REASON") == std::string::npos);
    auto incomplete_full = Durable(fixture, {partial}, out::Mode::Full);
    auto stopped = Projector(incomplete_full, {true, 4096, "node-policy-1"})->Project(incomplete_full);
    REQUIRE_FALSE(stopped.has_value()); CHECK(stopped.error().code == "result_capture_incomplete");
}

TEST_CASE("SDK result projection: text read quota input cap and escaped transmission cap refuse explicitly") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(fixture, std::string(20000, 'x'))); REQUIRE(session.has_value());
    auto quota_snapshot = Turn(*session, 16);
    auto quota = Projector(quota_snapshot)->Project(quota_snapshot);
    REQUIRE_FALSE(quota.has_value()); CHECK(quota.error().code == "result_too_large");
    // A text body smaller than 1 MiB may still exceed the serialized JSON cap.
    auto escaped = Durable(fixture, {Channel("combined", std::string(600000, '\t'))}, out::Mode::Full);
    auto escaped_projection = Projector(escaped, {true, 4096, "node-policy-1"})->Project(escaped);
    REQUIRE_FALSE(escaped_projection.has_value()); CHECK(escaped_projection.error().code == "result_too_large");
    auto oversized = Durable(fixture, {Channel("combined", std::string(8 * 1024 * 1024 + 1, 'x'))});
    auto huge = Projector(oversized)->Project(oversized);
    REQUIRE_FALSE(huge.has_value()); CHECK(huge.error().code == "result_too_large");
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK result projection: closed storage schema remains strict even with recomputed corruption digest") {
    Fixture fixture;
    auto snapshot = Durable(fixture, {Channel("combined", "RESULT_HEAD frozen text")});
    auto projector = Projector(snapshot);
    auto frozen = projector->Project(snapshot); REQUIRE(frozen.has_value());
    auto storage = frozen->SerializeForStorage(); REQUIRE(storage.has_value());
    for (const auto& field : {"previewMaxBytes", "mode", "sessionPolicyVersion", "text", "captureComplete"}) {
        auto damaged = Json::parse(*storage);
        damaged["native"]["payload"][field] = Json::array();
        damaged["native"]["sha256"] = lubancode::platform::Sha256Hex(damaged["native"]["payload"].dump());
        damaged.erase("sha256"); damaged["sha256"] = lubancode::platform::Sha256Hex(damaged.dump());
        auto refused = projector->RestoreSavedProjection(damaged.dump(), snapshot);
        REQUIRE_FALSE(refused.has_value()); CHECK(refused.error().code == "result_sync_invalid_frozen_record");
    }
    auto changed = Durable(fixture, {Channel("combined", "replacement bytes")});
    CHECK_FALSE(projector->RestoreSavedProjection(*storage, changed).has_value());
    for (const std::string invalid : {"null", "{}", "{", "[]"})
        CHECK_FALSE(projector->RestoreSavedProjection(invalid, snapshot).has_value());
}
